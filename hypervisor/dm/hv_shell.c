/* SPDX-License-Identifier: TBD */
/*
 * EL2 debug shell -- see hv_shell.h for the console-ownership contract.
 *
 * Runs entirely inside el2_irq_handler on pCPU0 (the only core the physical
 * PL011 SPI is routed to), so none of the state here needs locking. Output
 * goes through printk/console_putc, which serialize against guest TX under
 * the shared console lock (lib/print.c).
 */
#include <types.h>
#include <printk.h>
#include <vm.h>
#include <vuart.h>
#include <hv_shell.h>

#define HV_SHELL_PROMPT "hv> "

bool shell_active = false;

void hv_shell_enter(void)
{
    shell_active = true;
    printk("\n");
    printk(HV_SHELL_PROMPT);
}

void hv_shell_exit(void)
{
    shell_active = false;
    printk("\n");
}

void hv_shell_rx(u8 ch)
{
    (void)ch;   /* line editing arrives in Task 2 */
}
