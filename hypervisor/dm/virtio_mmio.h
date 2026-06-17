/* SPDX-License-Identifier: TBD */
#ifndef HV_DM_VIRTIO_MMIO_H
#define HV_DM_VIRTIO_MMIO_H

#include <types.h>
#include "../arch/arm64/vmexit/mmio.h"   /* struct mmio_access, mmio_handler_t */
#include "virtqueue.h"

#define VIRTIO_MMIO_NUM_QUEUES   2U   /* console: RX (0) + TX (1) */

struct virtio_mmio_dev;   /* forward */

/*
 * Device backend. The transport owns queue setup + IRQ; the backend owns the
 * device id, feature bits, and what to do when a queue is notified.
 */
struct virtio_backend {
    u32  device_id;          /* virtio device id (3 = console)       */
    u64  device_features;    /* host feature bits                    */
    /* Called from QueueNotify: process all available buffers on `qidx`.
     * Returns true if at least one buffer was completed (⇒ raise IRQ). */
    bool (*queue_notify)(struct virtio_mmio_dev *dev, u16 qidx);
};

struct virtio_mmio_dev {
    u64 base;                /* MMIO frame guest IPA            */
    const struct virtio_backend *backend;
    u32 irq_intid;           /* SPI INTID raised on used-buffer */

    struct virtqueue vq[VIRTIO_MMIO_NUM_QUEUES];

    u32 queue_sel;
    u32 status;
    u32 interrupt_status;    /* bit0 = VRING */
    u32 device_features_sel;
    u32 driver_features_sel;
    u64 driver_features;
};

#define VIRTIO_MMIO_INT_VRING   0x1U

/* Initialise device state (zeroes queues, wires backend + irq) but does not
 * touch the bus. */
void virtio_mmio_dev_init(struct virtio_mmio_dev *dev, u64 base,
                          const struct virtio_backend *backend, u32 irq_intid);

/* The M3.1 mmio_handler_t registered on the bus; ctx is the virtio_mmio_dev. */
int virtio_mmio_handler(struct mmio_access *acc, void *ctx);

/* Raise the used-buffer (VRING) interrupt to the guest. */
void virtio_mmio_signal(struct virtio_mmio_dev *dev);

#endif /* HV_DM_VIRTIO_MMIO_H */
