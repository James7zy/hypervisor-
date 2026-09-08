/* SPDX-License-Identifier: TBD */
#include <types.h>
#include <printk.h>
#include <vm.h>
#include <percpu.h>
#include <uart.h>
#include <vuart.h>
#include <psci.h>
#include "vm_config.h"
#include "stage2.h"
#include <vgic.h>
#include <vgic_v3_mmio.h>

/* secondary_entry (head.S): EL2 PA a secondary core is powered on at. */
extern char secondary_entry[];

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
         * (id / VCPUS_PER_VM, id % VCPUS_PER_VM), while psci.c/vgic_sgi.c
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
            for (;;)
                asm volatile("wfi");
        }

        /* Every vCPU knows its VM and its VM-local index; vcpu[1+]'s regs are
         * authored later by the guest-driven PSCI CPU_ON. */
        for (u32 i = 0; i < (u32)VCPUS_PER_VM; i++) {
            m->vcpu[i].owner    = m;
            m->vcpu[i].vcpu_idx = i;
        }

        struct vcpu *v = &m->vcpu[0];

        /*
         * arm64 Linux boot protocol (Documentation/arm64/booting.rst):
         *   x0 = physical address of the DTB (here: guest IPA of the DTB)
         *   x1 = x2 = x3 = 0 (reserved, must be zero)
         *   PC = kernel entry; CPU in EL1h, DAIF masked, MMU/caches off.
         */
        v->regs.x[0] = (u64)cfg->dtb_ipa;
        v->regs.x[1] = 0;
        v->regs.x[2] = 0;
        v->regs.x[3] = 0;

        /*
         * SPSR_EL2 = 0x3C5: M[4:0]=00101 (EL1h, SP_EL1), DAIF=1111 (all masked).
         */
        v->regs.elr_el2  = cfg->entry;
        v->regs.spsr_el2 = 0x3C5ULL;
        v->regs.sp_el1   = cfg->mem_base + cfg->mem_size - 0x10UL;

        /*
         * HCR_EL2: VM(0)|FMO(3)|IMO(4)|AMO(5)|RW(31) set; HCD(29) clear (allow HVC).
         * RW=1: EL1 executes in AArch64 state.
         */
        v->hcr_el2 = (1ULL << 0) | (1ULL << 3) | (1ULL << 4) | (1ULL << 5) |
                     (1ULL << 31);

        stage2_init(m);

        vgic_init(v);

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
    vgicv3_mmio_init();
    vuart_bus_init();
}

void vm_run(void)
{
    /*
     * Make TPIDR_EL2 the single source of truth for "current vCPU on this
     * core" before the first guest entry. The exception-entry asm reads the
     * guest frame through &percpu[id]->cur_vcpu (PERCPU_CUR_VCPU) instead of
     * the address of a single global VM (M3.5 Slice 1; supersedes the
     * ADR-0003 trick). CPU0 is always VM0 vCPU0.
     */
    percpu[0].cpu_id   = 0;
    percpu[0].cur_vcpu = &vm[0].vcpu[0];
    __asm__ volatile("msr tpidr_el2, %0" :: "r"(&percpu[0]));
    /* Virtual MPIDR for vCPU0: Aff0 = 0 (vCPU1 sets Aff0=1 in secondary_main). */
    __asm__ volatile("msr vmpidr_el2, %0" :: "r"(0ULL));
    __asm__ volatile("isb");

    stage2_activate(&vm[0].vcpu[0]);
    vgic_restore(&vm[0].vcpu[0]);

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
        s64 r = psci_cpu_on((u64)p, (u64)(uintptr_t)secondary_entry, (u64)p);
        if (r != (s64)PSCI_RET_SUCCESS)
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
        vcpu_run(&vm[0].vcpu[0]);
        /* Most synchronous exits (MMIO data abort, PSCI, unknown HVC) eret
         * straight back to the guest from el1_sync_handler and never return to
         * C. vcpu_run returns here only on the timer IRQ exit; the loop then
         * re-enters the guest so successive timer PPIs (injected by
         * el2_irq_handler) keep advancing guest time.
         *
         * The HC_GUEST_DONE HVC (an M2 debug hook a real Linux guest never
         * issues) calls hv_restore(), which restores g_hv_ctx and rets to the
         * most recent vcpu_run call site -- i.e. back into this same loop body,
         * not out of vm_run. With a single UP vCPU and no scheduler this loop
         * never exits; an exit path out of vm_run arrives with M3.5 (SMP). */
    }
}
