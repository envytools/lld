//===- SymbolTable.cpp ----------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Symbol table is a bag of all known symbols. We put all symbols of
// all input files to the symbol table. The symbol table is basically
// a hash table with the logic to resolve symbol name conflicts using
// the symbol types.
//
//===----------------------------------------------------------------------===//

#include "SymbolTable.h"
#include "Config.h"
#include "Error.h"
#include "LinkerScript.h"
#include "Symbols.h"
#include "llvm/Bitcode/ReaderWriter.h"
#include "llvm/Support/StringSaver.h"

using namespace llvm;
using namespace llvm::object;
using namespace llvm::ELF;

using namespace lld;
using namespace lld::elf;

// All input object files must be for the same architecture
// (e.g. it does not make sense to link x86 object files with
// MIPS object files.) This function checks for that error.
template <class ELFT> static bool isCompatible(InputFile *FileP) {
  auto *F = dyn_cast<ELFFileBase<ELFT>>(FileP);
  if (!F)
    return true;
  if (F->EKind == Config->EKind && F->EMachine == Config->EMachine)
    return true;
  StringRef A = F->getName();
  StringRef B = Config->Emulation;
  if (B.empty())
    B = Config->FirstElf->getName();
  error(A + " is incompatible with " + B);
  return false;
}

// Add symbols in File to the symbol table.
template <class ELFT>
void SymbolTable<ELFT>::addFile(std::unique_ptr<InputFile> File) {
  InputFile *FileP = File.get();
  if (!isCompatible<ELFT>(FileP))
    return;

  // .a file
  if (auto *F = dyn_cast<ArchiveFile>(FileP)) {
    ArchiveFiles.emplace_back(cast<ArchiveFile>(File.release()));
    F->parse<ELFT>();
    return;
  }

  // Lazy object file
  if (auto *F = dyn_cast<LazyObjectFile>(FileP)) {
    LazyObjectFiles.emplace_back(cast<LazyObjectFile>(File.release()));
    F->parse<ELFT>();
    return;
  }

  if (Config->Trace)
    llvm::outs() << getFilename(FileP) << "\n";

  // .so file
  if (auto *F = dyn_cast<SharedFile<ELFT>>(FileP)) {
    // DSOs are uniquified not by filename but by soname.
    F->parseSoName();
    if (!SoNames.insert(F->getSoName()).second)
      return;

    SharedFiles.emplace_back(cast<SharedFile<ELFT>>(File.release()));
    F->parseRest();
    return;
  }

  // LLVM bitcode file
  if (auto *F = dyn_cast<BitcodeFile>(FileP)) {
    BitcodeFiles.emplace_back(cast<BitcodeFile>(File.release()));
    F->parse<ELFT>(ComdatGroups);
    return;
  }

  // Regular object file
  auto *F = cast<ObjectFile<ELFT>>(FileP);
  ObjectFiles.emplace_back(cast<ObjectFile<ELFT>>(File.release()));
  F->parse(ComdatGroups);
}

// This function is where all the optimizations of link-time
// optimization happens. When LTO is in use, some input files are
// not in native object file format but in the LLVM bitcode format.
// This function compiles bitcode files into a few big native files
// using LLVM functions and replaces bitcode symbols with the results.
// Because all bitcode files that consist of a program are passed
// to the compiler at once, it can do whole-program optimization.
template <class ELFT> void SymbolTable<ELFT>::addCombinedLtoObject() {
  if (BitcodeFiles.empty())
    return;

  // Compile bitcode files.
  Lto.reset(new BitcodeCompiler);
  for (const std::unique_ptr<BitcodeFile> &F : BitcodeFiles)
    Lto->add(*F);
  std::vector<std::unique_ptr<InputFile>> IFs = Lto->compile();

  // Replace bitcode symbols.
  for (auto &IF : IFs) {
    ObjectFile<ELFT> *Obj = cast<ObjectFile<ELFT>>(IF.release());

    llvm::DenseSet<StringRef> DummyGroups;
    Obj->parse(DummyGroups);
    ObjectFiles.emplace_back(Obj);
  }
}

