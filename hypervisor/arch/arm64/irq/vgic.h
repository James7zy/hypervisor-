/* SPDX-License-Identifier: TBD */
#ifndef HV_VGIC_H
#define HV_VGIC_H

#include <vm.h>   /* struct vcpu */

/* ICC_SRE_EL2: enable the EL2 system-register interface and permit EL1 to
 * use ICC_SRE_EL1. */
#define ICC_SRE_EL2_SRE     (1ULL << 0)
#define ICC_SRE_EL2_ENABLE  (1ULL << 3)

/* ICH_HCR_EL2: virtual CPU interface enable. */
#define ICH_HCR_EL2_EN      (1ULL << 0)

/* ICH_LR<n>_EL2 fields (GICv3, 64-bit list register). */
#define ICH_LR_STATE_PENDING (1ULL << 62)   /* State[63:62] = 0b01 (Pending) */
#define ICH_LR_HW            (1ULL << 61)    /* 0 = software injection      */
#define ICH_LR_GROUP1        (1ULL << 60)    /* Group 1                     */
#define ICH_LR_PRIO_SHIFT    48              /* Priority[55:48]             */
#define ICH_LR_VINTID_MASK   0xFFFFFFFFULL   /* vINTID[31:0]                */

/* Enable the virtual CPU interface and blank per-vCPU vGIC state. */
void vgic_init(struct vcpu *vcpu);

/* Inject a pending, Group-1, software (HW=0) virtual interrupt via ICH_LR0.
 * Writes the live register so the vIRQ is presented on the next eret to EL1. */
void vgic_inject_sw(struct vcpu *vcpu, u32 vintid, u8 prio);

/* Save/restore the virtual interface state to/from struct vcpu. */
void vgic_save(struct vcpu *vcpu);
void vgic_restore(struct vcpu *vcpu);

#endif /* HV_VGIC_H */
