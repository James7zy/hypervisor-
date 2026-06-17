/* SPDX-License-Identifier: TBD */
#ifndef HV_ARM64_MMIO_H
#define HV_ARM64_MMIO_H

#include <types.h>
#include <vm.h>

/*
 * A decoded Stage-2 MMIO access handed to a device handler.
 *
 *   ipa      - faulting guest IPA (byte address, FAR offset already merged)
 *   offset   - ipa - region base; what most device handlers actually use
 *   size     - access width in bytes: 1, 2, 4, or 8
 *   is_write - true for a store (guest -> device), false for a load
 *   data     - WRITE: value the guest stored (filled in before the handler).
 *              READ:  value the device returns (handler fills it in).
 */
struct mmio_access {
    u64  ipa;
    u64  offset;
    u8   size;
    bool is_write;
    u64  data;
};

/*
 * Device handler. Receives the decoded access and the ctx registered with the
 * region. Returns 0 on success, non-zero to signal an unhandled/failed access
 * (which the abort path treats like "no handler": diagnostic + park).
 */
typedef int (*mmio_handler_t)(struct mmio_access *acc, void *ctx);

/*
 * Register a device over the IPA range [base, base + len). Later milestones
 * (M3.2 vGIC, M3.3 virtio) call this. Returns 0 on success, -1 if the bus is
 * full. Ranges are not checked for overlap (single-author bus, M3.x).
 */
int mmio_bus_register(u64 base, u64 len, mmio_handler_t handler, void *ctx);

/*
 * Stage-2 data-abort entry point (EC = 0x24). Decodes ESR_EL2 ISS + HPFAR/FAR,
 * dispatches to a registered handler, plumbs the transfer register, and on
 * success advances regs->elr_el2 past the faulting instruction.
 * Returns 0 if handled (caller erets back), non-zero if unhandled.
 */
int mmio_handle_data_abort(struct vcpu_regs *regs, u64 esr);

/* M3.1 scaffold (TEMPORARY — removed in M3.2). Registers a GICD-range test
 * device that prints decoded accesses. */
void mmio_scaffold_init(void);

#endif /* HV_ARM64_MMIO_H */
