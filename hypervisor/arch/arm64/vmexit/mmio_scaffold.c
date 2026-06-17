/* SPDX-License-Identifier: TBD */
/*
 * M3.1 TEMPORARY scaffold device. Registered over the GICD MMIO range to prove
 * the Stage-2 data-abort decode + MMIO dispatch path delivers correct
 * {addr, size, direction, data} to a handler. REMOVE in M3.2: the real
 * vGICv3 distributor emulation registers over this same range.
 */
#include <types.h>
#include <printk.h>
#include <board.h>
#include "mmio.h"

/* QEMU virt GICD frame: 0x08000000, 64 KB. */
#define SCAFFOLD_GICD_BASE  BOARD_GIC_DIST_BASE
#define SCAFFOLD_GICD_LEN   0x00010000ULL

static int scaffold_handler(struct mmio_access *acc, void *ctx)
{
    (void)ctx;

    if (acc->is_write) {
        printk("[hv] MMIO scaffold: WRITE ipa=0x%lx off=0x%lx size=%u data=0x%lx\n",
               acc->ipa, acc->offset, (unsigned)acc->size, acc->data);
    } else {
        /* Read-as-zero: the scaffold has no real registers. */
        acc->data = 0;
        printk("[hv] MMIO scaffold: READ  ipa=0x%lx off=0x%lx size=%u -> 0x%lx\n",
               acc->ipa, acc->offset, (unsigned)acc->size, acc->data);
    }
    return 0;
}

void mmio_scaffold_init(void)
{
    if (mmio_bus_register(SCAFFOLD_GICD_BASE, SCAFFOLD_GICD_LEN,
                          scaffold_handler, NULL) != 0)
        printk("[hv] MMIO scaffold: bus full, registration failed\n");
    else
        printk("[hv] MMIO scaffold: registered GICD range 0x%lx len 0x%lx\n",
               (unsigned long)SCAFFOLD_GICD_BASE,
               (unsigned long)SCAFFOLD_GICD_LEN);
}
