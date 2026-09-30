/* SPDX-License-Identifier: TBD */
#ifndef HV_VM_CONFIG_H
#define HV_VM_CONFIG_H

#include <types.h>
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
    uintptr_t vuart_base;  /* guest-physical base of the emulated console
                              UART (dm/vuart.c); same in every VM (unified
                              guest address map, ADR-0014) */
    u32       vuart_irq;   /* virtual INTID the vuart raises in this VM */
};

/*
 * The static VM configuration table (ADR-0008), defined in vm_configs.c --
 * the one file outside arch/ allowed to read BOARD_* (ADR-0015). This
 * header stays board-free so dm/ and common code can use struct vm_config.
 */
extern const struct vm_config vm_configs[NR_VMS];

#endif /* HV_VM_CONFIG_H */
