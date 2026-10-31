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
     * state. TWI/TWE: trap guest wfi/wfe -- wfe passes through, wfi is
     * the B1 cooperative yield point. TID3: trap ID-group reads
     * (MIDR/MPIDR/ID_AA64*) --
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
    /* Blank the per-vCPU virtual interface on every load: every slice-A load
     * is a fresh-boot load (OFF->ON cycles included), so a re-ONed vCPU must
     * not inherit the dead context's ich_lr[]/VMCR. Lives here (not in the
     * generic run loops) because vgic_init is an arch symbol (ADR-0015) and
     * this is the pCPU-local point both loops share (gate-2 Important 2). */
    vgic_init(v);
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

void vcpu_arch_put(struct vcpu *v)
{
    /* M11 B1 minimal body: vGIC shadow only (same-VM switch shares Stage-2/
     * VTTBR/HCR, and EL1 sysregs are untouched until B2's bulk set). Same
     * pCPU as the coming vcpu_arch_resume by construction (run loops call
     * put(prev) immediately before loading next on THIS pCPU). */
    vgic_save(v);
    __asm__ volatile("dsb ish" ::: "memory");
}

/* Block-site save (M11 B2, gate-1 R1): the vCPU leaves the CPU WITHOUT a
 * switch happening — same shadow save as put. T3 splits the two: block
 * keeps the physical timer LIVE (it is the only wake source); put quiesces
 * it. A later resume (shared-pCPU wakeup) restores exactly what was saved
 * here. */
void vcpu_arch_block(struct vcpu *v)
{
    vgic_save(v);
    __asm__ volatile("dsb ish" ::: "memory");
}

void vcpu_arch_resume(struct vcpu *v)
{
    /* Already-launched vCPU re-entering THIS pCPU: shadow restore only. No
     * vgic_init blanking, no EL1 neutralize — the live guest's GIC state
     * and EL1 must survive the round trip (B1 boundary: same VM, same
     * pCPU, no remote shadow publishers — the svm_ctxsw profile; the
     * general runtime protocol incl. wake merging is slice B2). */
    vgic_restore(v);
}

void vcpu_arch_inject_irq(struct vcpu *v, u32 irq)
{
    /* The only injection source outside arch/ today is the vuart's SPI. */
    vgic_inject_spi(v, irq);
}
