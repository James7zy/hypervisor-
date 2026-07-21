/* SPDX-License-Identifier: TBD */
#ifndef HV_DM_HV_SHELL_H
#define HV_DM_HV_SHELL_H
#include <types.h>

/*
 * EL2 debug shell on the physical console (ACRN's hypervisor/debug/shell.c
 * plays the same role there).
 *
 * Ctrl-T (0x14) on the physical UART is a two-state toggle handled by
 * irq_handler.c's PL011 RX-drain loop: it enters this shell, or exits back to
 * whichever VM console_focus names (vuart.h). While shell_active is true, NO
 * physical RX byte reaches any guest -- every byte is line-edited here and
 * interpreted as a command on Enter.
 *
 * Not multi-core safe by design: the physical PL011 SPI is routed to pCPU0
 * only (gic_v3.c), so every byte and every state change below happens on that
 * one core inside el2_irq_handler. Nothing else in the hypervisor touches
 * this state.
 */

/* True while the physical console is owned by the shell rather than a guest.
 * Read by irq_handler.c to pick a dispatch; written only by the two functions
 * below. Plain extern matches this codebase's convention for simple
 * cross-file state (cf. console_focus in vuart.h, struct vm vm[]). */
extern bool shell_active;

/* Ctrl-T while not in the shell: take the console and print the prompt. */
void hv_shell_enter(void);

/* Ctrl-T while in the shell: hand the console back to vm[console_focus].
 * Deliberately prints nothing -- the guest's own output resumes and is the
 * user's feedback that the switch happened. */
void hv_shell_exit(void);

/* Feed one received byte to the line editor. On Enter, the accumulated line
 * is parsed and dispatched, then a fresh prompt is printed. Only called while
 * shell_active is true. */
void hv_shell_rx(u8 ch);

#endif /* HV_DM_HV_SHELL_H */
