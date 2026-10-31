/* SPDX-License-Identifier: TBD */
/* EL2 vector table. M0: every entry funnels into panic_vector. */

    .macro VECTOR_ENTRY label
    .align 7                    /* each entry is 128 bytes (1 << 7) */
    b   \label
    .endm

    .section .text.vectors, "ax"
    .align 11                   /* table must be 2048-byte aligned */
    .globl hv_vectors
hv_vectors:
    /* Current EL with SP_EL0 */
    VECTOR_ENTRY panic_vector
    VECTOR_ENTRY panic_vector
    VECTOR_ENTRY panic_vector
    VECTOR_ENTRY panic_vector
    /* Current EL with SP_ELx. IRQ (2nd slot) is live since M11 B2: an EL2 C
     * context (the scheduler loop's cpu_arch_idle nap, interrupts briefly
     * unmasked) takes physical IRQs here — the blocked-vCPU wake path. The
     * rest stay panics: nothing at EL2 C may sync-abort or take FIQ. */
    VECTOR_ENTRY panic_vector
    VECTOR_ENTRY el2_irq_entry
    VECTOR_ENTRY panic_vector
    VECTOR_ENTRY panic_vector
    /* Lower EL, AArch64 */
    VECTOR_ENTRY el1_sync_handler
    VECTOR_ENTRY el1_irq_handler_asm
    VECTOR_ENTRY panic_vector
    VECTOR_ENTRY panic_vector
    /* Lower EL, AArch32 */
    VECTOR_ENTRY panic_vector
    VECTOR_ENTRY panic_vector
    VECTOR_ENTRY panic_vector
    VECTOR_ENTRY panic_vector

    .section .text
    .globl panic_vector
panic_vector:
    mrs     x0, esr_el2
    mrs     x1, elr_el2
    adrp    x2, .Lpanic_fmt
    add     x2, x2, :lo12:.Lpanic_fmt
    mov     x3, x0
    mov     x4, x1
    mov     x0, x2
    mov     x1, x3
    mov     x2, x4
    bl      printk
1:  wfi
    b       1b

    .section .rodata
.Lpanic_fmt:
    .asciz "!VEC ESR=0x%lx ELR=0x%lx\n"

/* ------------------------------------------------------------------ */
/*  el2_irq_entry — current-EL (SP_ELx) IRQ                            */
/* ------------------------------------------------------------------ */
    .section .text
    .globl el2_irq_entry
el2_irq_entry:
    /* Interrupted context is EL2 C on this pCPU's own stack. Save the
     * caller-saved frame the C handler is allowed to clobber, plus the
     * exception-return pair; callee-saved regs are the handler's job per
     * the ABI. Exception entry masked DAIF — restoring SPSR_EL2 (the
     * interrupted PSTATE, I clear inside cpu_arch_idle's window) unmasked
     * on eret. */
    stp     x29, x30, [sp, #-16]!
    stp     x0,  x1,  [sp, #-16]!
    stp     x2,  x3,  [sp, #-16]!
    stp     x4,  x5,  [sp, #-16]!
    stp     x6,  x7,  [sp, #-16]!
    stp     x8,  x9,  [sp, #-16]!
    stp     x10, x11, [sp, #-16]!
    stp     x12, x13, [sp, #-16]!
    stp     x14, x15, [sp, #-16]!
    stp     x16, x17, [sp, #-16]!
    stp     x18, x19, [sp, #-16]!
    stp     x20, x21, [sp, #-16]!
    stp     x22, x23, [sp, #-16]!
    stp     x24, x25, [sp, #-16]!
    stp     x26, x27, [sp, #-16]!
    stp     x28, xzr, [sp, #-16]!
    mrs     x9,  elr_el2
    mrs     x10, spsr_el2
    stp     x9,  x10, [sp, #-16]!

    bl      el2_irq_handler

    ldp     x9,  x10, [sp], #16
    msr     elr_el2, x9
    msr     spsr_el2, x10
    ldp     x28, xzr, [sp], #16
    ldp     x26, x27, [sp], #16
    ldp     x24, x25, [sp], #16
    ldp     x22, x23, [sp], #16
    ldp     x20, x21, [sp], #16
    ldp     x18, x19, [sp], #16
    ldp     x16, x17, [sp], #16
    ldp     x14, x15, [sp], #16
    ldp     x12, x13, [sp], #16
    ldp     x10, x11, [sp], #16
    ldp     x8,  x9,  [sp], #16
    ldp     x6,  x7,  [sp], #16
    ldp     x4,  x5,  [sp], #16
    ldp     x2,  x3,  [sp], #16
    ldp     x0,  x1,  [sp], #16
    ldp     x29, x30, [sp], #16
    eret
