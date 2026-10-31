/* SPDX-License-Identifier: TBD */
#include <sched.h>
#include <vm_config.h>   /* vcpu_pcpu[] affinity */
#include <sched_q.h>
#include <percpu.h>
#include <spinlock.h>

/* One queue + one lock per pCPU. Static sizing: NR_CPUS queues of
 * SCHED_Q_MAX entries each — trivial next to struct vm. */
static struct sched_q  queues[NR_CPUS];
static struct spinlock q_lock[NR_CPUS];

void sched_init(void)
{
    for (u32 p = 0; p < (u32)NR_CPUS; p++) {
        sched_q_init(&queues[p]);
        q_lock[p] = (struct spinlock)SPINLOCK_INIT;
    }
    /* Fill from the config: every RUNNABLE boot vCPU starts queued on its
     * affinity pCPU (vm_init authors vcpu[0] RUNNABLE; vcpu[1+] wait for
     * guest CPU_OFF... CPU_ON and enqueue from vpsci). */
    for (u32 vmi = 0; vmi < (u32)NR_VMS; vmi++) {
        for (u32 i = 0; i < (u32)VCPUS_PER_VM; i++) {
            struct vcpu *v = &vm[vmi].vcpu[i];
            if (v->state == VCPU_RUNNABLE)
                (void)sched_q_enqueue(&queues[vm[vmi].config->vcpu_pcpu[i]], v);
        }
    }
}

struct vcpu *sched_pick(u32 pcpu)
{
    spin_lock(&q_lock[pcpu]);
    struct vcpu *v = sched_q_dequeue(&queues[pcpu]);
    spin_unlock(&q_lock[pcpu]);
    return v;
}

void sched_yield_cur(void)
{
    /* Queue derived from the CURRENT pCPU while sched_enqueue derives it
     * from the vCPU's affinity — equal under the B1 invariant that
     * cur_vcpu is always this pCPU's affined vCPU (static affinity, no
     * migration). B2 re-audits if that ever loosens. */
    struct vcpu *v = current_vcpu();
    u32 pcpu = current_vcpu_id();

    spin_lock(&q_lock[pcpu]);
    (void)sched_q_enqueue(&queues[pcpu], v);   /* idempotent */
    spin_unlock(&q_lock[pcpu]);
}

void sched_enqueue(struct vcpu *v)
{
    u32 pcpu = v->owner->config->vcpu_pcpu[v->vcpu_idx];

    spin_lock(&q_lock[pcpu]);
    (void)sched_q_enqueue(&queues[pcpu], v);   /* idempotent */
    spin_unlock(&q_lock[pcpu]);
}
