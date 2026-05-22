/* SPDX-License-Identifier: TBD */
/*
 * AArch64 EL2 entry point. QEMU passes:
 *   x0 = DTB physical address
 *   PC = 0x40080000
 *   CurrentEL = 0b1000 (EL2) when -machine virt,virtualization=on
 *
 * x19 is callee-saved (AAPCS64); used to preserve DTB across memset call.
 */

    /* Early-panic UART base — only hard-coded board address in M0.
     * Justified: C is not available yet, cannot use board.h. */
    .equ EARLY_UART_DR, 0x09000000

    .section .text._start, "ax"
    .globl _start
_start:
    /* Park secondary CPUs: MPIDR_EL1.Aff0 != 0 → sleep */
    mrs     x9, mpidr_el1
    and     x9, x9, #0xFF
    cbnz    x9, secondary_park

    /* Assert CurrentEL == EL2 (bits[3:2] = 0b10, raw value = 0x8) */
    mrs     x9, CurrentEL
    cmp     x9, #(2 << 2)
    b.ne    panic_early

    /* Save DTB pointer across memset */
    mov     x19, x0

    /* Set boot stack */
    ldr     x9, =__stack_top
    mov     sp, x9

    /* Clear BSS: memset(__bss_start, 0, __bss_end - __bss_start) */
    ldr     x0, =__bss_start
    ldr     x2, =__bss_end
    sub     x2, x2, x0
    mov     x1, #0
    bl      memset

    /* Install EL2 vector table */
    ldr     x9, =hv_vectors
    msr     vbar_el2, x9
    isb

    /* Mask D, A, I, F at EL2 */
    msr     daifset, #0xF
    dsb     sy
    isb

    /* Enter C */
    mov     x0, x19
    bl      hypervisor_main

    /* hypervisor_main never returns */
1:  wfi
    b       1b

panic_early:
    /* Emit "!EL\n" directly to PL011 DR (no FIFO check — pre-C path) */
    ldr     x9, =EARLY_UART_DR
    mov     w10, #'!'
    str     w10, [x9]
    mov     w10, #'E'
    str     w10, [x9]
    mov     w10, #'L'
    str     w10, [x9]
    mov     w10, #'\n'
    str     w10, [x9]
2:  wfi
    b       2b

secondary_park:
3:  wfi
    b       3b
