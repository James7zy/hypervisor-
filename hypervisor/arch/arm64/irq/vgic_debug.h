/* SPDX-License-Identifier: TBD */
#ifndef ARCH_VGIC_DEBUG_H
#define ARCH_VGIC_DEBUG_H

/*
 * vGIC debug logging. OFF by default (not in defconfig) — zero overhead: the
 * call is preprocessed away to ((void)0), so no string lands in .rodata and no
 * branch is emitted. Enable with `CONFIG_DEBUG_VGIC=y` in .config (the Makefile
 * turns it into -DCONFIG_DEBUG_VGIC=1), then rebuild.
 *
 * Used to trace the two paths verified during the GICD/GICR punch-hole work
 * (docs/reference/arm/2026-06-21-gicd-gicr-trap-investigation.md):
 *   - GICD/GICR register accesses  -> vgic_v3_mmio.c shadow handlers
 *   - timer PPI / PL011 SPI inject -> irq_handler.c el2_irq_handler
 */
#ifdef CONFIG_DEBUG_VGIC
#include <printk.h>
#define vgic_dbg(...) printk("[vgic] " __VA_ARGS__)
#else
#define vgic_dbg(...) ((void)0)
#endif

#endif /* ARCH_VGIC_DEBUG_H */
