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

extern const char board_name[];

#endif /* BOARD_QEMU_VIRT_H */
