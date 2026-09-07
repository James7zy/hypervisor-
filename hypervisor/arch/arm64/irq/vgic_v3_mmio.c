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
#include <vm_config.h>
#include <spinlock.h>
#include <gic_v3.h>              /* GICR_SGI_OFFSET, GICR_ISENABLER0 (physical) */
#include "../../vmexit/mmio.h"   /* struct mmio_access, mmio_handler_t, bus */
#include "vgic_v3_mmio.h"
#include "vgic_debug.h"

/* Physical GICR SGI-frame enable registers (relative to RD_base + SGI frame). */
#define GICR_PHYS_ISENABLER0  0x0100U
#define GICR_PHYS_ICENABLER0  0x0180U

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

/* One distributor shadow per VM, keyed by vm->id (resolved at trap time via
 * current_vcpu()->owner; the MMIO bus registration itself stays global). */
static struct vgicv3_dist g_vgicd[NR_VMS];
/* Both vCPUs can access GICD and ANY GICR frame. BSS-zeroed before entry. */
static struct spinlock vgic_mmio_lock[NR_VMS];

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

static u32 vgicd_read(struct vgicv3_dist *d, u64 off, u8 size)
{
    /* IROUTER is 64-bit; Linux may also read it as two 32-bit halves. */
    if (off >= VGICD_IROUTER_BASE && off <= VGICD_IROUTER_END) {
        u64 aligned = off & ~0x7ULL;
        u32 idx = (u32)((aligned - VGICD_IROUTER_BASE) / 8U);
        u64 full = (idx < 988U) ? d->irouter[idx] : 0ULL;
        if (size == 8U)
            return (u32)full;                 /* low half; caller width=8 */
        return (off & 0x4ULL) ? (u32)(full >> 32) : (u32)full;
    }

    switch (off) {
    case VGICD_CTLR:  return d->ctlr | VGICD_CTLR_ARE_NS; /* ARE_NS RA1 */
    case VGICD_TYPER: return vgicd_typer();
    case VGICD_IIDR:  return 0x0000043BU;                       /* ARM */
    case VGICD_PIDR2: return VGIC_PIDR2_GICV3;
    default: break;
    }

    if (off >= VGICD_IGROUPR_BASE && off <= VGICD_IGROUPR_END)
        return d->igroupr[reg_index(off, VGICD_IGROUPR_BASE)];
    if (off >= VGICD_ISENABLER_BASE && off <= VGICD_ISENABLER_END)
        return d->enabled[reg_index(off, VGICD_ISENABLER_BASE)];
    if (off >= VGICD_ICENABLER_BASE && off <= VGICD_ICENABLER_END)
        return d->enabled[reg_index(off, VGICD_ICENABLER_BASE)];
    if (off >= VGICD_ISPENDR_BASE && off <= VGICD_ISPENDR_END)
        return d->ispend[reg_index(off, VGICD_ISPENDR_BASE)];
    if (off >= VGICD_ICPENDR_BASE && off <= VGICD_ICPENDR_END)
        return d->ispend[reg_index(off, VGICD_ICPENDR_BASE)];
    if (off >= VGICD_ISACTIVER_BASE && off <= VGICD_ISACTIVER_END)
        return d->isactive[reg_index(off, VGICD_ISACTIVER_BASE)];
    if (off >= VGICD_ICACTIVER_BASE && off <= VGICD_ICACTIVER_END)
        return d->isactive[reg_index(off, VGICD_ICACTIVER_BASE)];
    if (off >= VGICD_IPRIORITYR_BASE && off <= VGICD_IPRIORITYR_END)
        return d->ipriorityr[reg_index(off, VGICD_IPRIORITYR_BASE)];
    if (off >= VGICD_ICFGR_BASE && off <= VGICD_ICFGR_END)
        return d->icfgr[reg_index(off, VGICD_ICFGR_BASE)];

    return 0U;   /* RAZ */
}

