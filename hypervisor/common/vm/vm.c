/* SPDX-License-Identifier: TBD */
#include <types.h>
#include <printk.h>
#include <vm.h>
#include "vm_config.h"
#include "../../../arch/arm64/mmu/stage2.h"

/* Non-static: vmexit_asm.S references g_vm by symbol */
struct vm g_vm;

void vm_init(void)
{
    g_vm.config = &svm_config;

    /*
     * SPSR_EL2 = 0x3C5:
     *   M[4:0] = 0b00101 = EL1h (use SP_EL1)
     *   DAIF   = 0b1111  (bits[9:6], all interrupts masked)
     */
    g_vm.vcpu.regs.elr_el2  = svm_config.entry;
    g_vm.vcpu.regs.spsr_el2 = 0x3C5ULL;
    g_vm.vcpu.regs.sp_el1   = svm_config.mem_base + svm_config.mem_size - 0x10UL;

    /*
     * HCR_EL2: VM(0)|FMO(3)|IMO(4)|AMO(5)|RW(31) set; HCD(29) clear (allow HVC).
     * RW=1: EL1 executes in AArch64 state; without it eret to EL1h is illegal.
     */
    g_vm.vcpu.hcr_el2 = (1ULL << 0) | (1ULL << 3) | (1ULL << 4) | (1ULL << 5) |
                        (1ULL << 31);

    stage2_init(&g_vm.vcpu, (u32)svm_config.vmid);

    printk("[hv] SVM: launching VMID=%u entry=0x%lx\n",
           (unsigned)svm_config.vmid, svm_config.entry);
}

void vm_run(void)
{
    stage2_activate(&g_vm.vcpu);
    vcpu_run(&g_vm.vcpu);
    /* Returns here after hv_restore() is called from HVC handler */
}
