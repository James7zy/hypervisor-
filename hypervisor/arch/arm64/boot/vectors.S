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
    /* Current EL with SP_ELx */
    VECTOR_ENTRY panic_vector
    VECTOR_ENTRY panic_vector
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
