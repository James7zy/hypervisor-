/* SPDX-License-Identifier: TBD */
#ifndef HV_VGIC_SGI_H
#define HV_VGIC_SGI_H

#include <types.h>

/* Sender pCPU: decode a guest ICC_SGI1R_EL1 write, mark target bitmaps, kick. */
void vgic_sgi_trap(u64 sgi1r);

/* Target pCPU: drain this core's pending SGI bitmap into its LRs. */
void vgic_sgi_drain(u32 cpu);

#endif /* HV_VGIC_SGI_H */