template <class ELFT>
DefinedRegular<ELFT> *SymbolTable<ELFT>::addAbsolute(StringRef Name,
                                                     uint8_t Visibility) {
  return cast<DefinedRegular<ELFT>>(
      addRegular(Name, STB_GLOBAL, Visibility)->body());
}

// Add Name as an "ignored" symbol. An ignored symbol is a regular
// linker-synthesized defined symbol, but is only defined if needed.
template <class ELFT>
DefinedRegular<ELFT> *SymbolTable<ELFT>::addIgnored(StringRef Name,
                                                    uint8_t Visibility) {
  if (!find(Name))
    return nullptr;
  return addAbsolute(Name, Visibility);
}

// Rename SYM as __wrap_SYM. The original symbol is preserved as __real_SYM.
// Used to implement --wrap.
template <class ELFT> void SymbolTable<ELFT>::wrap(StringRef Name) {
  SymbolBody *B = find(Name);
  if (!B)
    return;
  StringSaver Saver(Alloc);
  Symbol *Sym = B->symbol();
  Symbol *Real = addUndefined(Saver.save("__real_" + Name));
  Symbol *Wrap = addUndefined(Saver.save("__wrap_" + Name));
  // We rename symbols by replacing the old symbol's SymbolBody with the new
  // symbol's SymbolBody. This causes all SymbolBody pointers referring to the
  // old symbol to instead refer to the new symbol.
  memcpy(Real->Body.buffer, Sym->Body.buffer, sizeof(Sym->Body));
  memcpy(Sym->Body.buffer, Wrap->Body.buffer, sizeof(Wrap->Body));
}

static uint8_t getMinVisibility(uint8_t VA, uint8_t VB) {
  if (VA == STV_DEFAULT)
    return VB;
  if (VB == STV_DEFAULT)
    return VA;
  return std::min(VA, VB);
}

// A symbol version may be included in a symbol name as a prefix after '@'.
// This function parses that part and returns a version ID number.
static uint16_t getVersionId(Symbol *Sym, StringRef Name) {
  size_t VersionBegin = Name.find('@');
  if (VersionBegin == StringRef::npos)
    return Config->VersionScriptGlobalByDefault ? VER_NDX_GLOBAL
                                                : VER_NDX_LOCAL;

  // If symbol name contains '@' or '@@' we can assign its version id right
  // here. '@@' means version by default. It is usually the most recent one.
  // VERSYM_HIDDEN flag should be set for all non-default versions.
  StringRef Version = Name.drop_front(VersionBegin + 1);
  bool Default = Version.startswith("@");
  if (Default)
    Version = Version.drop_front();

  size_t I = 2;
  for (elf::Version &V : Config->SymbolVersions) {
    if (V.Name == Version)
      return Default ? I : (I | VERSYM_HIDDEN);
    ++I;
  }
  error("symbol " + Name + " has undefined version " + Version);
  return 0;
}

// Find an existing symbol or create and insert a new one.
template <class ELFT>
std::pair<Symbol *, bool> SymbolTable<ELFT>::insert(StringRef Name) {
  unsigned NumSyms = SymVector.size();
  auto P = Symtab.insert(std::make_pair(Name, NumSyms));
  Symbol *Sym;
  if (P.second) {
    Sym = new (Alloc) Symbol;
    Sym->Binding = STB_WEAK;
    Sym->Visibility = STV_DEFAULT;
    Sym->IsUsedInRegularObj = false;
    Sym->ExportDynamic = false;
    Sym->VersionId = getVersionId(Sym, Name);
    Sym->VersionedName =
        Sym->VersionId != VER_NDX_LOCAL && Sym->VersionId != VER_NDX_GLOBAL;
    SymVector.push_back(Sym);
  } else {
    Sym = SymVector[P.first->second];
  }
  return {Sym, P.second};
}

