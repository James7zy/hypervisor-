/* SPDX-License-Identifier: TBD */
/*
 * arm64 implementation of the generic VM/vCPU hooks (ADR-0015). The generic
 * VM lifecycle lives in common/vm/vm.c; everything it needs from the
 * architecture comes through these functions.
 */
#include <types.h>
#include <vm.h>
#include <vm_config.h>
#include <arch/sysreg.h>
#include "../mmu/stage2.h"
#include <vgic.h>
#include <vgic_v3_mmio.h>

void vm_arch_init(struct vm *m)
{
    stage2_init(m);
}

void vm_arch_devices_init(void)
{
    vgicv3_mmio_init();
}

void vcpu_arch_reset(struct vcpu *v, const struct vm_config *cfg)
{
    /*
     * arm64 Linux boot protocol (Documentation/arm64/booting.rst):
     *   x0 = physical address of the DTB (here: guest IPA of the DTB)
     *   x1 = x2 = x3 = 0 (reserved, must be zero)
     *   PC = kernel entry; CPU in EL1h, DAIF masked, MMU/caches off.
     */
    v->regs.x[0] = (u64)cfg->dtb_ipa;
    v->regs.x[1] = 0;
    v->regs.x[2] = 0;
    v->regs.x[3] = 0;

    /*
     * SPSR_EL2 = 0x3C5: M[4:0]=00101 (EL1h, SP_EL1), DAIF=1111 (all masked).
     */
    v->regs.elr_el2  = cfg->entry;
    v->regs.spsr_el2 = 0x3C5ULL;
    v->regs.sp_el1   = cfg->mem_base + cfg->mem_size - 0x10UL;

    /*
     * HCR_EL2: VM(0)|FMO(3)|IMO(4)|AMO(5)|RW(31) set; HCD(29) clear (allow HVC).
     * RW=1: EL1 executes in AArch64 state.
     */
    v->arch.hcr_el2 = (1ULL << 0) | (1ULL << 3) | (1ULL << 4) | (1ULL << 5) |
                      (1ULL << 31);

    vgic_init(v);
}

void vcpu_arch_load(struct vcpu *v)
{
    /* Virtual MPIDR: Aff0 = VM-local vCPU index. */
    SYSREG_WRITE(VMPIDR_EL2, (u64)v->vcpu_idx);
    __asm__ volatile("isb");

    /* Required order: Stage-2 activate BEFORE vGIC restore. */
    stage2_activate(v);
    vgic_restore(v);
}
