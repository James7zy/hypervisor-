/* SPDX-License-Identifier: TBD */
#include <types.h>
#include <printk.h>
#include <vm.h>
#include "mmio.h"

/*
 * Fixed-size MMIO bus. M3.x needs only a handful of regions:
 * GICD, GICR, and one or two virtio-mmio frames. 8 is ample headroom.
 */
#define MMIO_MAX_REGIONS 8

struct mmio_region {
    u64            base;
    u64            len;
    mmio_handler_t handler;
    void          *ctx;
};

static struct mmio_region mmio_regions[MMIO_MAX_REGIONS];
static u32 mmio_region_count;

int mmio_bus_register(u64 base, u64 len, mmio_handler_t handler, void *ctx)
{
    if (mmio_region_count >= MMIO_MAX_REGIONS)
        return -1;

    mmio_regions[mmio_region_count].base    = base;
    mmio_regions[mmio_region_count].len     = len;
    mmio_regions[mmio_region_count].handler = handler;
    mmio_regions[mmio_region_count].ctx     = ctx;
    mmio_region_count++;
    return 0;
}

static struct mmio_region *mmio_bus_lookup(u64 ipa)
{
    for (u32 i = 0; i < mmio_region_count; i++) {
        struct mmio_region *r = &mmio_regions[i];
        if (ipa >= r->base && ipa < r->base + r->len)
            return r;
    }
    return NULL;
}
