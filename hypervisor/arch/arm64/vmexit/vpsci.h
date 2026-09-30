/* SPDX-License-Identifier: TBD */
#ifndef HV_VPSCI_H
#define HV_VPSCI_H

#include <vm.h>   /* struct arch_regs */

/* Dispatch a PSCI call. regs->x[0] holds the function ID on entry; the
 * result (for calls that return) is written back into regs->x[0]. The
 * power-down calls (CPU_OFF/SYSTEM_OFF/SYSTEM_RESET) do not return. */
void psci_handle(struct arch_regs *regs);

#endif /* HV_VPSCI_H */
