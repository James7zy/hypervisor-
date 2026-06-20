/* SPDX-License-Identifier: TBD */
#include <types.h>
#include <printk.h>
#include <vm.h>
#include "vm_config.h"
#include "stage2.h"
#include <vgic.h>
#include "../../arch/arm64/irq/vgic_v3_mmio.h"
#include "virtio_console.h"

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

    virtio_console_init();
}

void vm_run(void)
{
    stage2_activate(&g_vm.vcpu);
    vgic_restore(&g_vm.vcpu);

    for (;;) {
        /*
         * NOTE: do NOT poll virtio-console RX here. The Linux guest runs its
         * interactive console on console=ttyAMA0 (the PL011 passthrough), so it
         * reads the PL011 RX FIFO directly. virtio_console_rx_poll() calls
         * uart_getc(), which drains that same FIFO and would steal the guest's
         * input. The virtio-console (hvc0) RX path is only needed if the guest
         * is switched to console=hvc0 (see docs/guest-initramfs.md).
         */
        vcpu_run(&g_vm.vcpu);
        /* vcpu_run returns to the hv on each handled exit (MMIO data abort,
         * HVC) and on the timer IRQ exit. Re-enter the guest so successive
         * timer PPIs (injected by el2_irq_handler) keep advancing guest time.
         * The HVC "done" path still calls hv_restore(), which longjmps past
         * this loop and out of vm_run. */
    }
}
