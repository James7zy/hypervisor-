/* SPDX-License-Identifier: TBD */
/*
 * vGICv3 distributor (GICD) + redistributor (GICR cpu0) MMIO trap-and-emulate.
 *
 * Built on the M3.1 MMIO bus (struct mmio_access). Shadow-only: the guest's
 * GICD/GICR register accesses never touch the physical GIC. The one physical
 * interrupt the guest consumes (vtimer PPI 27) is owned by gic_init() and
 * hardware-forwarded by el2_irq_handler (M2.5/ADR-0001); device SPIs are
 * software-injected via vgic_inject_spi. Single VM, single vCPU, cpu0 only.
 */
#include <types.h>
#include <printk.h>
#include <board.h>
#include <vm.h>
#include "../../vmexit/mmio.h"   /* struct mmio_access, mmio_handler_t, bus */
#include "vgic_v3_mmio.h"

/* ── GICD shadow state (1024 INTIDs => 32 words of 1 bit/INTID) ── */
struct vgicv3_dist {
    u32 ctlr;
    u32 igroupr[32];
    u32 enabled[32];      /* ISENABLER/ICENABLER */
    u32 ispend[32];       /* ISPENDR/ICPENDR     */
    u32 isactive[32];     /* ISACTIVER/ICACTIVER */
    u32 ipriorityr[256];  /* 1 byte/INTID, packed 4 per word */
    u32 icfgr[64];        /* 2 bits/INTID */
    u64 irouter[988];     /* SPIs 32..1019 */
};

static struct vgicv3_dist g_vgicd;

/* GICD_TYPER for a single-cpu, 1024-INTID, GICv3 distributor.
 *   ITLinesNumber[4:0] = 31  -> (31+1)*32 = 1024 INTIDs
 *   CPUNumber[7:5]     = 0    -> 1 cpu (ncpu-1)
 *   IDbits[23:19]      = 9    -> 10-bit INTID space
 *   A3V[24] = 1, No1N[25] = 1
 */
static u32 vgicd_typer(void)
{
    return 31U | (0U << 5) | (9U << 19) | (1U << 24) | (1U << 25);
}

/* Helper: index of a per-32-INTID register word from an offset range. */
static u32 reg_index(u64 off, u32 base)
{
    return (u32)((off - base) / 4U);
}

static u32 vgicd_read(u64 off, u8 size)
{
    /* IROUTER is 64-bit; Linux may also read it as two 32-bit halves. */
    if (off >= VGICD_IROUTER_BASE && off <= VGICD_IROUTER_END) {
        u64 aligned = off & ~0x7ULL;
        u32 idx = (u32)((aligned - VGICD_IROUTER_BASE) / 8U);
        u64 full = (idx < 988U) ? g_vgicd.irouter[idx] : 0ULL;
        if (size == 8U)
            return (u32)full;                 /* low half; caller width=8 */
        return (off & 0x4ULL) ? (u32)(full >> 32) : (u32)full;
    }

    switch (off) {
    case VGICD_CTLR:  return g_vgicd.ctlr | VGICD_CTLR_ARE_NS; /* ARE_NS RA1 */
    case VGICD_TYPER: return vgicd_typer();
    case VGICD_IIDR:  return 0x0000043BU;                       /* ARM */
    case VGICD_PIDR2: return VGIC_PIDR2_GICV3;
    default: break;
    }

    if (off >= VGICD_IGROUPR_BASE && off <= VGICD_IGROUPR_END)
        return g_vgicd.igroupr[reg_index(off, VGICD_IGROUPR_BASE)];
    if (off >= VGICD_ISENABLER_BASE && off <= VGICD_ISENABLER_END)
        return g_vgicd.enabled[reg_index(off, VGICD_ISENABLER_BASE)];
    if (off >= VGICD_ICENABLER_BASE && off <= VGICD_ICENABLER_END)
        return g_vgicd.enabled[reg_index(off, VGICD_ICENABLER_BASE)];
    if (off >= VGICD_ISPENDR_BASE && off <= VGICD_ISPENDR_END)
        return g_vgicd.ispend[reg_index(off, VGICD_ISPENDR_BASE)];
    if (off >= VGICD_ICPENDR_BASE && off <= VGICD_ICPENDR_END)
        return g_vgicd.ispend[reg_index(off, VGICD_ICPENDR_BASE)];
    if (off >= VGICD_ISACTIVER_BASE && off <= VGICD_ISACTIVER_END)
        return g_vgicd.isactive[reg_index(off, VGICD_ISACTIVER_BASE)];
    if (off >= VGICD_ICACTIVER_BASE && off <= VGICD_ICACTIVER_END)
        return g_vgicd.isactive[reg_index(off, VGICD_ICACTIVER_BASE)];
    if (off >= VGICD_IPRIORITYR_BASE && off <= VGICD_IPRIORITYR_END)
        return g_vgicd.ipriorityr[reg_index(off, VGICD_IPRIORITYR_BASE)];
    if (off >= VGICD_ICFGR_BASE && off <= VGICD_ICFGR_END)
        return g_vgicd.icfgr[reg_index(off, VGICD_ICFGR_BASE)];

    return 0U;   /* RAZ */
}

