/* SPDX-License-Identifier: TBD */
#ifndef HV_VGIC_H
#define HV_VGIC_H

#include <vm.h>   /* struct vcpu */

/* ICC_SRE_EL2: enable the EL2 system-register interface and permit EL1 to
 * use ICC_SRE_EL1. */
#define ICC_SRE_EL2_SRE     (1ULL << 0)
#define ICC_SRE_EL2_ENABLE  (1ULL << 3)

/* ICH_HCR_EL2: virtual CPU interface enable + SGI trap control. */
#define ICH_HCR_EL2_EN      (1ULL << 0)
#define ICH_HCR_EL2_TC      (1ULL << 10)   /* trap guest ICC_SGI*R_EL1 to EL2 */

/* ICH_LR<n>_EL2 fields (GICv3, 64-bit list register). */
#define ICH_LR_STATE_PENDING (1ULL << 62)   /* State[63:62] = 0b01 (Pending) */
#define ICH_LR_HW            (1ULL << 61)    /* 0 = software injection      */
#define ICH_LR_GROUP1        (1ULL << 60)    /* Group 1                     */
#define ICH_LR_PRIO_SHIFT    48              /* Priority[55:48]             */
#define ICH_LR_PINTID_SHIFT  32              /* pINTID[44:32] (HW=1)        */
#define ICH_LR_VINTID_MASK   0xFFFFFFFFULL   /* vINTID[31:0]                */

/* Enable the virtual CPU interface and blank per-vCPU vGIC state. */
void vgic_init(struct vcpu *vcpu);

/* Inject a pending, Group-1, software (HW=0) virtual interrupt via ICH_LR0.
 * Writes the live register so the vIRQ is presented on the next eret to EL1. */
void vgic_inject_sw(struct vcpu *vcpu, u32 vintid, u8 prio);

/* Inject a pending, Group-1, hardware-forwarded (HW=1) virtual interrupt via
 * ICH_LR0: the physical INTID is linked in pINTID so the guest's deactivate of
 * the virtual IRQ releases the physical one. See docs/adr/0001-*. */
void vgic_inject_hw(struct vcpu *vcpu, u32 vintid, u32 pintid, u8 prio);

/* Inject a Shared Peripheral Interrupt (SPI, INTID >= 32) into the guest as a
 * software (HW=0) virtual interrupt via the list registers. Used by emulated
 * devices (M3.3 virtio-console: SPI 48) that have no physical GIC line. Thin
 * wrapper over vgic_inject_sw with a device-class priority. */
void vgic_inject_spi(struct vcpu *vcpu, u32 intid);

/* Inject a virtual SGI (INTID 0..15) via ICH_LR2 (LR0=vtimer, LR1=PL011).
 * Software (HW=0) Group-1, used by the cross-core IPI path (vgic_sgi.c). */
void vgic_inject_sgi(struct vcpu *vcpu, u32 vintid);

/* Save/restore the virtual interface state to/from struct vcpu. */
void vgic_save(struct vcpu *vcpu);
void vgic_restore(struct vcpu *vcpu);

#endif /* HV_VGIC_H */
