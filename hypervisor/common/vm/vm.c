/* SPDX-License-Identifier: TBD */
#include <types.h>
#include <printk.h>
#include <vm.h>
#include <percpu.h>
#include <uart.h>
#include <vuart.h>
#include <cpu.h>
#include <vm_config.h>

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

    struct vcpu *v = &vm[0].vcpu[0];
    vcpu_arch_load(v);   /* RUNNABLE -> RUNNING below; never left this pCPU */
    (void)vcpu_state_step(&v->state, VCPU_EV_LOAD);

    for (;;) {
        /*
         * The guest's interactive console is the emulated vuart (M5 slice 2):
         * the second-stage tables trap its console MMIO window and the arch's
         * console-UART interrupt branch drains the physical FIFO on the
         * hypervisor's behalf (arm64: Stage-2 / irq_handler.c), so this loop
         * still has no device emulation to poll (ADR-0013 moved device
         * emulation to a future Service-VM userspace DM).
         *
         * vcpu_arch_run returns on the exits that need this loop: the timer
         * IRQ exit (re-enter below so successive timer interrupts keep
         * advancing guest time) and power-down unwinds (CPU_OFF/SYSTEM_OFF
         * via hv_restore; state is then OFF and this pCPU waits to be
         * re-authorized, M11 slice A — CPU_OFF of vcpu0 no longer halts the
         * pCPU, a sibling CPU_ON can revive it). Most synchronous exits still
         * resume the guest directly inside the arch's exit handler and never
         * get here.
         */
        /* Acquire load: orders this pCPU's subsequent state reads after the
         * sender's state->dsb->kick publication (same role as the dmb the
         * arch side uses; common code uses the __atomic builtins per
         * include/spinlock.h's precedent — ADR-0015 bans asm here). */
        while (__atomic_load_n(&v->state, __ATOMIC_ACQUIRE) == VCPU_OFF)
            cpu_arch_idle();
        if (v->state == VCPU_ON_PENDING)
            (void)vcpu_state_step(&v->state, VCPU_EV_PICK);
        if (v->state == VCPU_RUNNABLE) {
            vcpu_arch_load(v);
            (void)vcpu_state_step(&v->state, VCPU_EV_LOAD);
        }
        vcpu_arch_run(v);
    }
}
