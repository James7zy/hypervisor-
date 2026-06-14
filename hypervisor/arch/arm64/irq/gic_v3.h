/* SPDX-License-Identifier: TBD */
#ifndef HV_GIC_V3_H
#define HV_GIC_V3_H

#include <types.h>

/* GICD (distributor) register offsets. */
#define GICD_CTLR              0x0000U
#define GICD_CTLR_ENGRP1NS     (1U << 1)   /* Enable Group 1 NS */
#define GICD_CTLR_ARE_NS       (1U << 4)   /* Affinity Routing Enable, NS */

/* GICR (redistributor) register offsets, relative to RD_base. */
#define GICR_CTLR              0x0000U
#define GICR_WAKER             0x0014U
#define GICR_WAKER_PROC_SLEEP  (1U << 1)
#define GICR_WAKER_CHILD_ASLEEP (1U << 2)

/* SGI/PPI frame sits 64 KiB above RD_base. */
#define GICR_SGI_OFFSET        0x10000U
#define GICR_IGROUPR0          0x0080U
#define GICR_ISENABLER0        0x0100U
#define GICR_IPRIORITYR        0x0400U     /* byte-addressable, one byte per INTID */

/* ICC_CTLR_EL1.EOImode — split priority-drop (EOIR) / deactivate (DIR). */
#define ICC_CTLR_EL1_EOIMODE   (1ULL << 1)

/* Enable the physical GICv3: distributor, CPU0 redistributor, the virtual
 * timer PPI (INTID 27), and the EL2 physical CPU interface with EOImode=1. */
void gic_init(void);

/* Acknowledge the highest-priority pending Group-1 IRQ (ICC_IAR1_EL1). */
u32  gic_ack_irq(void);

/* Priority-drop only (ICC_EOIR1_EL1) — leaves the INTID Active. */
void gic_priority_drop(u32 intid);

/* Deactivate (ICC_DIR_EL1) — valid only with EOImode=1. */
void gic_deactivate(u32 intid);

#endif /* HV_GIC_V3_H */
