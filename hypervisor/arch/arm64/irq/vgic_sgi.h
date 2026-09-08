/* SPDX-License-Identifier: TBD */
#ifndef HV_VGIC_SGI_H
#define HV_VGIC_SGI_H

#include <types.h>

struct vm;

/* Sender pCPU: decode a guest ICC_SGI1R_EL1 write, mark target bitmaps, kick. */
void vgic_sgi_trap(u64 sgi1r);

/* Target pCPU: drain this core's pending SGI bitmap into its LRs. */
void vgic_sgi_drain(u32 cpu);

/* Force every ONLINE pCPU of VM `m` other than `caller_pcpu` into EL2 via the
 * physical kick SGI, without marking anything pending in the SGI bitmap (no
 * virtual SGI is meant to be delivered — the target is expected to notice
 * m->off and park, see el2_irq_handler's kick-SGI branch). Used by
 * psci_power_down (M5 slice 3) to force-park the rest of a VM being powered
 * down. */
void vgic_kick_vm_other_pcpus(struct vm *m, u32 caller_pcpu);

#endif /* HV_VGIC_SGI_H */
