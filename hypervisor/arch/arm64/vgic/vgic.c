/* SPDX-License-Identifier: TBD */
#include <types.h>
#include <vm.h>
#include <vm_config.h>
#include <percpu.h>
#include <vgic.h>
#include <gic_v3.h>
#include <arch/sysreg.h>

/* Pre-entry only: IMSC starts zero and only the target guest can enable it
 * after its sole restore. Early RX cannot publish LR1. Never reset spi_lock. */
void vgic_init(struct vcpu *vcpu)
{
    /* One-off (per PE): enable the EL2 sysreg interface and let EL1 use the
     * ICC_* interface. This is the only physical GIC register M2 touches. */
    SYSREG_WRITE(ICC_SRE_EL2, ICC_SRE_EL2_SRE | ICC_SRE_EL2_ENABLE);
    asm volatile("isb");

    /* Per-vCPU virtual interface: enabled, blank VMCR and list registers.
     * The guest programs its own VPMR/VENG1 via ICC_PMR_EL1/ICC_IGRPEN1_EL1.
     * TC=1 traps the guest's ICC_SGI1R_EL1 (IPI) writes to EL2 (M3.5). */
    vcpu->arch.ich_hcr_el2  = ICH_HCR_EL2_EN | ICH_HCR_EL2_TC;
    vcpu->arch.ich_vmcr_el2 = 0;
    vcpu->arch.ich_lr[0] = 0;
    vcpu->arch.ich_lr[1] = 0;
    vcpu->arch.ich_lr[2] = 0;
    vcpu->arch.ich_lr[3] = 0;
    vcpu->arch.spi_shadow_pending = false;
}

void vgic_inject_sw(struct vcpu *vcpu, u32 vintid, u8 prio)
{
    u64 lr = ICH_LR_STATE_PENDING | ICH_LR_GROUP1 |
             ((u64)prio << ICH_LR_PRIO_SHIFT) |
             ((u64)vintid & ICH_LR_VINTID_MASK);

    vcpu->arch.ich_lr[0] = lr;
    /* Write the live register; the eret back to EL1 synchronises (no isb). */
    SYSREG_WRITE(ICH_LR0_EL2, lr);
}

void vgic_inject_hw(struct vcpu *vcpu, u32 vintid, u32 pintid, u8 prio)
{
    u64 lr = ICH_LR_STATE_PENDING | ICH_LR_HW | ICH_LR_GROUP1 |
             ((u64)prio << ICH_LR_PRIO_SHIFT) |
             ((u64)pintid << ICH_LR_PINTID_SHIFT) |
             ((u64)vintid & ICH_LR_VINTID_MASK);

    vcpu->arch.ich_lr[0] = lr;
    /* Write the live register; the eret back to EL1 synchronises (no isb). */
    SYSREG_WRITE(ICH_LR0_EL2, lr);
}

/* Shared encoding for LR1 (the PL011 SPI list register): pending, Group-1,
 * software (HW=0), fixed device-class priority 0xA0. Single place that builds
 * this bit pattern for both local and remote vgic_inject_spi delivery. */
static u64 vgic_spi_lr_encode(u32 intid)
{
    return ICH_LR_STATE_PENDING | ICH_LR_GROUP1 |
           ((u64)0xA0 << ICH_LR_PRIO_SHIFT) |
           ((u64)intid & ICH_LR_VINTID_MASK);
}

/* Requires spi_lock; publishing the payload and pending is one operation. */
static void vgic_set_spi_shadow_locked(struct vcpu *vcpu, u32 intid)
{
    vcpu->arch.ich_lr[1] = vgic_spi_lr_encode(intid);
    vcpu->arch.spi_shadow_pending = true;
}

/* Software SPI injection into LR1 (the vtimer owns LR0 and is re-injected
 * every tick, so sharing LR0 would clobber this before the guest takes it).
 *
 * HW=0: EL2 owns the physical PL011, drains it and deactivates its IRQ in
 * irq_handler.c; guest completion must not release physical Active state.
 * Only the current target can use this PE's live LR1. Its live write and
 * pending clear stay under spi_lock so a later unrelated kick cannot replay
 * the shadow or clear a newer publication. Remote publication uses the same
 * lock, then orders the shadow before notifying the statically pinned PE. */
