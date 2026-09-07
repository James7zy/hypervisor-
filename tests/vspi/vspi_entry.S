/* SPDX-License-Identifier: TBD */
    .section .text.start, "ax"
    .globl _start
_start:
    msr daifset, #0xf
    ldr x0, =vspi_stack0 + 0x4000
    mov sp, x0
    ldr x0, =__bss_start
    ldr x1, =__bss_end
1:  cmp x0, x1
    b.hs 2f
    str xzr, [x0], #8
    b 1b
2:  b vspi_primary

    .globl vspi_secondary_entry
vspi_secondary_entry:
    msr daifset, #0xf
    ldr x0, =vspi_stack1 + 0x4000
    mov sp, x0
    b vspi_secondary
