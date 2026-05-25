/* SPDX-License-Identifier: TBD */
#ifndef HV_HYPERCALL_H
#define HV_HYPERCALL_H

#define SMCCC_NOT_SUPPORTED  (~0ULL)
#define HVC_VENDOR_BASE      0x80000000U
#define HC_GUEST_DONE        (HVC_VENDOR_BASE | 0x0001U)

#endif /* HV_HYPERCALL_H */
