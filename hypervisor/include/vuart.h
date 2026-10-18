/* SPDX-License-Identifier: TBD */
#ifndef HV_DM_VUART_H
#define HV_DM_VUART_H
#include <types.h>

struct vm;

/* Register the shared console UART window on the MMIO bus (call once). */
void vuart_bus_init(void);
/* Hypervisor RX path: push one received char into this VM's vuart and raise its
 * virtual RX interrupt if unmasked. Caller (console_rx_drain) must
 * check vuart_rx_has_room() BEFORE popping a byte off the
 * physical UART with uart_getc() -- a physical read is destructive, so
 * dropping room-checking to inside vuart_rx would be too late to avoid
 * losing the byte. */
void vuart_rx(struct vm *m, u8 ch);
/* Locked snapshot, not a reservation: only pCPU0 produces; consumers free room. */
bool vuart_rx_has_room(struct vm *m);
#endif
