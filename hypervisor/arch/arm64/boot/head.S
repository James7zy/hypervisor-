/* SPDX-License-Identifier: TBD */
/*
 * AArch64 EL2 entry point. QEMU passes:
 *   x0 = DTB physical address
 *   PC = 0x40080000
 *   CurrentEL = 0b1000 (EL2) when -machine virt,virtualization=on
 *
 * x19 is callee-saved (AAPCS64); used to preserve DTB across memset call.
 */

#include <percpu.h>   /* NR_CPUS, PERCPU_SIZE (asm-visible macros) */

    /* Early-panic UART base — only hard-coded board address in M0.
     * Justified: C is not available yet, cannot use board.h. */
    .equ EARLY_UART_DR, 0x09000000

    /* Per-CPU EL2 stack size for PSCI-woken secondaries (16 KiB, matches the
     * boot stack in linker.lds). */
    .equ SECONDARY_STACK_SIZE, 0x4000

    .section .text._start, "ax"
    .globl _start
_start:
    /* Defensive: only the boot core should reach _start. Under QEMU PSCI the
     * secondaries are powered OFF and are later started directly at
     * secondary_entry, not here. Park anything unexpected (Aff0 != 0). */
    mrs     x9, mpidr_el1
    and     x9, x9, #0xFF
    cbnz    x9, boot_core_park

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

boot_core_park:
4:  wfi
    b       4b

/*
 * secondary_entry — EL2 entry for a PSCI-powered secondary core (M3.5 Slice 2).
 *
 * QEMU's PSCI implementation powers the secondary on directly at EL2 and jumps
 * here with x0 = context_id (we pass the cpu index 0/1). Unlike _start, the
 * secondary is freshly powered on: no DTB, BSS already cleared by CPU0. Bring
 * EL2 to the same baseline as the boot core, then enter C.
 */
    .globl secondary_entry
secondary_entry:
    mov     x19, x0                 /* x19 = context_id (cpu index), callee-saved */

    /* Per-CPU EL2 stack: secondary_stacks + (id+1) * SECONDARY_STACK_SIZE
     * (top of this core's slot; full-descending). */
    ldr     x9, =secondary_stacks
    add     x10, x19, #1
    mov     x11, #SECONDARY_STACK_SIZE
    mul     x10, x10, x11
    add     x9, x9, x10
    mov     sp, x9

    /* TPIDR_EL2 = &percpu[id] = percpu + id * PERCPU_SIZE */
    ldr     x9, =percpu
    mov     x10, #PERCPU_SIZE
    mul     x10, x19, x10
    add     x9, x9, x10
    msr     tpidr_el2, x9

    /* Install EL2 vector table */
    ldr     x9, =hv_vectors
    msr     vbar_el2, x9
    isb

    /* Mask D, A, I, F at EL2 */
    msr     daifset, #0xF
    dsb     sy
    isb

    /* secondary_main(context_id) */
    mov     x0, x19
    bl      secondary_main

    /* secondary_main returns only if it parks; never reach here normally. */
3:  wfi
    b       3b

    .section .bss
    .align 4
    /* Per-CPU EL2 stacks for secondaries. Slot 0 is unused (CPU0 uses the
     * boot stack from the linker script); indexing by cpu id keeps the asm
     * uniform. */
secondary_stacks:
    .zero   SECONDARY_STACK_SIZE * NR_CPUS