static void vgicd_write(struct vgicv3_dist *d, u64 off, u32 val, u8 size)
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
            d->irouter[idx] =
                (d->irouter[idx] & 0x00000000FFFFFFFFULL) | ((u64)val << 32);
        else
            d->irouter[idx] =
                (d->irouter[idx] & 0xFFFFFFFF00000000ULL) | (u64)val;
        return;
    }

    switch (off) {
    case VGICD_CTLR:  d->ctlr = val | VGICD_CTLR_ARE_NS; return; /* force ARE_NS */
    case VGICD_TYPER: /* fallthrough */
    case VGICD_IIDR:  /* fallthrough */
    case VGICD_PIDR2: return;   /* RO */
    default: break;
    }

    if (off >= VGICD_IGROUPR_BASE && off <= VGICD_IGROUPR_END) {
        d->igroupr[reg_index(off, VGICD_IGROUPR_BASE)] = val; return;
    }
    if (off >= VGICD_ISENABLER_BASE && off <= VGICD_ISENABLER_END) {
        d->enabled[reg_index(off, VGICD_ISENABLER_BASE)] |= val; return;
    }
    if (off >= VGICD_ICENABLER_BASE && off <= VGICD_ICENABLER_END) {
        d->enabled[reg_index(off, VGICD_ICENABLER_BASE)] &= ~val; return;
    }
    if (off >= VGICD_ISPENDR_BASE && off <= VGICD_ISPENDR_END) {
        d->ispend[reg_index(off, VGICD_ISPENDR_BASE)] |= val; return;
    }
    if (off >= VGICD_ICPENDR_BASE && off <= VGICD_ICPENDR_END) {
        d->ispend[reg_index(off, VGICD_ICPENDR_BASE)] &= ~val; return;
    }
    if (off >= VGICD_ISACTIVER_BASE && off <= VGICD_ISACTIVER_END) {
        d->isactive[reg_index(off, VGICD_ISACTIVER_BASE)] |= val; return;
    }
    if (off >= VGICD_ICACTIVER_BASE && off <= VGICD_ICACTIVER_END) {
        d->isactive[reg_index(off, VGICD_ICACTIVER_BASE)] &= ~val; return;
    }
    if (off >= VGICD_IPRIORITYR_BASE && off <= VGICD_IPRIORITYR_END) {
        d->ipriorityr[reg_index(off, VGICD_IPRIORITYR_BASE)] = val; return;
    }
    if (off >= VGICD_ICFGR_BASE && off <= VGICD_ICFGR_END) {
        d->icfgr[reg_index(off, VGICD_ICFGR_BASE)] = val; return;
    }
    /* else WI */
}

static int vgicd_mmio_handler(struct mmio_access *acc, void *ctx)
{
    (void)ctx;

    /* Which VM faulted: the current vCPU's owner. */
    struct vm *m = current_vcpu()->owner;
    struct vgicv3_dist *d = &g_vgicd[m->id];

    vgic_dbg("GICD %s off=0x%lx size=%d data=0x%lx\n",
             acc->is_write ? "wr" : "rd", (unsigned long)acc->offset,
             (int)acc->size, (unsigned long)acc->data);

    spin_lock(&vgic_mmio_lock[m->id]);
    /* 64-bit IROUTER access, including the complete shadow read/write. */
    if (acc->size == 8U &&
        acc->offset >= VGICD_IROUTER_BASE && acc->offset <= VGICD_IROUTER_END) {
        u32 idx = (u32)(((acc->offset & ~0x7ULL) - VGICD_IROUTER_BASE) / 8U);
        if (idx < 988U) {
            if (acc->is_write)
                d->irouter[idx] = acc->data;
            else
                acc->data = d->irouter[idx];
        } else if (!acc->is_write) {
            acc->data = 0;
        }
        goto out;
    }

    if (acc->is_write)
        vgicd_write(d, acc->offset, (u32)acc->data, acc->size);
    else
        acc->data = vgicd_read(d, acc->offset, acc->size);
out:
    spin_unlock(&vgic_mmio_lock[m->id]);
    return 0;
}

/* ── GICR cpu0 shadow state (INTIDs 0..31: SGIs 0-15 + PPIs 16-31) ── */
struct vgicv3_redist {
    /* RD frame */
    u32 ctlr;
    u32 waker;            /* reset: ProcessorSleep | ChildrenAsleep */
    /* SGI frame */
    u32 igroupr0;
    u32 isenabler0;       /* PPI 27 enable lands here */
    u32 ispendr0;
    u32 isactiver0;
    u32 ipriorityr[8];    /* 8 regs x 4 INTIDs = 32 */
    u32 icfgr1;           /* PPIs (ICFGR0 = SGIs, RO edge) */
};

