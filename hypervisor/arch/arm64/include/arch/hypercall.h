/* SPDX-License-Identifier: TBD */
#ifndef HV_HYPERCALL_H
#define HV_HYPERCALL_H

#define SMCCC_NOT_SUPPORTED  (~0ULL)
/* Hypervisor's own (vendor) hypercalls live in the SMCCC vendor-specific
 * service range 0x87000000+ (DEN 0028 B). 0x80000001 was the old value and
 * collides with SMCCC_ARCH_FEATURES once pre-B answers it. */
#define HVC_VENDOR_BASE      0x87000000U
#define HC_GUEST_DONE        (HVC_VENDOR_BASE | 0x0001U)

/* SMCCC v1.x base functions (ARM DEN 0028 A). Answered in handle_hvc before
 * the PSCI dispatch: their service ranges overlap the 0x80/0xC0 calling
 * convention prefixes. */
#define SMCCC_VERSION           0x80000000U
#define SMCCC_ARCH_FEATURES     0x80000001U
#define SMCCC_VERSION_1_1       0x00010001U   /* major<<16 | minor */

#endif /* HV_HYPERCALL_H */