// Find an existing symbol or create and insert a new one, then apply the given
// attributes.
template <class ELFT>
std::pair<Symbol *, bool>
SymbolTable<ELFT>::insert(StringRef Name, uint8_t Type, uint8_t Visibility,
                          bool CanOmitFromDynSym, bool IsUsedInRegularObj,
                          InputFile *File) {
  Symbol *S;
  bool WasInserted;
  std::tie(S, WasInserted) = insert(Name);

  // Merge in the new symbol's visibility.
  S->Visibility = getMinVisibility(S->Visibility, Visibility);
  if (!CanOmitFromDynSym && (Config->Shared || Config->ExportDynamic))
    S->ExportDynamic = true;
  if (IsUsedInRegularObj)
    S->IsUsedInRegularObj = true;
  if (!WasInserted && S->body()->Type != SymbolBody::UnknownType &&
      ((Type == STT_TLS) != S->body()->isTls()))
    error("TLS attribute mismatch for symbol: " +
          conflictMsg(S->body(), File));

  return {S, WasInserted};
}

// Construct a string in the form of "Sym in File1 and File2".
// Used to construct an error message.
template <typename ELFT>
std::string SymbolTable<ELFT>::conflictMsg(SymbolBody *Existing,
                                           InputFile *NewFile) {
  StringRef Sym = Existing->getName();
  return demangle(Sym) + " in " + getFilename(Existing->getSourceFile<ELFT>()) +
         " and " + getFilename(NewFile);
}

template <class ELFT> Symbol *SymbolTable<ELFT>::addUndefined(StringRef Name) {
  return addUndefined(Name, STB_GLOBAL, STV_DEFAULT, /*Type*/ 0,
                      /*CanOmitFromDynSym*/ false, /*File*/ nullptr);
}

template <class ELFT>
Symbol *SymbolTable<ELFT>::addUndefined(StringRef Name, uint8_t Binding,
                                        uint8_t StOther, uint8_t Type,
                                        bool CanOmitFromDynSym,
                                        InputFile *File) {
  Symbol *S;
  bool WasInserted;
  std::tie(S, WasInserted) =
      insert(Name, Type, StOther & 3, CanOmitFromDynSym,
             /*IsUsedInRegularObj*/ !File || !isa<BitcodeFile>(File), File);
  if (WasInserted) {
    S->Binding = Binding;
    replaceBody<Undefined>(S, Name, StOther, Type);
    cast<Undefined>(S->body())->File = File;
    return S;
  }
  if (Binding != STB_WEAK) {
    if (S->body()->isShared() || S->body()->isLazy())
      S->Binding = Binding;
    if (auto *SS = dyn_cast<SharedSymbol<ELFT>>(S->body()))
      SS->File->IsUsed = true;
  }
  if (auto *L = dyn_cast<Lazy>(S->body())) {
    // An undefined weak will not fetch archive members, but we have to remember
    // its type. See also comment in addLazyArchive.
    if (S->isWeak())
      L->Type = Type;
    else if (auto F = L->getFile())
      addFile(std::move(F));
  }
  return S;
}

// We have a new defined symbol with the specified binding. Return 1 if the new
// symbol should win, -1 if the new symbol should lose, or 0 if both symbols are
// strong defined symbols.
static int compareDefined(Symbol *S, bool WasInserted, uint8_t Binding) {
  if (WasInserted)
    return 1;
  SymbolBody *Body = S->body();
  if (Body->isLazy() || Body->isUndefined() || Body->isShared())
    return 1;
  if (Binding == STB_WEAK)
    return -1;
  if (S->isWeak())
    return 1;
  return 0;
}

// We have a new non-common defined symbol with the specified binding. Return 1
// if the new symbol should win, -1 if the new symbol should lose, or 0 if there
// is a conflict. If the new symbol wins, also update the binding.
static int compareDefinedNonCommon(Symbol *S, bool WasInserted, uint8_t Binding) {
  if (int Cmp = compareDefined(S, WasInserted, Binding)) {
    if (Cmp > 0)
      S->Binding = Binding;
    return Cmp;
  }
  if (isa<DefinedCommon>(S->body())) {
    // Non-common symbols take precedence over common symbols.
    if (Config->WarnCommon)
      warning("common " + S->body()->getName() + " is overridden");
    return 1;
  }
  return 0;
}

