/* SPDX-License-Identifier: TBD */
#include <types.h>
#include <printk.h>
#include <vm.h>
#include <percpu.h>
#include <uart.h>
#include <vuart.h>
#include <cpu.h>
#include "vm_config.h"

struct vm vm[NR_VMS];

void vm_init(void)
{
    for (u32 vmi = 0; vmi < (u32)NR_VMS; vmi++) {
        struct vm *m = &vm[vmi];

        m->id     = vmi;
        m->config = &vm_configs[vmi];

        const struct vm_config *cfg = m->config;

        /*
         * secondary_main derives (VM, vCPU) for a woken pCPU structurally as
         * (id / VCPUS_PER_VM, id % VCPUS_PER_VM), while vpsci.c/vgic_sgi.c
         * derive the pCPU for a given (VM, vCPU) as config->pcpu_base + idx.
         * These two derivations only agree if pcpu_base == vmi * VCPUS_PER_VM
         * for every configured VM. Nothing else checks that at compile time
         * (pcpu_base is data, not derived), so verify it here before any
         * vCPU of this VM can be powered on.
         */
        if (cfg->pcpu_base != (u8)(vmi * (u32)VCPUS_PER_VM)) {
            printk("[hv] BUG: vm[%u].config->pcpu_base=%u != %u "
                   "(vmi*VCPUS_PER_VM); static pCPU mapping violated\n",
                   (unsigned)vmi, (unsigned)cfg->pcpu_base,
                   (unsigned)(vmi * (u32)VCPUS_PER_VM));
            cpu_arch_halt();
        }

        /* Every vCPU knows its VM and its VM-local index; vcpu[1+]'s regs are
         * authored later by the guest-driven PSCI CPU_ON. */
        for (u32 i = 0; i < (u32)VCPUS_PER_VM; i++) {
            m->vcpu[i].owner    = m;
            m->vcpu[i].vcpu_idx = i;
        }

        vm_arch_init(m);
        vcpu_arch_reset(&m->vcpu[0], cfg);

#ifdef CONFIG_GUEST_SVM
        printk("SVM: launching VMID=%u entry=0x%lx ram_pa=0x%lx\n",
               (unsigned)cfg->vmid, (unsigned long)cfg->entry,
               (unsigned long)cfg->ram_pa);
#else
        printk("[hv] Linux guest: VMID=%u entry=0x%lx dtb=0x%lx ram_pa=0x%lx\n",
               (unsigned)cfg->vmid, (unsigned long)cfg->entry,
               (unsigned long)cfg->dtb_ipa, (unsigned long)cfg->ram_pa);
#endif
    }

    /* Global MMIO bus registration: once total, not per VM. */
    vm_arch_devices_init();
    vuart_bus_init();
}

void vm_run(void)
{
    /*
     * Make this pCPU's percpu slot (arm64: TPIDR_EL2) the single source of
     * truth for "current vCPU on this core" before the first guest entry.
     * The exception-entry asm reads the guest frame through
     * &percpu[id]->cur_vcpu (PERCPU_CUR_VCPU) instead of the address of a
     * single global VM (M3.5 Slice 1; supersedes the ADR-0003 trick). CPU0
     * is always VM0 vCPU0.
     */
    percpu[0].cpu_id   = 0;
    percpu[0].cur_vcpu = &vm[0].vcpu[0];
    cpu_arch_set_this_percpu(&percpu[0]);
    vcpu_arch_load(&vm[0].vcpu[0]);

    /*
     * CPU0 owns the physical PL011 RX SPI (BOARD_PL011_IRQ), matching the
     * existing routing. Enable RX + receive-timeout interrupts once, before
     * the guest-entry loop: from here EL2 drains the physical FIFO itself on
     * every RX IRQ and feeds bytes to the vuart model (M5 slice 2).
     */
    uart_rx_irq_enable();

    /*
     * Hypervisor-driven boot of every other VM's boot vCPU (M5 slice 3).
     * VM0's vCPU0 is entered directly below (CPU0 IS VM0's boot pCPU); every
     * other VM's vCPU0 has no guest asking for it via PSCI (there is no guest
     * running yet), so the hypervisor itself issues the physical CPU_ON here,
     * once, before dropping into VM0's guest loop. Unlike psci_cpu_on_guest's
     * guest-driven path (which synchronously waits for `online` because it
     * must honor the guest's CPU_ON return-value contract), this is
     * fire-and-forget: nothing here needs to observe the target pCPU up
     * before proceeding, and secondary_main() publishes `online` on its own
     * for debugging/monitoring only.
     */
    for (u32 i = 1; i < (u32)NR_VMS; i++) {
        u32 p = vm[i].config->pcpu_base;
        s64 r = cpu_arch_power_on(p);
        if (r != 0)
            printk("[hv] VM%u boot pCPU%u CPU_ON failed (%d)\n",
                   (unsigned)i, (unsigned)p, (int)r);
    }

    for (;;) {
        /*
         * The guest's interactive console is the emulated vuart (M5 slice 2):
         * Stage-2 traps its PL011 IPA window and irq_handler.c's PL011 branch
         * drains the physical FIFO on EL2's behalf, so this loop still has no
         * device emulation to poll. The EL2 virtio device model was removed in
         * ADR-0013 (device emulation moves to a future Service-VM userspace DM).
         */
        vcpu_arch_run(&vm[0].vcpu[0]);
        /* Most synchronous exits (MMIO data abort, PSCI, unknown HVC) eret
         * straight back to the guest from el1_sync_handler and never return to
         * C. vcpu_arch_run returns here only on the timer IRQ exit; the loop then
         * re-enters the guest so successive timer PPIs (injected by
         * el2_irq_handler) keep advancing guest time.
         *
         * The HC_GUEST_DONE HVC (an M2 debug hook a real Linux guest never
         * issues) calls hv_restore(), which restores g_hv_ctx and rets to the
         * most recent vcpu_arch_run call site -- i.e. back into this same loop body,
         * not out of vm_run. With a single UP vCPU and no scheduler this loop
         * never exits; an exit path out of vm_run arrives with M3.5 (SMP). */
    }
}
