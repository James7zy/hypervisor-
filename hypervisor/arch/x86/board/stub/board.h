/* SPDX-License-Identifier: TBD */
#ifndef BOARD_X86_STUB_H
#define BOARD_X86_STUB_H

/*
 * Placeholder values for the BOARD_* macros common/vm/vm_configs.c reads.
 * The stub is linked, never run, so these describe no real machine.
 */
#define BOARD_UART_BASE        0x000003F8UL   /* COM1 (I/O port, placeholder) */
#define BOARD_PL011_IRQ        4U             /* COM1 IRQ */

#define BOARD_SVM_ENTRY        0x00200000UL
#define BOARD_SVM_MEM_BASE     0x00200000UL
#define BOARD_SVM_MEM_SIZE     0x00200000UL
#define BOARD_SVM2_RAM_PA      0x40200000UL

#define BOARD_LINUX_RAM_IPA    0x00000000UL
#define BOARD_LINUX_RAM_PA     0x40000000UL
#define BOARD_LINUX_RAM_SIZE   0x10000000UL
#define BOARD_LINUX_IMAGE_PA   0x40100000UL
#define BOARD_LINUX_DTB_IPA    0x00000000UL
#define BOARD_LINUX2_RAM_PA    0x80000000UL
#define BOARD_LINUX2_IMAGE_PA  0x80100000UL

#endif /* BOARD_X86_STUB_H */
