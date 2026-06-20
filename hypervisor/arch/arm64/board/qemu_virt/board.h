/* SPDX-License-Identifier: TBD */
#ifndef BOARD_QEMU_VIRT_H
#define BOARD_QEMU_VIRT_H

/* PL011 base on QEMU virt machine. See qemu/hw/arm/virt.c. */
#define BOARD_UART_BASE  0x09000000UL
#define BOARD_DRAM_BASE  0x40000000UL

/* M1: SVM interface contract — stable ABI between hypervisor and external SVM binary */
#define BOARD_SVM_ENTRY    0x40200000UL
#define BOARD_SVM_MEM_BASE 0x40200000UL
#define BOARD_SVM_MEM_SIZE 0x00200000UL   /* 2 MB */

/* M2.5: physical GICv3 + virtual timer. See qemu/hw/arm/virt.c. */
#define BOARD_GIC_DIST_BASE   0x08000000UL   /* GICD                  */
#define BOARD_GIC_RDIST_BASE  0x080A0000UL   /* GICR CPU0 RD_base     */
#define BOARD_VTIMER_IRQ      27U            /* EL1 virtual timer PPI */
#define BOARD_PL011_IRQ       33U            /* PL011 UART SPI (DTS interrupts=<0 1 4> → 32+1) */

/*
 * M3.0: Linux UP guest. Guest RAM is a dedicated PA region that does NOT
 * overlap the hv image (which loads at PA 0x40080000). Stage-2 maps the
 * guest RAM IPA window to this PA. PL011 (0x09000000) is identity-mapped
 * Device-nGnRE by the existing stage-2 device block — no new mapping.
 *
 * QEMU `-device loader,addr=` takes PHYSICAL addresses; the .dts /memory
 * node and the boot-protocol register state use the IPA values.
 */
#define BOARD_LINUX_RAM_IPA   0x40000000UL  /* guest sees RAM base here     */
#define BOARD_LINUX_RAM_PA    0x80000000UL  /* backing physical RAM (1GB-aligned: a Stage-2 L1 1GB block requires a 1GB-aligned output PA) */
#define BOARD_LINUX_RAM_SIZE  0x10000000UL  /* 256 MB                       */
#define BOARD_LINUX_IMAGE_PA  0x80080000UL  /* RAM_PA + text_offset 0x80000 */
#define BOARD_LINUX_DTB_IPA   0x42000000UL  /* guest sees DTB here          */
#define BOARD_LINUX_DTB_PA    0x82000000UL  /* RAM_PA + 0x02000000          */

extern const char board_name[];

#endif /* BOARD_QEMU_VIRT_H */
