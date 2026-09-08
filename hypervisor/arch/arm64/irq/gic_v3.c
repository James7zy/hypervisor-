/* SPDX-License-Identifier: TBD */
/*
 * Physical GICv3 driver (QEMU virt, single CPU).
 *
 * The hypervisor owns the physical GIC; the guest sees only the virtual CPU
 * interface (ICC_* redirected to ICV_* by the hardware). M2.5 enables just
 * enough of the distributor/redistributor to deliver the virtual-timer PPI
 * (INTID 27) to EL2, then hardware-forwards it (see vgic_inject_hw / ADR-0001).
 */
#include <types.h>
#include <board.h>
#include <printk.h>
#include <asm/sysreg.h>
#include <gic_v3.h>

static inline void mmio_write32(unsigned long addr, u32 val)
{
    *(volatile u32 *)addr = val;
}

static inline u32 mmio_read32(unsigned long addr)
{
    return *(volatile u32 *)addr;
}

static inline void mmio_write8(unsigned long addr, u8 val)
{
    *(volatile u8 *)addr = val;
}

static inline void mmio_write64(unsigned long addr, u64 val)
{
    *(volatile u64 *)addr = val;
}

void gic_init(void)
{
    const unsigned long dist = BOARD_GIC_DIST_BASE;
    const unsigned long rd   = BOARD_GIC_RDIST_BASE;
    const unsigned long sgi  = rd + GICR_SGI_OFFSET;
    const u32 ppi = BOARD_VTIMER_IRQ;

    /* 1. Distributor: affinity routing + Group 1 NS. */
    mmio_write32(dist + GICD_CTLR, GICD_CTLR_ARE_NS | GICD_CTLR_ENGRP1NS);

    /* 2. Wake CPU0 redistributor: clear ProcessorSleep, wait ChildrenAsleep=0. */
    mmio_write32(rd + GICR_WAKER,
                 mmio_read32(rd + GICR_WAKER) & ~GICR_WAKER_PROC_SLEEP);
    while (mmio_read32(rd + GICR_WAKER) & GICR_WAKER_CHILD_ASLEEP)
        ;

    /* 3. Configure the virtual-timer PPI in the SGI/PPI frame:
     *    Group 1 NS, priority 0xA0, enabled. */
    mmio_write32(sgi + GICR_IGROUPR0,
                 mmio_read32(sgi + GICR_IGROUPR0) | (1U << ppi));
    mmio_write8(sgi + GICR_IPRIORITYR + ppi, 0xA0);
    mmio_write32(sgi + GICR_ISENABLER0, (1U << ppi));

    /* 3a. Cross-core kick-SGI (M3.5) on CPU0's GICR: Group 1 NS, prio 0xA0,
     *     enabled. Lets another pCPU force CPU0 into EL2 to drain its pending
     *     SGI bitmap. (secondary_main enables the same INTID on its own GICR.) */
    {
        const u32 kick = BOARD_KICK_SGI;
        mmio_write32(sgi + GICR_IGROUPR0,
                     mmio_read32(sgi + GICR_IGROUPR0) | (1U << kick));
        mmio_write8(sgi + GICR_IPRIORITYR + kick, 0xA0);
        mmio_write32(sgi + GICR_ISENABLER0, (1U << kick));
    }

    /* 3b. PL011 UART SPI (INTID 33) in the distributor: Group 1 NS, priority
     *     0xA0, routed to CPU0 (Aff=0), enabled. The guest's ttyAMA0 driver is
     *     interrupt-driven; this physical SPI is taken to EL2 (HCR_EL2.IMO) and
     *     injected into the guest's vGIC by el2_irq_handler so the guest reads
     *     the RX byte from the passed-through PL011 DR. */
    {
        const u32 spi  = BOARD_PL011_IRQ;
        const u32 word = spi / 32U;          /* register index               */
        const u32 bit  = spi % 32U;          /* bit within the 32-bit word    */
        mmio_write32(dist + GICD_IGROUPR + word * 4U,
                     mmio_read32(dist + GICD_IGROUPR + word * 4U) | (1U << bit));
        mmio_write8(dist + GICD_IPRIORITYR + spi, 0xA0);
        mmio_write64(dist + GICD_IROUTER + spi * 8U, 0ULL);   /* Aff3.2.1.0 = 0 */
        mmio_write32(dist + GICD_ISENABLER + word * 4U, (1U << bit));
    }

    /* 4. EL2 physical CPU interface. */
    SYSREG_WRITE(ICC_SRE_EL2, 0xFULL);          /* SRE|DFB|DIB|Enable */
    asm volatile("isb");
    SYSREG_WRITE(ICC_PMR_EL1, 0xFFULL);         /* allow all priorities */
    SYSREG_WRITE(ICC_IGRPEN1_EL1, 1ULL);        /* enable Group 1 */
    SYSREG_WRITE(ICC_CTLR_EL1,
                 SYSREG_READ(ICC_CTLR_EL1) | ICC_CTLR_EL1_EOIMODE);
    asm volatile("isb");

    printk("[hv] GIC: initialized (dist=0x%lx rdist=0x%lx PPI=%u)\n",
           dist, rd, (unsigned)ppi);
}

u32 gic_ack_irq(void)
{
    return (u32)(SYSREG_READ(ICC_IAR1_EL1) & 0xFFFFFFULL);
}

void gic_priority_drop(u32 intid)
{
    SYSREG_WRITE(ICC_EOIR1_EL1, intid);
}

void gic_deactivate(u32 intid)
{
    SYSREG_WRITE(ICC_DIR_EL1, intid);
}

/* ICC_SGI1R_EL1 fields (Arm IHI0069). Our topology is flat: Aff1=Aff2=Aff3=0,
 * so the TargetList bitmap alone selects the target PE and its bit is Aff0,
 * which equals the pCPU index. */
#define SGI1R_INTID_SHIFT   24

void gic_kick_pcpu(u32 cpu)
{
    u64 sgi = ((u64)BOARD_KICK_SGI << SGI1R_INTID_SHIFT) | (1ULL << cpu);
    asm volatile("msr ICC_SGI1R_EL1, %0" :: "r"(sgi));
    asm volatile("isb");
}

void gic_ppi_set_enable(u32 cpu, u32 intid, bool enable)
{
    const unsigned long sgi = BOARD_GIC_RDIST_BASE
                              + (unsigned long)cpu * 0x20000UL   /* GICR stride */
                              + GICR_SGI_OFFSET;
    const unsigned long reg = sgi + (enable ? GICR_ISENABLER0 : 0x0180U /*ICENABLER0*/);
    mmio_write32(reg, (1U << (intid & 0x1FU)));
    asm volatile("dsb sy; isb");
}
