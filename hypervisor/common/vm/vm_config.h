/* SPDX-License-Identifier: TBD */
#ifndef HV_VM_CONFIG_H
#define HV_VM_CONFIG_H

#include <types.h>
#include <board.h>

struct vm_config {
    u32       vmid;
    uintptr_t entry;
    uintptr_t mem_base;
    size_t    mem_size;
};

static const struct vm_config svm_config = {
    .vmid     = 1,
    .entry    = BOARD_SVM_ENTRY,
    .mem_base = BOARD_SVM_MEM_BASE,
    .mem_size = BOARD_SVM_MEM_SIZE,
};

#endif /* HV_VM_CONFIG_H */
