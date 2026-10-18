/* SPDX-License-Identifier: TBD */
#ifndef HV_DM_CONSOLE_H
#define HV_DM_CONSOLE_H
#include <types.h>

/*
 * Physical console RX arbitration.
 *
 * The hypervisor owns the physical console UART exclusively (M10 / ADR-0014),
 * so exactly one physical UART must serve several consumers: the debug shell
 * (hv_shell.h) and one emulated vuart per VM (vuart.h). This module owns the
 * policy that decides, byte by byte, which of them a received character goes
 * to -- the Ctrl-T escape, the shell/guest split, and the focused-VM routing.
 *
 * It is deliberately separate from both consumers: putting this in vuart.c
 * would make the guest UART model depend on the debug shell, and putting it in
 * the physical UART driver would make the lowest-level console driver depend on VMs
 * and shell state. It is equally not interrupt-controller code, which is why
 * it no longer lives in the arch's IRQ handler (arm64: the GIC handler).
 *
 * TX is not arbitrated: every VM's output reaches the physical UART
 * regardless of focus or shell state. Only RX is routed.
 */

/* Which VM receives physical console RX when the debug shell is NOT active
 * (0..NR_VMS-1). A successful `vm_console <n>` command (hv_shell.c) sets this
 * value and immediately leaves the shell; Ctrl-T can also leave the shell
 * without changing it. Plain extern, matching this codebase's existing
 * convention for simple cross-file state (see e.g. struct vm vm[] itself). */
extern u32 console_focus;

/* Drain the physical console UART's RX FIFO and deliver each byte to its
 * arbitrated consumer. Called from the arch's console-UART interrupt branch
 * with IRQs masked on the pCPU the interrupt is routed to (pCPU0) -- all
 * console state below is single-core by that routing, not by locking.
 * (arm64: the PL011 branch of el2_irq_handler; the SPI is routed in gic_init.)
 *
 * Returns having drained as much as the current consumer can accept; a byte
 * left unread stays in the hardware FIFO for a later RX interrupt. */
void console_rx_drain(void);

#endif /* HV_DM_CONSOLE_H */