template <class ELFT>
Symbol *SymbolTable<ELFT>::addCommon(StringRef N, uint64_t Size,
                                     uint64_t Alignment, uint8_t Binding,
                                     uint8_t StOther, uint8_t Type,
                                     InputFile *File) {
  Symbol *S;
  bool WasInserted;
  std::tie(S, WasInserted) =
      insert(N, Type, StOther & 3, /*CanOmitFromDynSym*/ false,
             /*IsUsedInRegularObj*/ true, File);
  int Cmp = compareDefined(S, WasInserted, Binding);
  if (Cmp > 0) {
    S->Binding = Binding;
    replaceBody<DefinedCommon>(S, N, Size, Alignment, StOther, Type);
  } else if (Cmp == 0) {
    auto *C = dyn_cast<DefinedCommon>(S->body());
    if (!C) {
      // Non-common symbols take precedence over common symbols.
      if (Config->WarnCommon)
        warning("common " + S->body()->getName() + " is overridden");
      return S;
    }

    if (Config->WarnCommon)
      warning("multiple common of " + S->body()->getName());

    C->Size = std::max(C->Size, Size);
    C->Alignment = std::max(C->Alignment, Alignment);
  }
  return S;
}

template <class ELFT>
void SymbolTable<ELFT>::reportDuplicate(SymbolBody *Existing,
                                        InputFile *NewFile) {
  std::string Msg = "duplicate symbol: " + conflictMsg(Existing, NewFile);
  if (Config->AllowMultipleDefinition)
    warning(Msg);
  else
    error(Msg);
}

template <typename ELFT>
Symbol *SymbolTable<ELFT>::addRegular(StringRef Name, const Elf_Sym &Sym,
                                      InputSectionBase<ELFT> *Section) {
  Symbol *S;
  bool WasInserted;
  std::tie(S, WasInserted) =
      insert(Name, Sym.getType(), Sym.getVisibility(),
             /*CanOmitFromDynSym*/ false, /*IsUsedInRegularObj*/ true,
             Section ? Section->getFile() : nullptr);
  int Cmp = compareDefinedNonCommon(S, WasInserted, Sym.getBinding());
  if (Cmp > 0)
    replaceBody<DefinedRegular<ELFT>>(S, Name, Sym, Section);
  else if (Cmp == 0)
    reportDuplicate(S->body(), Section->getFile());
  return S;
}

template <typename ELFT>
Symbol *SymbolTable<ELFT>::addRegular(StringRef Name, uint8_t Binding,
                                      uint8_t StOther) {
  Symbol *S;
  bool WasInserted;
  std::tie(S, WasInserted) =
      insert(Name, STT_NOTYPE, StOther & 3, /*CanOmitFromDynSym*/ false,
             /*IsUsedInRegularObj*/ true, nullptr);
  int Cmp = compareDefinedNonCommon(S, WasInserted, Binding);
  if (Cmp > 0)
    replaceBody<DefinedRegular<ELFT>>(S, Name, StOther);
  else if (Cmp == 0)
    reportDuplicate(S->body(), nullptr);
  return S;
}

template <typename ELFT>
Symbol *SymbolTable<ELFT>::addSynthetic(StringRef N,
                                        OutputSectionBase<ELFT> *Section,
                                        uintX_t Value) {
  Symbol *S;
  bool WasInserted;
  std::tie(S, WasInserted) =
      insert(N, STT_NOTYPE, STV_HIDDEN, /*CanOmitFromDynSym*/ false,
             /*IsUsedInRegularObj*/ true, nullptr);
  int Cmp = compareDefinedNonCommon(S, WasInserted, STB_GLOBAL);
  if (Cmp > 0)
    replaceBody<DefinedSynthetic<ELFT>>(S, N, Value, Section);
  else if (Cmp == 0)
    reportDuplicate(S->body(), nullptr);
  return S;
}

