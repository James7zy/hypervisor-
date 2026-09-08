/* SPDX-License-Identifier: TBD */
/*
 * Physical console RX arbitration (see console.h).
 *
 * Moved out of the EL2 IRQ handler: deciding which consumer owns a typed
 * character is console policy, not interrupt-controller work. The GIC layer
 * now only recognises the PL011 SPI and calls console_rx_drain().
 */
#include <types.h>
#include <vm.h>
#include <uart.h>
#include <vuart.h>
#include <hv_shell.h>
#include "console.h"

/* Owned here rather than in vuart.c: focus is arbitration state shared by the
 * shell and the vuarts, not part of any one VM's UART model. */
u32 console_focus = 0;

void console_rx_drain(void)
{
    /*
     * Room-checking is per-consumer and must happen BEFORE uart_getc(),
     * because a physical DR read is destructive (it pops the hardware FIFO).
     * If the consumer cannot take the byte we must stop the loop WITHOUT
     * consuming it -- QEMU's pl011 model backpressures its chardev on the
     * physical FIFO having room, so an un-popped byte simply waits for a
     * later RX IRQ instead of being lost. The shell always has room (its line
     * buffer submits-and-resets when full), so only the vuart path needs the
     * check -- and it must be re-evaluated every iteration against the
     * CURRENT consumer, since a Ctrl-T mid-drain switches it.
     */
    for (;;) {
        if (!shell_active && !vuart_rx_has_room(&vm[console_focus])) {
            break;
        }

        int c = uart_getc();
        if (c < 0) {
            break;
        }

        /*
         * Ctrl-T (HV_SHELL_ESCAPE_KEY) is intercepted here and delivered to
         * NOBODY: it toggles the EL2 shell (hv_shell.h). This is the right
         * layer for it precisely because both consumers below are downstream
         * of that decision.
         */
        if (c == (int)HV_SHELL_ESCAPE_KEY) {
            if (shell_active) {
                hv_shell_exit();
            } else {
                hv_shell_enter();
            }
            continue;
        }

        /* The vuart buffers the byte and injects a SOFTWARE SPI into that
         * guest's vGIC if its virtual IMSC is unmasked. */
        if (shell_active) {
            hv_shell_rx((u8)c);
        } else {
            vuart_rx(&vm[console_focus], (u8)c);
        }
    }
}
