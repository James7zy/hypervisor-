/* SPDX-License-Identifier: TBD */
#ifndef HV_VTIMER_H
#define HV_VTIMER_H

/* Configure the EL1 virtual timer for the guest: let EL1 read the physical
 * counter/timer without trapping (CNTHCTL_EL2=0b11) and use a zero virtual
 * offset (CNTVOFF_EL2=0). Must be called before vm_run(). */
void vtimer_init(void);

#endif /* HV_VTIMER_H */