template <typename ELFT>
void SymbolTable<ELFT>::addShared(SharedFile<ELFT> *F, StringRef Name,
                                  const Elf_Sym &Sym,
                                  const typename ELFT::Verdef *Verdef) {
  // DSO symbols do not affect visibility in the output, so we pass STV_DEFAULT
  // as the visibility, which will leave the visibility in the symbol table
  // unchanged.
  Symbol *S;
  bool WasInserted;
  std::tie(S, WasInserted) =
      insert(Name, Sym.getType(), STV_DEFAULT, /*CanOmitFromDynSym*/ true,
             /*IsUsedInRegularObj*/ false, F);
  // Make sure we preempt DSO symbols with default visibility.
  if (Sym.getVisibility() == STV_DEFAULT)
    S->ExportDynamic = true;
  if (WasInserted || isa<Undefined>(S->body())) {
    replaceBody<SharedSymbol<ELFT>>(S, F, Name, Sym, Verdef);
    if (!S->isWeak())
      F->IsUsed = true;
  }
}

template <class ELFT>
Symbol *SymbolTable<ELFT>::addBitcode(StringRef Name, bool IsWeak,
                                      uint8_t StOther, uint8_t Type,
                                      bool CanOmitFromDynSym, BitcodeFile *F) {
  Symbol *S;
  bool WasInserted;
  std::tie(S, WasInserted) = insert(Name, Type, StOther & 3, CanOmitFromDynSym,
                                    /*IsUsedInRegularObj*/ false, F);
  int Cmp =
      compareDefinedNonCommon(S, WasInserted, IsWeak ? STB_WEAK : STB_GLOBAL);
  if (Cmp > 0)
    replaceBody<DefinedBitcode>(S, Name, StOther, Type, F);
  else if (Cmp == 0)
    reportDuplicate(S->body(), F);
  return S;
}

template <class ELFT> SymbolBody *SymbolTable<ELFT>::find(StringRef Name) {
  auto It = Symtab.find(Name);
  if (It == Symtab.end())
    return nullptr;
  return SymVector[It->second]->body();
}

// Returns a list of defined symbols that match with a given glob pattern.
template <class ELFT>
std::vector<SymbolBody *> SymbolTable<ELFT>::findAll(StringRef Pattern) {
  // Fast-path. Fallback to find() if Pattern doesn't contain any wildcard
  // characters.
  if (Pattern.find_first_of("?*") == StringRef::npos) {
    if (SymbolBody *B = find(Pattern))
      if (!B->isUndefined())
        return {B};
    return {};
  }

  std::vector<SymbolBody *> Res;
  for (auto &It : Symtab) {
    StringRef Name = It.first.Val;
    SymbolBody *B = SymVector[It.second]->body();
    if (!B->isUndefined() && globMatch(Pattern, Name))
      Res.push_back(B);
  }
  return Res;
}

template <class ELFT>
void SymbolTable<ELFT>::addLazyArchive(
    ArchiveFile *F, const llvm::object::Archive::Symbol Sym) {
  Symbol *S;
  bool WasInserted;
  std::tie(S, WasInserted) = insert(Sym.getName());
  if (WasInserted) {
    replaceBody<LazyArchive>(S, *F, Sym, SymbolBody::UnknownType);
    return;
  }
  if (!S->body()->isUndefined())
    return;

  // Weak undefined symbols should not fetch members from archives. If we were
  // to keep old symbol we would not know that an archive member was available
  // if a strong undefined symbol shows up afterwards in the link. If a strong
  // undefined symbol never shows up, this lazy symbol will get to the end of
  // the link and must be treated as the weak undefined one. We already marked
  // this symbol as used when we added it to the symbol table, but we also need
  // to preserve its type. FIXME: Move the Type field to Symbol.
  if (S->isWeak()) {
    replaceBody<LazyArchive>(S, *F, Sym, S->body()->Type);
    return;
  }
  MemoryBufferRef MBRef = F->getMember(&Sym);
  if (!MBRef.getBuffer().empty())
    addFile(createObjectFile(MBRef, F->getName()));
}

