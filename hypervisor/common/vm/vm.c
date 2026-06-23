/* SPDX-License-Identifier: TBD */
#include <types.h>
#include <printk.h>
#include <vm.h>
#include "vm_config.h"
#include "stage2.h"
#include <vgic.h>
#include "../../arch/arm64/irq/vgic_v3_mmio.h"

/* Non-static: vmexit_asm.S references g_vm by symbol */
struct vm g_vm;

void vm_init(void)
{
    g_vm.config = &linux_config;

    const struct vm_config *cfg = g_vm.config;

    /*
     * arm64 Linux boot protocol (Documentation/arm64/booting.rst):
     *   x0 = physical address of the DTB (here: guest IPA of the DTB)
     *   x1 = x2 = x3 = 0 (reserved, must be zero)
     *   PC = kernel entry; CPU in EL1h, DAIF masked, MMU/caches off.
     */
    g_vm.vcpu.regs.x[0] = (u64)cfg->dtb_ipa;
    g_vm.vcpu.regs.x[1] = 0;
    g_vm.vcpu.regs.x[2] = 0;
    g_vm.vcpu.regs.x[3] = 0;

    /*
     * SPSR_EL2 = 0x3C5: M[4:0]=00101 (EL1h, SP_EL1), DAIF=1111 (all masked).
     */
    g_vm.vcpu.regs.elr_el2  = cfg->entry;
    g_vm.vcpu.regs.spsr_el2 = 0x3C5ULL;
    g_vm.vcpu.regs.sp_el1   = cfg->mem_base + cfg->mem_size - 0x10UL;

    /*
     * HCR_EL2: VM(0)|FMO(3)|IMO(4)|AMO(5)|RW(31) set; HCD(29) clear (allow HVC).
     * RW=1: EL1 executes in AArch64 state.
     */
    g_vm.vcpu.hcr_el2 = (1ULL << 0) | (1ULL << 3) | (1ULL << 4) | (1ULL << 5) |
                        (1ULL << 31);

    stage2_init(&g_vm.vcpu, (u32)cfg->vmid, cfg->ram_pa);

    vgic_init(&g_vm.vcpu);

    printk("[hv] Linux guest: VMID=%u entry=0x%lx dtb=0x%lx ram_pa=0x%lx\n",
           (unsigned)cfg->vmid, (unsigned long)cfg->entry,
           (unsigned long)cfg->dtb_ipa, (unsigned long)cfg->ram_pa);

    vgicv3_mmio_init();
}

void vm_run(void)
{
    stage2_activate(&g_vm.vcpu);
    vgic_restore(&g_vm.vcpu);

    for (;;) {
        /*
         * The guest's interactive console is console=ttyAMA0 (the PL011
         * passthrough); it reads the PL011 RX FIFO directly, so this loop has no
         * device emulation to poll. The EL2 virtio device model was removed in
         * ADR-0013 (device emulation moves to a future Service-VM userspace DM).
         */
        vcpu_run(&g_vm.vcpu);
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
