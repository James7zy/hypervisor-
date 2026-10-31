/* SPDX-License-Identifier: TBD */
#include <sched.h>   /* sched_wake (M11 B2) */
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
/* Live-pending probe for the WFI three-way (M11 B2, gate-1 R3): MUST read
 * the live ICH_LRx_EL2 state bits, never the shadow arrays — HW=1 LRs
 * (the vtimer PPI via LR0, ADR-0001) gain pending state in hardware from
 * the physical line, and the guest's IAR/EOI/DIR advance LR state with no
 * shadow update; the shadow diverges by design (that is why vgic_save
 * exists). A stale shadow would read "pending" forever after the guest's
 * first EOI and every wfi would take the resume branch: rotation starves
 * and idle never blocks. */
bool vgic_has_pending(struct vcpu *vcpu)
{
    (void)vcpu;   /* the live LRs belong to THIS pCPU == the caller's */
    for (int i = 0; i < 4; i++) {
        u64 lr;
        switch (i) {
        case 0: lr = SYSREG_READ(ICH_LR0_EL2); break;
        case 1: lr = SYSREG_READ(ICH_LR1_EL2); break;
        case 2: lr = SYSREG_READ(ICH_LR2_EL2); break;
        default: lr = SYSREG_READ(ICH_LR3_EL2); break;
        }
        /* State bits are [63:62]: 01=pending, 10=active, 11=both. Pending
         * only — an active-without-pending LR must NOT wake a wfi (the
         * interrupt is mid-service). Bit 0 is the vINTID's LSB: an odd
         * vINTID (27!) there made the first cut read "pending" forever. */
        if (((lr >> 62) & 1ULL) != 0ULL)
            return true;
    }
    return false;
}

void vgic_inject_spi(struct vcpu *target, u32 intid)
{
    bool local = target == current_vcpu();
    u32 pcpu = target->owner->config->vcpu_pcpu[target->vcpu_idx];

    spin_lock(&target->arch.spi_lock);
    vgic_set_spi_shadow_locked(target, intid);
    if (local) {
        SYSREG_WRITE(ICH_LR1_EL2, target->arch.ich_lr[1]);
        target->arch.spi_shadow_pending = false;
    }
    spin_unlock(&target->arch.spi_lock);

    if (!local && target->state != VCPU_OFF) {
        asm volatile("dsb ish" ::: "memory");
        gic_kick_pcpu(pcpu);
    }
    sched_wake(target);   /* no-op unless BLOCKED (M11 B2, gate-1 R6) */
    /* OFF target (post-SYSTEM_OFF console focus, M11 slice A): no kick.
     * The pCPU sits in its run loop's OFF-wait with EL2 IRQs masked; a kick
     * would pend forever and make every wfi return immediately — a permanent
     * busy-spin on a dead VM's keystrokes. The shadow publication above is
     * terminal-state garbage either way: nothing re-ONs a SYSTEM_OFF'd VM,
     * and a CPU_OFF'd vCPU's re-ON wipes LRs via vgic_init. */
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

/* Initial entry (vcpu_arch_load) AND slice-B1 same-pCPU switch re-entry
 * (vcpu_arch_resume; svm_ctxsw profile — no remote shadow publishers there).
 * The runtime protocol with concurrent remote publishers (shadow-vs-live-LR
 * merge on wake) remains slice B2 (spec 8.1/D13). */
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

/* M11 B2 runtime protocol (spec 8.1/D13): PUBLISHER WINS. Called by
 * vcpu_arch_put/block on switch-away or block — possibly while a remote
 * pCPU has just published a fresh vuart SPI into our LR1 shadow
 * (vgic_inject_spi under spi_lock + spi_shadow_pending). Reading the live
 * LR1 here would overwrite that publication with the stale pre-publish
 * hardware value and lose the interrupt. Merge rule: if the publisher's
 * flag is set, keep the SHADOW (it is newer than the hardware); otherwise
 * the hardware is the only writer — read it. The whole merge sits under
 * spi_lock so a publisher cannot slip between flag test and shadow use. */
void vgic_save(struct vcpu *vcpu)
{
    vcpu->arch.ich_hcr_el2  = SYSREG_READ(ICH_HCR_EL2);
    vcpu->arch.ich_vmcr_el2 = SYSREG_READ(ICH_VMCR_EL2);
    vcpu->arch.ich_lr[0] = SYSREG_READ(ICH_LR0_EL2);
    vcpu->arch.ich_lr[2] = SYSREG_READ(ICH_LR2_EL2);
    vcpu->arch.ich_lr[3] = SYSREG_READ(ICH_LR3_EL2);

    spin_lock(&vcpu->arch.spi_lock);
    if (!vcpu->arch.spi_shadow_pending)
        vcpu->arch.ich_lr[1] = SYSREG_READ(ICH_LR1_EL2);
    /* else: keep the shadow — the remote publication wins over the live
     * register (which still holds the pre-publish value). */
    spin_unlock(&vcpu->arch.spi_lock);
}
