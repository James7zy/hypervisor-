/* SPDX-License-Identifier: TBD */
#include <types.h>
#include <uart.h>
#include <printk.h>
#include <board_info.h>
#include <cpu.h>
#include <vm.h>

void hypervisor_main(uintptr_t dtb_phys)
{
    (void)dtb_phys;

    uart_init(board_uart_base);

    printk("\n");
    printk("  H   H Y   Y PPPP  EEEEE RRRR  V   V IIIII SSSSS  OOO  RRRR  \n");
    printk("  H   H  Y Y  P   P E     R   R V   V   I   S     O   O R   R  \n");
    printk("  HHHHH   Y   PPPP  EEE   RRRR  V   V   I   SSSSS O   O RRRR   \n");
    printk("  H   H   Y   P     E     R R    V V    I       S  O   O R R    \n");
    printk("  H   H   Y   P     EEEEE R  R    V   IIIII SSSSS   OOO  R  R   \n");
    printk("\n");

    cpu_arch_init();

    vm_init();   /* builds vCPU0 Stage-2 / VMID, which vCPU1 shares (M3.5) */

    /* vCPU1 is now brought up on demand by the guest's PSCI CPU_ON (handled in
     * psci_cpu_on_guest), not by the hypervisor. */
    vm_run();

    cpu_arch_halt();
}
