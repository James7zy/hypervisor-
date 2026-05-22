/* SPDX-License-Identifier: TBD */
#ifndef BOARD_QEMU_VIRT_H
#define BOARD_QEMU_VIRT_H

/* PL011 base on QEMU virt machine. See qemu/hw/arm/virt.c. */
#define BOARD_UART_BASE  0x09000000UL
#define BOARD_DRAM_BASE  0x40000000UL

extern const char board_name[];

#endif /* BOARD_QEMU_VIRT_H */
