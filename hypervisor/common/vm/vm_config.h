/* SPDX-License-Identifier: TBD */
#ifndef HV_VM_CONFIG_H
#define HV_VM_CONFIG_H

#include <types.h>
#include <board.h>
#include <vm.h>       /* NR_VMS */

struct vm_config {
    u32       vmid;
    uintptr_t entry;       /* guest IPA of the kernel entry point        */
    uintptr_t mem_base;    /* guest IPA of RAM base                      */
    uintptr_t ram_pa;      /* physical address backing mem_base (Stage-2)*/
    size_t    mem_size;
    uintptr_t dtb_ipa;     /* guest IPA where the DTB is visible (x0)    */
    u8        pcpu_base;   /* first pCPU of the VM's static slot; vCPU i
                              runs on pCPU pcpu_base + i */
};

#ifdef CONFIG_GUEST_SVM
static const struct vm_config vm_configs[NR_VMS] = {
    [0] = { .vmid = 1, .entry = BOARD_SVM_ENTRY, .mem_base = BOARD_SVM_MEM_BASE,
            .ram_pa = BOARD_SVM_MEM_BASE, .mem_size = BOARD_SVM_MEM_SIZE,
            .dtb_ipa = 0, .pcpu_base = 0 },
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
            .dtb_ipa = 0, .pcpu_base = VCPUS_PER_VM },
#endif
};
#else
static const struct vm_config vm_configs[NR_VMS] = {
    [0] = { .vmid = 1,
            .entry = BOARD_LINUX_IMAGE_PA - BOARD_LINUX_RAM_PA + BOARD_LINUX_RAM_IPA,
            .mem_base = BOARD_LINUX_RAM_IPA, .ram_pa = BOARD_LINUX_RAM_PA,
            .mem_size = BOARD_LINUX_RAM_SIZE, .dtb_ipa = BOARD_LINUX_DTB_IPA,
            .pcpu_base = 0 },
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
            .pcpu_base = VCPUS_PER_VM },
#endif
};
#endif

#endif /* HV_VM_CONFIG_H */
