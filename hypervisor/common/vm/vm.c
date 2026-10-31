/* SPDX-License-Identifier: TBD */
#include <types.h>
#include <printk.h>
#include <vm.h>
#include <percpu.h>
#include <uart.h>
#include <vuart.h>
#include <cpu.h>
#include <vm_config.h>
#include <sched.h>

struct vm vm[NR_VMS];

void vm_init(void)
{
    for (u32 vmi = 0; vmi < (u32)NR_VMS; vmi++) {
        struct vm *m = &vm[vmi];

        m->id     = vmi;
        m->config = &vm_configs[vmi];

        const struct vm_config *cfg = m->config;

        /*
         * Per-vCPU affinity bounds (M11 slice A, D6): every affinity entry
         * must name a pCPU that exists, or later code indexes percpu[] / arch
         * per-pCPU arrays out of bounds. The old compile-time
         * NR_VMS*VCPUS_PER_VM<=NR_CPUS assert could not express 1:N data.
         * Duplicates (vCPUs sharing a pCPU) are legal: slice A runs the
         * lowest-(vmi,idx) one, sharing executes from slice B.
         */
        for (u32 i = 0; i < (u32)VCPUS_PER_VM; i++) {
            if (cfg->vcpu_pcpu[i] >= (u8)NR_CPUS) {
                printk("[hv] BUG: vm[%u].config->vcpu_pcpu[%u]=%u >= NR_CPUS=%u\n",
                       (unsigned)vmi, (unsigned)i,
                       (unsigned)cfg->vcpu_pcpu[i], (unsigned)NR_CPUS);
                cpu_arch_halt();
            }
        }

        /* Every vCPU knows its VM and its VM-local index; vcpu[1+]'s regs are
         * authored later when the guest powers it on (arm64: PSCI CPU_ON).
         * All vCPUs start OFF (zero-init, restated for clarity); vcpu[0] is
         * then authored straight to RUNNABLE — init-only direct write, not a
         * vcpu_state_step event: the hypervisor itself is the boot vCPU's
         * authorizer (the guest is not running yet to issue PSCI for it),
         * the same way the state machine's [*] --> OFF edge is vm_init's.
         */
        for (u32 i = 0; i < (u32)VCPUS_PER_VM; i++) {
            m->vcpu[i].owner    = m;
            m->vcpu[i].vcpu_idx = i;
            m->vcpu[i].state    = VCPU_OFF;
        }

        vm_arch_init(m);
        vcpu_arch_reset(&m->vcpu[0], cfg);
        m->vcpu[0].state = VCPU_RUNNABLE;

#ifdef CONFIG_GUEST_SVM
        printk("SVM: launching VMID=%u entry=0x%lx ram_pa=0x%lx\n",
               (unsigned)cfg->vmid, (unsigned long)cfg->entry,
               (unsigned long)cfg->ram_pa);
#else
        printk("[hv] Linux guest: VMID=%u entry=0x%lx dtb=0x%lx ram_pa=0x%lx\n",
               (unsigned)cfg->vmid, (unsigned long)cfg->entry,
               (unsigned long)cfg->bootinfo_ipa, (unsigned long)cfg->ram_pa);
#endif
    }

    /* Global MMIO bus registration: once total, not per VM. */
    vm_arch_devices_init();
    vuart_bus_init();

    sched_init();   /* queues filled from the authored states (M11 B1) */
}

struct vcpu *vcpu_for_pcpu(u32 pcpu)
{
    for (u32 vmi = 0; vmi < (u32)NR_VMS; vmi++) {
        for (u32 i = 0; i < (u32)VCPUS_PER_VM; i++) {
            if ((u32)vm[vmi].config->vcpu_pcpu[i] == pcpu)
                return &vm[vmi].vcpu[i];
        }
    }
    return NULL;
}