template <class ELFT>
void SymbolTable<ELFT>::addLazyObject(StringRef Name, LazyObjectFile &Obj) {
  Symbol *S;
  bool WasInserted;
  std::tie(S, WasInserted) = insert(Name);
  if (WasInserted) {
    replaceBody<LazyObject>(S, Name, Obj, SymbolBody::UnknownType);
    return;
  }
  if (!S->body()->isUndefined())
    return;

  // See comment for addLazyArchive above.
  if (S->isWeak()) {
    replaceBody<LazyObject>(S, Name, Obj, S->body()->Type);
  } else {
    MemoryBufferRef MBRef = Obj.getBuffer();
    if (!MBRef.getBuffer().empty())
      addFile(createObjectFile(MBRef));
  }
}

// Process undefined (-u) flags by loading lazy symbols named by those flags.
template <class ELFT> void SymbolTable<ELFT>::scanUndefinedFlags() {
  for (StringRef S : Config->Undefined)
    if (auto *L = dyn_cast_or_null<Lazy>(find(S)))
      if (std::unique_ptr<InputFile> File = L->getFile())
        addFile(std::move(File));
}

// This function takes care of the case in which shared libraries depend on
// the user program (not the other way, which is usual). Shared libraries
// may have undefined symbols, expecting that the user program provides
// the definitions for them. An example is BSD's __progname symbol.
// We need to put such symbols to the main program's .dynsym so that
// shared libraries can find them.
// Except this, we ignore undefined symbols in DSOs.
template <class ELFT> void SymbolTable<ELFT>::scanShlibUndefined() {
  for (std::unique_ptr<SharedFile<ELFT>> &File : SharedFiles)
    for (StringRef U : File->getUndefinedSymbols())
      if (SymbolBody *Sym = find(U))
        if (Sym->isDefined())
          Sym->symbol()->ExportDynamic = true;
}

// This function process the dynamic list option by marking all the symbols
// to be exported in the dynamic table.
template <class ELFT> void SymbolTable<ELFT>::scanDynamicList() {
  for (StringRef S : Config->DynamicList)
    if (SymbolBody *B = find(S))
      B->symbol()->ExportDynamic = true;
}

// This function processes the --version-script option by marking all global
// symbols with the VersionScriptGlobal flag, which acts as a filter on the
// dynamic symbol table.
template <class ELFT> void SymbolTable<ELFT>::scanVersionScript() {
  // If version script does not contain versions declarations,
  // we just should mark global symbols.
  if (!Config->VersionScriptGlobals.empty()) {
    for (StringRef S : Config->VersionScriptGlobals)
      if (SymbolBody *B = find(S))
        B->symbol()->VersionId = VER_NDX_GLOBAL;
    return;
  }

  // If we have symbols version declarations, we should
  // assign version references for each symbol.
  size_t I = 2;
  for (Version &V : Config->SymbolVersions) {
    for (StringRef Name : V.Globals) {
      std::vector<SymbolBody *> Syms = findAll(Name);
      if (Syms.empty()) {
        if (Config->NoUndefinedVersion)
          error("version script assignment of " + V.Name + " to symbol " +
                Name + " failed: symbol not defined");
        continue;
      }

      for (SymbolBody *B : Syms) {
        if (B->symbol()->VersionId != VER_NDX_GLOBAL &&
            B->symbol()->VersionId != VER_NDX_LOCAL)
          warning("duplicate symbol " + Name + " in version script");
        B->symbol()->VersionId = I;
      }
    }
    ++I;
  }
}

// Print the module names which define the notified
// symbols provided through -y or --trace-symbol option.
template <class ELFT> void SymbolTable<ELFT>::traceDefined() {
  for (const auto &Symbol : Config->TraceSymbol)
    if (SymbolBody *B = find(Symbol.getKey()))
      if (B->isDefined() || B->isCommon())
        if (InputFile *File = B->getSourceFile<ELFT>())
          outs() << getFilename(File) << ": definition of "
                 << B->getName() << "\n";
}

template class elf::SymbolTable<ELF32LE>;
template class elf::SymbolTable<ELF32BE>;
template class elf::SymbolTable<ELF64LE>;
template class elf::SymbolTable<ELF64BE>;
