/* SPDX-License-Identifier: TBD */
#include <types.h>
#include <vm.h>
#include <vgic.h>
#include <asm/sysreg.h>

void vgic_init(struct vcpu *vcpu)
{
    /* One-off (per PE): enable the EL2 sysreg interface and let EL1 use the
     * ICC_* interface. This is the only physical GIC register M2 touches. */
    SYSREG_WRITE(ICC_SRE_EL2, ICC_SRE_EL2_SRE | ICC_SRE_EL2_ENABLE);
    asm volatile("isb");

    /* Per-vCPU virtual interface: enabled, blank VMCR and list registers.
     * The guest programs its own VPMR/VENG1 via ICC_PMR_EL1/ICC_IGRPEN1_EL1.
     * TC=1 traps the guest's ICC_SGI1R_EL1 (IPI) writes to EL2 (M3.5). */
    vcpu->ich_hcr_el2  = ICH_HCR_EL2_EN | ICH_HCR_EL2_TC;
    vcpu->ich_vmcr_el2 = 0;
    vcpu->ich_lr[0] = 0;
    vcpu->ich_lr[1] = 0;
    vcpu->ich_lr[2] = 0;
    vcpu->ich_lr[3] = 0;
}

void vgic_inject_sw(struct vcpu *vcpu, u32 vintid, u8 prio)
{
    u64 lr = ICH_LR_STATE_PENDING | ICH_LR_GROUP1 |
             ((u64)prio << ICH_LR_PRIO_SHIFT) |
             ((u64)vintid & ICH_LR_VINTID_MASK);

    vcpu->ich_lr[0] = lr;
    /* Write the live register; the eret back to EL1 synchronises (no isb). */
    SYSREG_WRITE(ICH_LR0_EL2, lr);
}

void vgic_inject_hw(struct vcpu *vcpu, u32 vintid, u32 pintid, u8 prio)
{
    u64 lr = ICH_LR_STATE_PENDING | ICH_LR_HW | ICH_LR_GROUP1 |
             ((u64)prio << ICH_LR_PRIO_SHIFT) |
             ((u64)pintid << ICH_LR_PINTID_SHIFT) |
             ((u64)vintid & ICH_LR_VINTID_MASK);

    vcpu->ich_lr[0] = lr;
    /* Write the live register; the eret back to EL1 synchronises (no isb). */
    SYSREG_WRITE(ICH_LR0_EL2, lr);
}

/* Software SPI injection into LR1 (the vtimer owns LR0 and is re-injected
 * every tick, so sharing LR0 would clobber this before the guest takes it).
 *
 * HW=0: as of M5 slice 2, EL2 owns and fully drains+deactivates the physical
 * PL011 IRQ itself (irq_handler.c) before this is ever called, so there is no
 * physical Active state left for an HW=1 LR to release via the deactivate
 * linkage -- using HW=1 here (as the old passthrough design did, when the
 * physical line was deliberately left Active for exactly that linkage) mints
 * a virtual/physical mismatch that wedges after the first couple of
 * injections. Purely-software Group-1 injection, same shape as
 * vgic_inject_sw/vgic_inject_sgi. LR1 is saved/restored by
 * vgic_{save,restore}. */
void vgic_inject_spi(struct vcpu *vcpu, u32 intid)
{
    u64 lr = ICH_LR_STATE_PENDING | ICH_LR_GROUP1 |
             ((u64)0xA0 << ICH_LR_PRIO_SHIFT) |
             ((u64)intid & ICH_LR_VINTID_MASK);

    vcpu->ich_lr[1] = lr;
    SYSREG_WRITE(ICH_LR1_EL2, lr);
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

    vcpu->ich_lr[2] = lr;
    SYSREG_WRITE(ICH_LR2_EL2, lr);
}

void vgic_restore(struct vcpu *vcpu)
{
    SYSREG_WRITE(ICH_HCR_EL2,  vcpu->ich_hcr_el2);
    SYSREG_WRITE(ICH_VMCR_EL2, vcpu->ich_vmcr_el2);
    SYSREG_WRITE(ICH_LR0_EL2,  vcpu->ich_lr[0]);
    SYSREG_WRITE(ICH_LR1_EL2,  vcpu->ich_lr[1]);
    SYSREG_WRITE(ICH_LR2_EL2,  vcpu->ich_lr[2]);
    SYSREG_WRITE(ICH_LR3_EL2,  vcpu->ich_lr[3]);
    asm volatile("isb");
}

/* Symmetric save half. M2's single-vCPU flow never reschedules, so this has
 * no caller yet; M2.5's timer context switch is the first user. It is real
 * (non-stub) code kept paired with vgic_restore per spec §4.2. */
void vgic_save(struct vcpu *vcpu)
{
    vcpu->ich_hcr_el2  = SYSREG_READ(ICH_HCR_EL2);
    vcpu->ich_vmcr_el2 = SYSREG_READ(ICH_VMCR_EL2);
    vcpu->ich_lr[0] = SYSREG_READ(ICH_LR0_EL2);
    vcpu->ich_lr[1] = SYSREG_READ(ICH_LR1_EL2);
    vcpu->ich_lr[2] = SYSREG_READ(ICH_LR2_EL2);
    vcpu->ich_lr[3] = SYSREG_READ(ICH_LR3_EL2);
}