void vm_run(void)
{
    /*
     * Make this pCPU's percpu slot (arm64: TPIDR_EL2) the single source of
     * truth for "current vCPU on this core" before the first guest entry.
     * The arch's exception-entry asm reads the guest frame through
     * &percpu[id]->cur_vcpu (PERCPU_CUR_VCPU) instead of the address of a
     * single global VM (M3.5 Slice 1; supersedes the ADR-0003 trick). CPU0
     * is always VM0 vCPU0.
     */
    percpu[0].cpu_id   = 0;
    percpu[0].cur_vcpu = &vm[0].vcpu[0];
    /* The boot pCPU is online too: secondary.c publishes .online for the
     * PSCI-brought-up cores, and AFFINITY_INFO's ON mapping reads run state;
     * .online stays the physical-online fact. */
    percpu[0].online   = 1;
    cpu_arch_set_this_percpu(&percpu[0]);

    /* Boot identity: CPU0 IS VM0's vCPU0 by construction below (both loops
     * hardcode it). A config affining it elsewhere would silently double-run
     * one vCPU on two pCPUs — catch it here (gate-2 Minor 5). */
    if (vcpu_for_pcpu(0) != &vm[0].vcpu[0]) {
        printk("[hv] BUG: pCPU0 must host VM0 vCPU0 (vm_config.vcpu_pcpu)\n");
        cpu_arch_halt();
    }

    /*
     * CPU0 owns the physical console UART's RX interrupt, matching the
     * existing routing. Enable RX + receive-timeout interrupts once, before
     * the guest-entry loop: from here the hypervisor drains the physical FIFO
     * itself on every RX interrupt and feeds bytes to the vuart model
     * (M5 slice 2).
     */
    uart_rx_irq_enable();

    /*
     * M11 slice A boot flow: the hypervisor brings up EVERY physical pCPU.
     * Each secondary finds its statically affined vCPU (secondary_main ->
     * vcpu_for_pcpu) or idles; boot vCPUs were authored RUNNABLE by vm_init,
     * so their pCPUs enter them right away. This replaces the old "power on
     * only each VM's boot pCPU" loop and turns guest CPU_ON into a kick (the
     * pCPU is already online), not a firmware power-on.
     */
    for (u32 p = 1; p < (u32)NR_CPUS; p++) {
        s64 r = cpu_arch_power_on(p);
        if (r != 0)
            printk("[hv] pCPU%u power-on failed (%d)\n",
                   (unsigned)p, (int)r);
    }

    struct vcpu *prev = NULL;

    for (;;) {
        /*
         * Scheduler loop (M11 B1, gate-1 R1/R2/R3). Pick carries the RR; a
         * picked vCPU that is no longer enterable (swept OFF by a racing
         * SYSTEM_OFF) is skipped, not waited on — waiting would starve the
         * queue's other vCPUs. Exits needing this loop: timer IRQ (re-pick),
         * power-down unwinds (hv_restore; the unwound vCPU re-enters via
         * pick), and B1's WFI yield (same unwind shape, T4). The vuart/
         * console work stays in the IRQ path (ADR-0013).
         */
        struct vcpu *v;
        if (prev != NULL &&
            __atomic_load_n(&prev->state, __ATOMIC_ACQUIRE) == VCPU_RUNNING) {
            /* Return-without-yield (timer exit, HC_GUEST_DONE park): the
             * vCPU is RUNNING and therefore OUT of the queue (RR dequeues
             * at pick; only yield re-enqueues) — re-enter it directly.
             * Picking here would see an empty queue and idle forever. */
            vcpu_arch_run(prev);
            continue;
        }
        v = sched_pick(0);
        if (v == NULL) {
            cpu_arch_idle();
            continue;
        }
        if (v->state != VCPU_RUNNABLE && v->state != VCPU_ON_PENDING)
            continue;                       /* swept: drop it, pick again */
        if (__atomic_load_n(&v->state, __ATOMIC_ACQUIRE) == VCPU_ON_PENDING)
            (void)vcpu_state_step(&v->state, VCPU_EV_PICK);
        if (v->state != VCPU_RUNNABLE)
            continue;                       /* lost the race above */

        if (v != prev) {
            if (prev != NULL && prev->state == VCPU_RUNNABLE)
                vcpu_arch_put(prev);        /* real switch-away only (R1) */
            percpu[0].cur_vcpu = v;         /* every pick re-binds (R2) */
            if (v->launched) {
                vcpu_arch_resume(v);
            } else {
                vcpu_arch_load(v);
                v->launched = true;
            }
            if (vcpu_state_step(&v->state, VCPU_EV_LOAD) != 0)
                continue;                   /* swept mid-load: drop */
        } else {
            /* Self re-pick (prev == v): either a yield (RUNNABLE, launched)
             * or THIS pCPU's previous vCPU re-ON'd after CPU_OFF — same
             * pointer, completely different entry: the fresh-boot warm
             * reset (SCTLR=0 + tlbi) is mandatory or the parked context's
             * MMU-on state survives into the secondary entry (manual-gate
             * Day-4 crash). Fork on launched, exactly like the switch-in
             * branch. */
            percpu[0].cur_vcpu = v;
            if (v->launched) {
                if (vcpu_state_step(&v->state, VCPU_EV_LOAD) != 0)
                    continue;
            } else {
                vcpu_arch_load(v);
                v->launched = true;
                if (vcpu_state_step(&v->state, VCPU_EV_LOAD) != 0)
                    continue;
            }
        }
        prev = v;
        vcpu_arch_run(v);
    }
}