void vgic_inject_spi(struct vcpu *target, u32 intid)
{
    bool local = target == current_vcpu();
    u32 pcpu = target->owner->config->pcpu_base + target->vcpu_idx;

    spin_lock(&target->arch.spi_lock);
    vgic_set_spi_shadow_locked(target, intid);
    if (local) {
        SYSREG_WRITE(ICH_LR1_EL2, target->arch.ich_lr[1]);
        target->arch.spi_shadow_pending = false;
    }
    spin_unlock(&target->arch.spi_lock);

    if (!local) {
        asm volatile("dsb ish" ::: "memory");
        gic_kick_pcpu(pcpu);
    }
}

/* Cross-core companion to vgic_inject_spi: reload ICH_LR1_EL2 on the CALLING
 * pCPU from its own current vCPU's shadow ich_lr[1], but ONLY if
 * remote vgic_inject_spi() left something freshly pending for this
 * vCPU. Test-and-clear on spi_shadow_pending: called unconditionally on
 * every kick-SGI (el2_irq_handler), including ones that are ordinary
 * SGI/IPI traffic with nothing to do with the console, so without this gate
 * a stale shadow (already consumed by the guest, never re-armed) would be
 * blindly replayed into the live register and could re-present an
 * already-handled PL011 interrupt. */
void vgic_reload_spi_lr(struct vcpu *vcpu)
{
    spin_lock(&vcpu->arch.spi_lock);
    if (vcpu->arch.spi_shadow_pending) {
        SYSREG_WRITE(ICH_LR1_EL2, vcpu->arch.ich_lr[1]);
        vcpu->arch.spi_shadow_pending = false;
    }
    spin_unlock(&vcpu->arch.spi_lock);
}

/* Inject a virtual SGI (INTID 0..15) via ICH_LR2. LR0 is the vtimer and LR1 is
 * the PL011 SPI, both re-injected on their own cadence; SGIs get their own LR
 * so a pending IPI is not clobbered. Software (HW=0) Group-1. Used by the
 * cross-core IPI path (vgic_sgi.c) on the *target* core. */
void vgic_inject_sgi(struct vcpu *vcpu, u32 vintid)
{
    u64 lr = ICH_LR_STATE_PENDING | ICH_LR_GROUP1 |
             ((u64)0xA0 << ICH_LR_PRIO_SHIFT) |
             ((u64)vintid & ICH_LR_VINTID_MASK);

    vcpu->arch.ich_lr[2] = lr;
    SYSREG_WRITE(ICH_LR2_EL2, lr);
}

/* Only initial entry in vm_run/secondary_main, before guest IMSC enable.
 * Static pinning never restores on a runtime re-entry. M11 must re-audit. */
void vgic_restore(struct vcpu *vcpu)
{
    SYSREG_WRITE(ICH_HCR_EL2,  vcpu->arch.ich_hcr_el2);
    SYSREG_WRITE(ICH_VMCR_EL2, vcpu->arch.ich_vmcr_el2);
    SYSREG_WRITE(ICH_LR0_EL2,  vcpu->arch.ich_lr[0]);
    SYSREG_WRITE(ICH_LR1_EL2,  vcpu->arch.ich_lr[1]);
    SYSREG_WRITE(ICH_LR2_EL2,  vcpu->arch.ich_lr[2]);
    SYSREG_WRITE(ICH_LR3_EL2,  vcpu->arch.ich_lr[3]);
    asm volatile("isb");
}

/* No callers under static pinning. NOT safe for runtime scheduling: even a
 * lock would not prevent a live read overwriting a remotely published shadow.
 * Future save/restore users must redesign that protocol, not just add a lock. */
void vgic_save(struct vcpu *vcpu)
{
    vcpu->arch.ich_hcr_el2  = SYSREG_READ(ICH_HCR_EL2);
    vcpu->arch.ich_vmcr_el2 = SYSREG_READ(ICH_VMCR_EL2);
    vcpu->arch.ich_lr[0] = SYSREG_READ(ICH_LR0_EL2);
    vcpu->arch.ich_lr[1] = SYSREG_READ(ICH_LR1_EL2);
    vcpu->arch.ich_lr[2] = SYSREG_READ(ICH_LR2_EL2);
    vcpu->arch.ich_lr[3] = SYSREG_READ(ICH_LR3_EL2);
}
