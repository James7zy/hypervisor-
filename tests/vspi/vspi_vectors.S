/* SPDX-License-Identifier: TBD */
/* EL1 vector table for the M2.5 timer SVM guest. The guest runs at EL1h
 * (SP_EL1), so the forwarded virtual timer IRQ enters at offset 0x280
 * (Current EL with SP_ELx, IRQ). Every other vector reports an exception. */

    .macro VEC label
    .align 7                    /* 128-byte (0x80) vector stride */
    b   \label
    .endm

    .section .text.vectors, "ax"
    .align 11                   /* VBAR_EL1 requires 2048-byte alignment */
    .globl vspi_vectors
vspi_vectors:
    VEC vspi_park       /* 0x000 Cur EL SP0  Sync */
    VEC vspi_park       /* 0x080 Cur EL SP0  IRQ  */
    VEC vspi_park       /* 0x100 Cur EL SP0  FIQ  */
    VEC vspi_park       /* 0x180 Cur EL SP0  SErr */
    VEC vspi_park       /* 0x200 Cur EL SPx  Sync */
    VEC vspi_irq_entry  /* 0x280 Cur EL SPx  IRQ  */
    VEC vspi_park       /* 0x300 Cur EL SPx  FIQ  */
    VEC vspi_park       /* 0x380 Cur EL SPx  SErr */
    VEC vspi_park       /* 0x400 Lower A64   Sync */
    VEC vspi_park       /* 0x480 Lower A64   IRQ  */
    VEC vspi_park       /* 0x500 Lower A64   FIQ  */
    VEC vspi_park       /* 0x580 Lower A64   SErr */
    VEC vspi_park       /* 0x600 Lower A32   Sync */
    VEC vspi_park       /* 0x680 Lower A32   IRQ  */
    VEC vspi_park       /* 0x700 Lower A32   FIQ  */
    VEC vspi_park       /* 0x780 Lower A32   SErr */

    .section .text
vspi_park:
    b   vspi_exception

    .globl vspi_irq_entry
vspi_irq_entry:
    /* Frame: x0..x18, x30, ELR_EL1, SPSR_EL1 — 11 pairs = 176 bytes (16-aligned).
     * x19..x28 are preserved by the AAPCS C handler, so we need not save them. */
    stp     x0,  x1,  [sp, #-176]!
    stp     x2,  x3,  [sp, #0x10]
    stp     x4,  x5,  [sp, #0x20]
    stp     x6,  x7,  [sp, #0x30]
    stp     x8,  x9,  [sp, #0x40]
    stp     x10, x11, [sp, #0x50]
    stp     x12, x13, [sp, #0x60]
    stp     x14, x15, [sp, #0x70]
    stp     x16, x17, [sp, #0x80]
    stp     x18, x30, [sp, #0x90]
    mrs     x0, elr_el1
    mrs     x1, spsr_el1
    stp     x0,  x1,  [sp, #0xA0]

    bl      vspi_irq_handler

    ldp     x0,  x1,  [sp, #0xA0]
    msr     elr_el1,  x0
    msr     spsr_el1, x1
    ldp     x2,  x3,  [sp, #0x10]
    ldp     x4,  x5,  [sp, #0x20]
    ldp     x6,  x7,  [sp, #0x30]
    ldp     x8,  x9,  [sp, #0x40]
    ldp     x10, x11, [sp, #0x50]
    ldp     x12, x13, [sp, #0x60]
    ldp     x14, x15, [sp, #0x70]
    ldp     x16, x17, [sp, #0x80]
    ldp     x18, x30, [sp, #0x90]
    ldp     x0,  x1,  [sp], #176
    eret
