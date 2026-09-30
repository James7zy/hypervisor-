/* SPDX-License-Identifier: TBD */
#ifndef HV_MMIO_H
#define HV_MMIO_H

#include <types.h>

/*
 * Architecture-neutral MMIO trap-and-emulate bus (ADR-0006; moved out of
 * arch/ by ADR-0015). Device models register guest-physical ranges; each
 * arch decodes its own trap (arm64: Stage-2 data abort, see
 * arch/arm64/vmexit/data_abort.c) into a struct mmio_access and dispatches it
 * here.
 */

/*
 * A decoded MMIO access handed to a device handler.
 *
 *   addr     - faulting guest-physical address (arm64: IPA), byte-exact
 *   offset   - addr - region base; what most device handlers actually use
 *              (filled in by mmio_bus_dispatch)
 *   size     - access width in bytes: 1, 2, 4, or 8
 *   is_write - true for a store (guest -> device), false for a load
 *   data     - WRITE: value the guest stored (filled in before the handler).
 *              READ:  value the device returns (handler fills it in).
 */
struct mmio_access {
    u64  addr;
    u64  offset;
    u8   size;
    bool is_write;
    u64  data;
};

/*
 * Device handler. Receives the decoded access and the ctx registered with the
 * region. Returns 0 on success, non-zero to signal an unhandled/failed access
 * (which the trap path treats like "no handler": diagnostic + park).
 */
typedef int (*mmio_handler_t)(struct mmio_access *acc, void *ctx);

/*
 * Register a device over the guest-physical range [base, base + len).
 * Returns 0 on success, -1 if the bus is full. Ranges are not checked for
 * overlap (single-author bus, M3.x).
 */
int mmio_bus_register(u64 base, u64 len, mmio_handler_t handler, void *ctx);

/*
 * Route a decoded access (acc->addr/size/is_write/data set) to the region
 * that covers acc->addr: fills acc->offset and calls its handler. Returns 0
 * if handled, -1 if no region covers the address or the handler failed.
 */
int mmio_bus_dispatch(struct mmio_access *acc);

#endif /* HV_MMIO_H */
