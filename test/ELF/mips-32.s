# Check R_MIPS_32 relocation calculation.

# RUN: llvm-mc -filetype=obj -triple=mips-unknown-linux %s -o %t-be.o
# RUN: ld.lld -shared %t-be.o -o %t-be.so
# RUN: llvm-objdump -t -s %t-be.so \
# RUN:   | FileCheck -check-prefix=SYM -check-prefix=BE %s
# RUN: llvm-readobj -r -dynamic-table -mips-plt-got %t-be.so \
# RUN:   | FileCheck -check-prefix=REL %s

# RUN: llvm-mc -filetype=obj -triple=mipsel-unknown-linux %s -o %t-el.o
# RUN: ld.lld -shared %t-el.o -o %t-el.so
# RUN: llvm-objdump -t -s %t-el.so \
# RUN:   | FileCheck -check-prefix=SYM -check-prefix=EL %s
# RUN: llvm-readobj -r -dynamic-table -mips-plt-got %t-el.so \
# RUN:   | FileCheck -check-prefix=REL %s

# REQUIRES: mips

  .globl  __start
__start:
  nop

  .data
  .type  v1,@object
  .size  v1,4
v1:
  .word 0

  .globl v2
  .type  v2,@object
  .size  v2,8
v2:
  .word v2+4 # R_MIPS_32 target v2 addend 4
  .word v1   # R_MIPS_32 target v1 addend 0

# BE: Contents of section .data:
# BE-NEXT: 30000 00000000 00000004 00030000
#                         ^-- v2+4 ^-- v1

# EL: Contents of section .data:
# EL-NEXT: 30000 00000000 04000000 00000300
#                         ^-- v2+4 ^-- v1

# SYM: SYMBOL TABLE:
# SYM: 00030000 l       .data           00000004 v1
# SYM: 00030004 g       .data           00000008 v2

# REL:      Relocations [
# REL-NEXT:   Section (7) .rel.dyn {
# REL-NEXT:     0x30008 R_MIPS_REL32 - 0x0
# REL-NEXT:     0x30004 R_MIPS_REL32 v2 0x0
# REL-NEXT:   }
# REL-NEXT: ]

# REL: DynamicSection [
# REL:   Tag        Type                 Name/Value
# REL:   0x00000012 RELSZ                16 (bytes)
# REL:   0x00000013 RELENT               8 (bytes)

# REL:      Primary GOT {
# REL-NEXT:   Canonical gp value:
# REL-NEXT:   Reserved entries [
# REL:        ]
# REL-NEXT:   Local entries [
# REL-NEXT:   ]
# REL-NEXT:   Global entries [
# REL-NEXT:     Entry {
# REL-NEXT:       Address:
# REL-NEXT:       Access:
# REL-NEXT:       Initial: 0x30004
# REL-NEXT:       Value: 0x30004
# REL-NEXT:       Type: Object
# REL-NEXT:       Section: .data
# REL-NEXT:       Name: v2
# REL-NEXT:     }
# REL-NEXT:   ]
# REL-NEXT:   Number of TLS and multi-GOT entries: 0
# REL-NEXT: }
