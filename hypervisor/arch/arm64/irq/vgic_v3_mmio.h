/* SPDX-License-Identifier: TBD */
#ifndef HV_VGIC_V3_MMIO_H
#define HV_VGIC_V3_MMIO_H

#include <types.h>
#include <percpu.h>   /* NR_CPUS */

/* Frame sizes (QEMU virt GICv3). */
#define VGICD_SIZE        0x00010000ULL   /* 64 KB distributor frame      */
#define VGICR_STRIDE      0x00020000ULL   /* one redistributor (RD+SGI)   */
#define VGICR_SIZE        (VGICR_STRIDE * NR_CPUS)  /* all GICR frames     */
#define VGICR_SGI_OFFSET  0x00010000ULL   /* SGI frame within a GICR      */

/* ── GICD register offsets ── */
#define VGICD_CTLR              0x0000U
#define VGICD_TYPER             0x0004U
#define VGICD_IIDR              0x0008U
#define VGICD_IGROUPR_BASE      0x0080U
#define VGICD_IGROUPR_END       0x00FCU
#define VGICD_ISENABLER_BASE    0x0100U
#define VGICD_ISENABLER_END     0x017CU
#define VGICD_ICENABLER_BASE    0x0180U
#define VGICD_ICENABLER_END     0x01FCU
#define VGICD_ISPENDR_BASE      0x0200U
#define VGICD_ISPENDR_END       0x027CU
#define VGICD_ICPENDR_BASE      0x0280U
#define VGICD_ICPENDR_END       0x02FCU
#define VGICD_ISACTIVER_BASE    0x0300U
#define VGICD_ISACTIVER_END     0x037CU
#define VGICD_ICACTIVER_BASE    0x0380U
#define VGICD_ICACTIVER_END     0x03FCU
#define VGICD_IPRIORITYR_BASE   0x0400U
#define VGICD_IPRIORITYR_END    0x07FCU
#define VGICD_ICFGR_BASE        0x0C00U
#define VGICD_ICFGR_END         0x0CFCU
#define VGICD_IROUTER_BASE      0x6100U
#define VGICD_IROUTER_END       0x7FD8U
#define VGICD_PIDR2             0xFFE8U

/* GICD_CTLR.ARE_NS (bit 4) is read-as-one in our affinity-routed model. */
#define VGICD_CTLR_ARE_NS       (1U << 4)

/* ── GICR RD-frame register offsets ── */
#define VGICR_CTLR      0x0000U
#define VGICR_IIDR      0x0004U
#define VGICR_TYPER     0x0008U   /* 64-bit; 0x000C is its high half */
#define VGICR_STATUSR   0x0010U
#define VGICR_WAKER     0x0014U
#define VGICR_PIDR2     0xFFE8U

/* GICR_WAKER bits. Reset = ProcessorSleep | ChildrenAsleep = 0x6. */
#define VGICR_WAKER_PROCESSOR_SLEEP  (1U << 1)
#define VGICR_WAKER_CHILDREN_ASLEEP  (1U << 2)

/* ── GICR SGI-frame register offsets (relative to SGI base) ── */
#define VGICR_IGROUPR0        0x0080U
#define VGICR_ISENABLER0      0x0100U
#define VGICR_ICENABLER0      0x0180U
#define VGICR_ISPENDR0        0x0200U
#define VGICR_ICPENDR0        0x0280U
#define VGICR_ISACTIVER0      0x0300U
#define VGICR_ICACTIVER0      0x0380U
#define VGICR_IPRIORITYR_BASE 0x0400U
#define VGICR_IPRIORITYR_END  0x041CU
#define VGICR_ICFGR0          0x0C00U
#define VGICR_ICFGR1          0x0C04U

/* ArchRev[7:4]=3 ⇒ GICv3, returned by both GICD_PIDR2 and GICR_PIDR2. */
#define VGIC_PIDR2_GICV3      0x30U

/* Register the GICD + GICR(cpu0) frames on the M3.1 MMIO bus. Call once during
 * vm_init, before the guest runs. */
void vgicv3_mmio_init(void);

#endif /* HV_VGIC_V3_MMIO_H */