/* Per-VM redistributor shadows: one per vCPU of each VM. */
static struct vgicv3_redist g_vgicr[NR_VMS][VCPUS_PER_VM] = {
    [0 ... NR_VMS - 1] = {
        [0 ... VCPUS_PER_VM - 1] = {
            .waker = VGICR_WAKER_PROCESSOR_SLEEP | VGICR_WAKER_CHILDREN_ASLEEP,
            .icfgr1 = 0,
        },
    },
};

/* GICR_TYPER for redistributor `cpu` (M3.5: one per vCPU, Aff0 = cpu):
 *   Aff (affinity value)     at bits [63:32] = cpu  (matches VMPIDR_EL2 Aff0)
 *   Processor_Number [23:8]  = cpu
 *   Last (bit 4)             = 1 only on the final redistributor in the range
 */
static u64 vgicr_typer(u32 cpu)
{
    u64 t = ((u64)cpu << 32) | ((u64)cpu << 8);
    if (cpu == (u32)(VCPUS_PER_VM - 1))
        t |= (1ULL << 4);   /* Last */
    return t;
}

static u64 vgicr_read_rd(struct vgicv3_redist *r, u32 cpu, u64 off, u8 size)
{
    switch (off) {
    case VGICR_CTLR:    return r->ctlr;
    case VGICR_IIDR:    return 0x0000043BU;
    /* GICR_TYPER is 64-bit; the Affinity_Value is in bits [63:32]. A 64-bit
     * read MUST return the full value (Linux's gic_populate_rdist matches the
     * cpu's MPIDR affinity against the high word; truncating to 32 bits drops
     * it and a secondary with Aff0 != 0 fails to find its redistributor). */
    case VGICR_TYPER:   return (size == 8U) ? vgicr_typer(cpu)
                                            : (vgicr_typer(cpu) & 0xFFFFFFFFULL);
    case VGICR_TYPER + 4: return (vgicr_typer(cpu) >> 32);  /* high half (32-bit) */
    case VGICR_STATUSR: return 0U;
    case VGICR_WAKER:   return r->waker;
    case VGICR_PIDR2:   return VGIC_PIDR2_GICV3;
    default:            return 0U;   /* RAZ */
    }
}

static void vgicr_write_rd(struct vgicv3_redist *r, u64 off, u32 val)
{
    switch (off) {
    case VGICR_CTLR:
        r->ctlr = val;
        break;
    case VGICR_WAKER:
        /* Handshake: guest clears ProcessorSleep, then polls ChildrenAsleep
         * until it reads 0. Clearing ProcessorSleep clears ChildrenAsleep. */
        if ((val & VGICR_WAKER_PROCESSOR_SLEEP) == 0U)
            r->waker = 0U;
        else
            r->waker = VGICR_WAKER_PROCESSOR_SLEEP |
                       VGICR_WAKER_CHILDREN_ASLEEP;
        break;
    default:
        break;   /* WI */
    }
}

static u32 vgicr_read_sgi(struct vgicv3_redist *r, u64 off)
{
    switch (off) {
    case VGICR_IGROUPR0:   return r->igroupr0;
    case VGICR_ISENABLER0: return r->isenabler0;
    case VGICR_ICENABLER0: return r->isenabler0;
    case VGICR_ISPENDR0:   return r->ispendr0;
    case VGICR_ICPENDR0:   return r->ispendr0;
    case VGICR_ISACTIVER0: return r->isactiver0;
    case VGICR_ICACTIVER0: return r->isactiver0;
    case VGICR_ICFGR0:     return 0xAAAAAAAAU;   /* SGIs edge-triggered, RO */
    case VGICR_ICFGR1:     return r->icfgr1;
    default:
        if (off >= VGICR_IPRIORITYR_BASE && off <= VGICR_IPRIORITYR_END)
            return r->ipriorityr[(u32)((off - VGICR_IPRIORITYR_BASE) / 4U)];
        return 0U;
    }
}