static void vgicd_write(u64 off, u32 val, u8 size)
{
    if (off >= VGICD_IROUTER_BASE && off <= VGICD_IROUTER_END) {
        u64 aligned = off & ~0x7ULL;
        u32 idx = (u32)((aligned - VGICD_IROUTER_BASE) / 8U);
        if (idx >= 988U)
            return;
        if (size == 8U) {
            /* 64-bit path: caller passes the full value via vgicd_write64. */
            return;   /* handled in the 8-byte branch of the handler */
        }
        if (off & 0x4ULL)
            g_vgicd.irouter[idx] =
                (g_vgicd.irouter[idx] & 0x00000000FFFFFFFFULL) | ((u64)val << 32);
        else
            g_vgicd.irouter[idx] =
                (g_vgicd.irouter[idx] & 0xFFFFFFFF00000000ULL) | (u64)val;
        return;
    }

    switch (off) {
    case VGICD_CTLR:  g_vgicd.ctlr = val | VGICD_CTLR_ARE_NS; return; /* force ARE_NS */
    case VGICD_TYPER: /* fallthrough */
    case VGICD_IIDR:  /* fallthrough */
    case VGICD_PIDR2: return;   /* RO */
    default: break;
    }

    if (off >= VGICD_IGROUPR_BASE && off <= VGICD_IGROUPR_END) {
        g_vgicd.igroupr[reg_index(off, VGICD_IGROUPR_BASE)] = val; return;
    }
    if (off >= VGICD_ISENABLER_BASE && off <= VGICD_ISENABLER_END) {
        g_vgicd.enabled[reg_index(off, VGICD_ISENABLER_BASE)] |= val; return;
    }
    if (off >= VGICD_ICENABLER_BASE && off <= VGICD_ICENABLER_END) {
        g_vgicd.enabled[reg_index(off, VGICD_ICENABLER_BASE)] &= ~val; return;
    }
    if (off >= VGICD_ISPENDR_BASE && off <= VGICD_ISPENDR_END) {
        g_vgicd.ispend[reg_index(off, VGICD_ISPENDR_BASE)] |= val; return;
    }
    if (off >= VGICD_ICPENDR_BASE && off <= VGICD_ICPENDR_END) {
        g_vgicd.ispend[reg_index(off, VGICD_ICPENDR_BASE)] &= ~val; return;
    }
    if (off >= VGICD_ISACTIVER_BASE && off <= VGICD_ISACTIVER_END) {
        g_vgicd.isactive[reg_index(off, VGICD_ISACTIVER_BASE)] |= val; return;
    }
    if (off >= VGICD_ICACTIVER_BASE && off <= VGICD_ICACTIVER_END) {
        g_vgicd.isactive[reg_index(off, VGICD_ICACTIVER_BASE)] &= ~val; return;
    }
    if (off >= VGICD_IPRIORITYR_BASE && off <= VGICD_IPRIORITYR_END) {
        g_vgicd.ipriorityr[reg_index(off, VGICD_IPRIORITYR_BASE)] = val; return;
    }
    if (off >= VGICD_ICFGR_BASE && off <= VGICD_ICFGR_END) {
        g_vgicd.icfgr[reg_index(off, VGICD_ICFGR_BASE)] = val; return;
    }
    /* else WI */
}

static int vgicd_mmio_handler(struct mmio_access *acc, void *ctx)
{
    (void)ctx;

    /* 64-bit IROUTER access. */
    if (acc->size == 8U &&
        acc->offset >= VGICD_IROUTER_BASE && acc->offset <= VGICD_IROUTER_END) {
        u32 idx = (u32)(((acc->offset & ~0x7ULL) - VGICD_IROUTER_BASE) / 8U);
        if (idx < 988U) {
            if (acc->is_write)
                g_vgicd.irouter[idx] = acc->data;
            else
                acc->data = g_vgicd.irouter[idx];
        } else if (!acc->is_write) {
            acc->data = 0;
        }
        return 0;
    }

    if (acc->is_write)
        vgicd_write(acc->offset, (u32)acc->data, acc->size);
    else
        acc->data = vgicd_read(acc->offset, acc->size);

    return 0;
}
