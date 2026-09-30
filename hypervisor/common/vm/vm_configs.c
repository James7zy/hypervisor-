/* SPDX-License-Identifier: TBD */
/*
 * Static VM configuration table (ADR-0008). This is board configuration, so
 * it is the one file outside arch/ that reads BOARD_* (the permanent
 * exemption in scripts/check-arch-boundary.sh, ADR-0015).
 */
#include <types.h>
#include <board.h>
#include <vm.h>
#include "vm_config.h"

/* Every VM's console: the PL011 window and SPI the guest DTS describes. */
#define VM_VUART_CONFIG .vuart_base = BOARD_UART_BASE, .vuart_irq = BOARD_PL011_IRQ

#ifdef CONFIG_GUEST_SVM
const struct vm_config vm_configs[NR_VMS] = {
    [0] = { .vmid = 1, .entry = BOARD_SVM_ENTRY, .mem_base = BOARD_SVM_MEM_BASE,
            .ram_pa = BOARD_SVM_MEM_BASE, .mem_size = BOARD_SVM_MEM_SIZE,
            .dtb_ipa = 0, .pcpu_base = 0,
            VM_VUART_CONFIG },
#if CONFIG_NR_VMS > 1
    /*
     * VM1's SVM guest: SAME guest-visible IPA as VM0 (entry/mem_base =
     * BOARD_SVM_MEM_BASE, unified guest address map) but a DIFFERENT backing
     * PA (BOARD_SVM2_RAM_PA) — Stage-2 maps that shared IPA to a distinct
     * physical block per VM. This only works because every SVM test case
     * links at the SAME address via tests/svm/svm.lds (0x40200000): the
     * guest code executes correctly regardless of which physical RAM backs
     * its IPA.
     */
    [1] = { .vmid = 2, .entry = BOARD_SVM_ENTRY, .mem_base = BOARD_SVM_MEM_BASE,
            .ram_pa = BOARD_SVM2_RAM_PA, .mem_size = BOARD_SVM_MEM_SIZE,
            .dtb_ipa = 0, .pcpu_base = VCPUS_PER_VM,
            VM_VUART_CONFIG },
#endif
};
#else
const struct vm_config vm_configs[NR_VMS] = {
    [0] = { .vmid = 1,
            .entry = BOARD_LINUX_IMAGE_PA - BOARD_LINUX_RAM_PA + BOARD_LINUX_RAM_IPA,
            .mem_base = BOARD_LINUX_RAM_IPA, .ram_pa = BOARD_LINUX_RAM_PA,
            .mem_size = BOARD_LINUX_RAM_SIZE, .dtb_ipa = BOARD_LINUX_DTB_IPA,
            .pcpu_base = 0,
            VM_VUART_CONFIG },
#if CONFIG_NR_VMS > 1
    /*
     * VM1's Linux guest: SAME guest-visible IPA map as VM0 (entry/mem_base/
     * dtb_ipa unchanged — one guest DTB compiled once, loaded at the same IPA
     * for both VMs) but backed by the distinct physical block at
     * BOARD_LINUX2_RAM_PA/BOARD_LINUX2_IMAGE_PA.
     */
    [1] = { .vmid = 2,
            .entry = BOARD_LINUX2_IMAGE_PA - BOARD_LINUX2_RAM_PA + BOARD_LINUX_RAM_IPA,
            .mem_base = BOARD_LINUX_RAM_IPA, .ram_pa = BOARD_LINUX2_RAM_PA,
            .mem_size = BOARD_LINUX_RAM_SIZE, .dtb_ipa = BOARD_LINUX_DTB_IPA,
            .pcpu_base = VCPUS_PER_VM,
            VM_VUART_CONFIG },
#endif
};
#endif
