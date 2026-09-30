/* SPDX-License-Identifier: TBD */
#ifndef HV_ARM64_DATA_ABORT_H
#define HV_ARM64_DATA_ABORT_H

#include <types.h>
#include <vm.h>

/*
 * Stage-2 data-abort entry point (EC = 0x24). Decodes ESR_EL2 ISS + HPFAR/FAR
 * into a struct mmio_access, dispatches it on the generic MMIO bus (<mmio.h>),
 * plumbs the transfer register, and on success advances regs->elr_el2 past
 * the faulting instruction.
 * Returns 0 if handled (caller erets back), non-zero if unhandled.
 */
int mmio_handle_data_abort(struct arch_regs *regs, u64 esr);

#endif /* HV_ARM64_DATA_ABORT_H */
