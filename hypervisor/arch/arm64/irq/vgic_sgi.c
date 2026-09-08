/* SPDX-License-Identifier: TBD */
/*
 * SGI / IPI virtualization (M3.5).
 *
 * A guest vCPU sends an IPI by writing the system register ICC_SGI1R_EL1. The
 * target vCPU may be running on another pCPU whose live ICH_LR_EL2 we cannot
 * write from the sender, so cross-core injection is two steps:
 *
 *   1. sender pCPU traps the ICC_SGI1R_EL1 write (ICH_HCR_EL2.TC=1), records
 *      the pending SGI in the *target* vCPU's shared bitmap, and issues a
 *      physical "kick" SGI (BOARD_KICK_SGI) to the target pCPU;
 *   2. the target pCPU takes that physical SGI to EL2, drains its own pending
 *      bitmap, injects each pending vINTID into its own LR, and deactivates
 *      the kick.
 *
 * The bitmap is the only cross-core-written state, so all access is under a
 * single SGI spinlock.
 */
#include <types.h>
#include <board.h>
#include <printk.h>
#include <vm.h>
#include <vm_config.h>   /* struct vm_config (pcpu_base) */
#include <percpu.h>
#include <spinlock.h>
#include <asm/sysreg.h>
#include <vgic.h>
#include <gic_v3.h>
#include "vgic_sgi.h"

/* Per-vCPU pending SGIs (INTID 0..15 → bits 0..15). Written cross-core. */
static volatile u16 sgi_pending[NR_CPUS];
static struct spinlock sgi_lock = SPINLOCK_INIT;

/* ICC_SGI1R_EL1 fields (Arm IHI0069). For our flat 2-vCPU topology the target
 * is the affinity-0 list (TargetList bitmap) at Aff1=Aff2=Aff3=0. */
#define SGI1R_INTID_SHIFT   24
#define SGI1R_INTID_MASK    0xFULL
#define SGI1R_TARGETLIST    0xFFFFULL          /* bits [15:0] */
#define SGI1R_IRM           (1ULL << 40)       /* 1 = broadcast to all but self */

/*
 * Trap path (sender pCPU): decode the guest's ICC_SGI1R_EL1 value, mark the
 * target vCPUs' pending bitmaps, and kick each target pCPU. Targets are
 * VM-local vCPU indices; the pCPU slot is pcpu_base + idx (static 1:1
 * pinning, no scheduler).
 */
void vgic_sgi_trap(u64 sgi1r)
{
    u32 vintid = (u32)((sgi1r >> SGI1R_INTID_SHIFT) & SGI1R_INTID_MASK);
    struct vcpu *sender = current_vcpu();
    struct vm   *m     = sender->owner;
    u32 self   = sender->vcpu_idx;
    u32 targets;

    if (sgi1r & SGI1R_IRM)
        targets = ((1U << VCPUS_PER_VM) - 1U) & ~(1U << self); /* all but self */
    else
        targets = (u32)(sgi1r & SGI1R_TARGETLIST);

    for (u32 idx = 0; idx < (u32)VCPUS_PER_VM; idx++) {
        if (!(targets & (1U << idx)))
            continue;

        u32 pcpu = m->config->pcpu_base + idx;   /* physical slot */

        spin_lock(&sgi_lock);
        sgi_pending[pcpu] |= (u16)(1U << vintid);
        spin_unlock(&sgi_lock);

        if (idx == self) {
            /* Self-IPI: inject directly, no physical kick needed. */
            vgic_sgi_drain(pcpu);
        } else {
            gic_kick_pcpu(pcpu);
        }
    }
}

/*
 * Force every other ONLINE pCPU of VM `m` into EL2 (M5 slice 3, VM-scoped
 * PSCI power-down). Deliberately does NOT touch sgi_pending: this kick is not
 * carrying a virtual SGI, just forcing the target to EL2 so it can observe
 * m->off (checked in el2_irq_handler's kick-SGI branch) and park instead of
 * re-entering its guest.
 */
void vgic_kick_vm_other_pcpus(struct vm *m, u32 caller_pcpu)
{
    for (u32 idx = 0; idx < (u32)VCPUS_PER_VM; idx++) {
        u32 pcpu = m->config->pcpu_base + idx;
        if (pcpu == caller_pcpu)
            continue;
        if (percpu[pcpu].online)
            gic_kick_pcpu(pcpu);
    }
}

/*
 * Target path (kicked pCPU): drain this core's pending SGI bitmap and inject
 * each as a virtual SGI. Called from el2_irq_handler on BOARD_KICK_SGI.
 */
void vgic_sgi_drain(u32 cpu)
{
    u16 pend;

    spin_lock(&sgi_lock);
    pend = sgi_pending[cpu];
    sgi_pending[cpu] = 0;
    spin_unlock(&sgi_lock);

    /* Drain always runs ON pCPU `cpu`; its pinned vCPU is cur_vcpu. */
    struct vcpu *v = percpu[cpu].cur_vcpu;
    for (u32 intid = 0; intid < 16U; intid++) {
        if (pend & (1U << intid))
            vgic_inject_sgi(v, intid);
    }
}
