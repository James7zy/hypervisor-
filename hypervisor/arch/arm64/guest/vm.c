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
    v->regs.x[0] = (u64)cfg->bootinfo_ipa;
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
     * HCR_EL2: VM(0)|FMO(3)|IMO(4)|AMO(5)|TWI(13)|TWE(14)|TID3(18)|TSC(19)|
     * RW(31) set; HCD(29) clear (allow HVC). RW=1: EL1 executes in AArch64
     * state. TWI/TWE: trap guest wfi/wfe -- handler passes them through (no
     * scheduler yet). TID3: trap ID-group reads (MIDR/MPIDR/ID_AA64*) --
     * handler forwards the value the guest saw before the trap existed.
     * TSC: trap guest smc -- handler returns SMCCC_NOT_SUPPORTED instead of
     * letting it reach EL3 (ADR-0017; no-op on QEMU, which forces TSC RES0
     * without EL3, but live on RK3588's EL3/TF-A).
     */
    v->arch.hcr_el2 = (1ULL << 0) | (1ULL << 3) | (1ULL << 4) | (1ULL << 5) |
                      (1ULL << 13) | (1ULL << 14) | (1ULL << 18) |
                      (1ULL << 19) | (1ULL << 31);

    vgic_init(v);
}

void vcpu_arch_load(struct vcpu *v)
{
    /* Virtual MPIDR: Aff0 = VM-local vCPU index. */
    SYSREG_WRITE(VMPIDR_EL2, (u64)v->vcpu_idx);
    __asm__ volatile("isb");

    /*
     * Fresh-boot EL1 CPU state. Every caller loads a RUNNABLE vCPU (first
     * boot or a PSCI CPU_ON re-authorization), and EL1 sysregs are not
     * saved across CPU_OFF: a parked vCPU leaves its guest's live SCTLR_EL1
     * (MMU on, the boot idmap long uninstalled) on this pCPU. Eretting to
     * the CPU_ON physical entry with that state faults on the very first
     * fetch — level-0 IABT at the entry address (M11 slice-A hotplug
     * re-online gate crash). Warm-reset equivalent: MMU/caches off, the
     * dead context's stage-1 TLB entries and I-cache lines dropped
     * (current VMID only; Stage-2 stays — a sibling vCPU still runs on
     * it). SCTLR_EL1 has no RES1 bits, so 0 is a legal reset value.
     */
    SYSREG_WRITE(SCTLR_EL1, 0ULL);
    __asm__ volatile("isb\n\ttlbi vmalle1\n\tic iallu\n\tdsb nsh\n\tisb"
                     ::: "memory");

    /* Required order: Stage-2 activate BEFORE vGIC restore. */
    stage2_activate(v);
    vgic_restore(v);
}

void vcpu_arch_inject_irq(struct vcpu *v, u32 irq)
{
    /* The only injection source outside arch/ today is the vuart's SPI. */
    vgic_inject_spi(v, irq);
}
