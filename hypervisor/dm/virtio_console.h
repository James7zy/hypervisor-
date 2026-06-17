/* SPDX-License-Identifier: TBD */
#ifndef HV_DM_VIRTIO_CONSOLE_H
#define HV_DM_VIRTIO_CONSOLE_H

#include <types.h>

/* virtio-console: device id 3. RX = queue 0, TX = queue 1. */
#define VIRTIO_CONSOLE_DEVICE_ID  3U
#define VIRTIO_CONSOLE_RX_QUEUE   0U
#define VIRTIO_CONSOLE_TX_QUEUE   1U

/* SPI INTID raised on used-buffer completion (dts SPI 16 ⇒ GIC INTID 48). */
#define VIRTIO_CONSOLE_INTID      48U

/* Register the virtio-mmio console frame on the M3.1 bus + init device state. */
void virtio_console_init(void);

/* Poll the PL011 RX FIFO; if a byte is present and an RX buffer is posted,
 * deliver it to the guest and raise the used-buffer IRQ. Called from the
 * vm_run loop. Polling-based: the hv has no input IRQ path in UP bring-up. */
void virtio_console_rx_poll(void);

#endif /* HV_DM_VIRTIO_CONSOLE_H */
