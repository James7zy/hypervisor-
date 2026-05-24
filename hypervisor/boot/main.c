/* SPDX-License-Identifier: TBD */
#include <types.h>
#include <uart.h>
#include <printk.h>
#include <board.h>

extern u64 read_currentel(void);
extern void cpu_wfi(void);

void hypervisor_main(uintptr_t dtb_phys)
{
    (void)dtb_phys;

    uart_init(BOARD_UART_BASE);

    u64 el = read_currentel();
    printk("[hv] Hello from EL2 on %s, CurrentEL=0x%lx\n", board_name, el);

    printk("hello hypervisor \n");

    for (;;)
        cpu_wfi();
}