static void vgicr_write_sgi(u32 pcpu, struct vgicv3_redist *r, u64 off, u32 val)
{
    switch (off) {
    case VGICR_IGROUPR0:   r->igroupr0   = val;  break;
    case VGICR_ISENABLER0:
        r->isenabler0 |= val;
        /* When the guest enables its vtimer PPI (27), (re-)enable the physical
         * PPI on this cpu's redistributor. This is the re-arm half of the M3.5
         * secondary vtimer gating: el2_irq_handler masks the physical PPI if it
         * fires before the guest's vGIC is up; here the guest's own enable
         * brings it back, by which point VENG1 is set. Idempotent on cpu0. */
        if (val & (1U << BOARD_VTIMER_IRQ))
            gic_ppi_set_enable(pcpu, BOARD_VTIMER_IRQ, true);
        break;
    case VGICR_ICENABLER0: r->isenabler0 &= ~val; break;  /* clear */
    case VGICR_ISPENDR0:   r->ispendr0   |= val; break;
    case VGICR_ICPENDR0:   r->ispendr0   &= ~val; break;
    case VGICR_ISACTIVER0: r->isactiver0 |= val; break;
    case VGICR_ICACTIVER0: r->isactiver0 &= ~val; break;
    case VGICR_ICFGR0:     break;   /* SGIs RO */
    case VGICR_ICFGR1:     r->icfgr1 = val; break;
    default:
        if (off >= VGICR_IPRIORITYR_BASE && off <= VGICR_IPRIORITYR_END)
            r->ipriorityr[(u32)((off - VGICR_IPRIORITYR_BASE) / 4U)] = val;
        break;   /* else WI */
    }
}

static int vgicr_mmio_handler(struct mmio_access *acc, void *ctx)
{
    (void)ctx;

    vgic_dbg("GICR %s off=0x%lx size=%d data=0x%lx\n",
             acc->is_write ? "wr" : "rd", (unsigned long)acc->offset,
             (int)acc->size, (unsigned long)acc->data);

    /* Which VM faulted: the current vCPU's owner. */
    struct vm *m = current_vcpu()->owner;

    /* Which redistributor frame (VM-local vCPU index): offset / VGICR_STRIDE. */
    u32 cpu = (u32)(acc->offset / VGICR_STRIDE);
    if (cpu >= (u32)VCPUS_PER_VM) {
        if (!acc->is_write)
            acc->data = 0;
        return 0;
    }
    struct vgicv3_redist *r = &g_vgicr[m->id][cpu];
    u64 foff = acc->offset - (u64)cpu * VGICR_STRIDE;   /* within this frame */

    bool sgi = (foff >= VGICR_SGI_OFFSET);
    u64  off = sgi ? (foff - VGICR_SGI_OFFSET) : foff;

    spin_lock(&vgic_mmio_lock[m->id]);
    if (acc->is_write) {
        if (sgi)
            vgicr_write_sgi(m->config->pcpu_base + cpu, r, off, (u32)acc->data);
        else
            vgicr_write_rd(r, off, (u32)acc->data);
    } else {
        acc->data = sgi ? vgicr_read_sgi(r, off)
                        : vgicr_read_rd(r, cpu, off, acc->size);
    }
    spin_unlock(&vgic_mmio_lock[m->id]);
    return 0;
}

void vgicv3_mmio_init(void)
{
    int rd = mmio_bus_register(BOARD_GIC_DIST_BASE, VGICD_SIZE,
                               vgicd_mmio_handler, NULL);
    int rr = mmio_bus_register(BOARD_GIC_RDIST_BASE, VGICR_SIZE,
                               vgicr_mmio_handler, NULL);
    if (rd != 0 || rr != 0)
        printk("[hv] vGICv3: bus full, registration failed (d=%d r=%d)\n",
               rd, rr);
    else
        printk("[hv] vGICv3: GICD 0x%lx/0x%lx GICR 0x%lx/0x%lx registered\n",
               (unsigned long)BOARD_GIC_DIST_BASE, (unsigned long)VGICD_SIZE,
               (unsigned long)BOARD_GIC_RDIST_BASE, (unsigned long)VGICR_SIZE);
}
