/* SPDX-License-Identifier: TBD */
#ifndef HV_VM_CONFIG_H
#define HV_VM_CONFIG_H

#include <types.h>
#include <board.h>

struct vm_config {
    u32       vmid;
    uintptr_t entry;       /* guest IPA of the kernel entry point        */
    uintptr_t mem_base;    /* guest IPA of RAM base                      */
    uintptr_t ram_pa;      /* physical address backing mem_base (Stage-2)*/
    size_t    mem_size;
    uintptr_t dtb_ipa;     /* guest IPA where the DTB is visible (x0)    */
};

static const struct vm_config svm_config = {
    .vmid     = 1,
    .entry    = BOARD_SVM_ENTRY,
    .mem_base = BOARD_SVM_MEM_BASE,
    .ram_pa   = BOARD_SVM_MEM_BASE,   /* identity-mapped */
    .mem_size = BOARD_SVM_MEM_SIZE,
    .dtb_ipa  = 0,                    /* SVM takes no DTB */
};

static const struct vm_config linux_config = {
    .vmid     = 1,
    .entry    = BOARD_LINUX_IMAGE_PA - BOARD_LINUX_RAM_PA + BOARD_LINUX_RAM_IPA,
    .mem_base = BOARD_LINUX_RAM_IPA,
    .ram_pa   = BOARD_LINUX_RAM_PA,
    .mem_size = BOARD_LINUX_RAM_SIZE,
    .dtb_ipa  = BOARD_LINUX_DTB_IPA,
};

#endif /* HV_VM_CONFIG_H */
