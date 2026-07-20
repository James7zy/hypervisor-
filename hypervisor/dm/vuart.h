/* SPDX-License-Identifier: TBD */
#ifndef HV_DM_VUART_H
#define HV_DM_VUART_H
#include <types.h>

/*
 * Buffer depth for the emulated RX ring. The real PL011 hardware FIFO is only
 * 16 bytes deep, but QEMU's pl011 model (hw/char/pl011.c) gates chardev
 * delivery on that FIFO having room (pl011_can_receive: read_count < 16), so
 * a single physical RX IRQ can hand EL2 up to a full 16-byte backlog in one
 * drain (irq_handler.c's PL011 branch drains the physical FIFO to empty
 * before the guest ever runs, unlike the old passthrough design where the
 * guest drained incrementally by polling hardware itself). Size this ring
 * with headroom above that worst-case single-IRQ burst so a normal typed
 * command line does not lose bytes before the guest gets scheduled to drain
 * it via DR reads.
 */
#define VUART_RX_FIFO 256U

struct vuart {
    u8  rx_buf[VUART_RX_FIFO];
    u32 rx_head, rx_tail;        /* head==tail -> empty */
    u32 imsc, ris;               /* interrupt mask / raw status      */
    u32 ibrd, fbrd, lcr_h, cr, ifls;  /* stored, no modelled effect  */
};

struct vm;
/* Register the shared PL011 IPA region on the MMIO bus (call once). */
void vuart_bus_init(void);
/* EL2 RX path: push one received char into this VM's vuart and raise its
 * virtual RX interrupt (SPI 33) if unmasked. Caller (irq_handler.c's drain
 * loop) must check vuart_rx_has_room() BEFORE popping a byte off the
 * physical UART with uart_getc() -- a physical read is destructive, so
 * dropping room-checking to inside vuart_rx would be too late to avoid
 * losing the byte. */
void vuart_rx(struct vm *m, u8 ch);
/* True if the virtual RX ring has space for at least one more byte. */
bool vuart_rx_has_room(const struct vm *m);
#endif
