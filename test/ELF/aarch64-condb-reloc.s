# RUN: llvm-mc -filetype=obj -triple=aarch64-unknown-freebsd %p/Inputs/aarch64-condb-reloc.s -o %t1
# RUN: llvm-mc -filetype=obj -triple=aarch64-unknown-freebsd %s -o %t2
# RUN: ld.lld %t1 %t2 -o %t
# RUN: llvm-objdump -d %t | FileCheck %s
# RUN: ld.lld -shared %t1 %t2 -o %t3
# RUN: llvm-objdump -d %t3 | FileCheck -check-prefix=DSO %s
# RUN: llvm-readobj -s -r %t3 | FileCheck -check-prefix=DSOREL %s
# REQUIRES: aarch64

# 0x11024 - 36 = 0x11000
# 0x11028 - 24 = 0x11010
# 0x1102c - 16 = 0x1101c
# CHECK:      Disassembly of section .text:
# CHECK-NEXT: _foo:
# CHECK-NEXT:    11000: {{.*}} nop
# CHECK-NEXT:    11004: {{.*}} nop
# CHECK-NEXT:    11008: {{.*}} nop
# CHECK-NEXT:    1100c: {{.*}} nop
# CHECK:      _bar:
# CHECK-NEXT:    11010: {{.*}} nop
# CHECK-NEXT:    11014: {{.*}} nop
# CHECK-NEXT:    11018: {{.*}} nop
# CHECK:      _dah:
# CHECK-NEXT:    1101c: {{.*}} nop
# CHECK-NEXT:    11020: {{.*}} nop
# CHECK:      _start:
# CHECK-NEXT:    11024: {{.*}} b.eq #-36
# CHECK-NEXT:    11028: {{.*}} b.eq #-24
# CHECK-NEXT:    1102c: {{.*}} b.eq #-16

#DSOREL:      Section {
#DSOREL:        Index:
#DSOREL:        Name: .got.plt
#DSOREL-NEXT:   Type: SHT_PROGBITS
#DSOREL-NEXT:   Flags [
#DSOREL-NEXT:     SHF_ALLOC
#DSOREL-NEXT:     SHF_WRITE
#DSOREL-NEXT:   ]
#DSOREL-NEXT:   Address: 0x3000
#DSOREL-NEXT:   Offset: 0x3000
#DSOREL-NEXT:   Size: 48
#DSOREL-NEXT:   Link: 0
#DSOREL-NEXT:   Info: 0
#DSOREL-NEXT:   AddressAlignment: 8
#DSOREL-NEXT:   EntrySize: 0
#DSOREL-NEXT:  }
#DSOREL:      Relocations [
#DSOREL-NEXT:  Section ({{.*}}) .rela.plt {
#DSOREL-NEXT:    0x3018 R_AARCH64_JUMP_SLOT _foo
#DSOREL-NEXT:    0x3020 R_AARCH64_JUMP_SLOT _bar
#DSOREL-NEXT:    0x3028 R_AARCH64_JUMP_SLOT _dah
#DSOREL-NEXT:  }
#DSOREL-NEXT:]

#DSO:      Disassembly of section .text:
#DSO-NEXT: _foo:
#DSO-NEXT:     1000: {{.*}} nop
#DSO-NEXT:     1004: {{.*}} nop
#DSO-NEXT:     1008: {{.*}} nop
#DSO-NEXT:     100c: {{.*}} nop
#DSO:      _bar:
#DSO-NEXT:     1010: {{.*}} nop
#DSO-NEXT:     1014: {{.*}} nop
#DSO-NEXT:     1018: {{.*}} nop
#DSO:      _dah:
#DSO-NEXT:     101c: {{.*}} nop
#DSO-NEXT:     1020: {{.*}} nop
#DSO:      _start:
#DSO-NEXT:     1024: {{.*}} b.eq #44
#DSO-NEXT:     1028: {{.*}} b.eq #56
#DSO-NEXT:     102c: {{.*}} b.eq #68
#DSO-NEXT: Disassembly of section .plt:
#DSO-NEXT: .plt:
#DSO-NEXT:     1030: {{.*}} stp x16, x30, [sp, #-16]!
#DSO-NEXT:     1034: {{.*}} adrp x16, #8192
#DSO-NEXT:     1038: {{.*}} ldr x17, [x16, #16]
#DSO-NEXT:     103c: {{.*}} add x16, x16, #16
#DSO-NEXT:     1040: {{.*}} br x17
#DSO-NEXT:     1044: {{.*}} nop
#DSO-NEXT:     1048: {{.*}} nop
#DSO-NEXT:     104c: {{.*}} nop
#DSO-NEXT:     1050: {{.*}} adrp x16, #8192
#DSO-NEXT:     1054: {{.*}} ldr x17, [x16, #24]
#DSO-NEXT:     1058: {{.*}} add x16, x16, #24
#DSO-NEXT:     105c: {{.*}} br x17
#DSO-NEXT:     1060: {{.*}} adrp x16, #8192
#DSO-NEXT:     1064: {{.*}} ldr x17, [x16, #32]
#DSO-NEXT:     1068: {{.*}} add x16, x16, #32
#DSO-NEXT:     106c: {{.*}} br x17
#DSO-NEXT:     1070: {{.*}} adrp x16, #8192
#DSO-NEXT:     1074: {{.*}} ldr x17, [x16, #40]
#DSO-NEXT:     1078: {{.*}} add x16, x16, #40
#DSO-NEXT:     107c: {{.*}} br x17

.globl _start
_start:
 b.eq _foo
 b.eq _bar
 b.eq _dah
