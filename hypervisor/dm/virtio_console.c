/* SPDX-License-Identifier: TBD */
#include <types.h>
#include <printk.h>
#include <uart.h>
#include <board.h>
#include "virtio_mmio.h"
#include "virtqueue.h"
#include "gpa.h"
#include "virtio_console.h"

/* virtio-mmio frame for the console (matches QEMU virt first slot). */
#define VIRTIO_CONSOLE_BASE   0x0A000000ULL
#define VIRTIO_CONSOLE_LEN    0x200ULL

/*
 * Console offers no optional features. (VIRTIO_F_VERSION_1, bit 32, is
 * mandatory for modern devices; advertising it keeps the modern driver happy.)
 */
#define VIRTIO_F_VERSION_1    (1ULL << 32)

static bool console_queue_notify(struct virtio_mmio_dev *dev, u16 qidx);

static const struct virtio_backend console_backend = {
    .device_id       = VIRTIO_CONSOLE_DEVICE_ID,
    .device_features = VIRTIO_F_VERSION_1,
    .queue_notify    = console_queue_notify,
};

static struct virtio_mmio_dev g_console;

/* TX: drain every readable byte of one available chain to the PL011. */
static u32 console_tx_chain(struct virtqueue *vq, u16 head)
{
    struct virtq_desc d;
    u32 written = 0;
    u16 idx = head;

    for (;;) {
        if (!virtqueue_read_desc(vq, idx, &d))
            break;

        /* TX descriptors are driver-readable (no F_WRITE). */
        if ((d.flags & VIRTQ_DESC_F_WRITE) == 0U && d.len != 0U) {
            const u8 *buf = (const u8 *)gpa_to_hva_len(d.addr, (u64)d.len);
            if (buf != NULL) {
                for (u32 i = 0; i < d.len; i++)
                    uart_putc((char)buf[i]);
                written += d.len;
            }
        }

        if ((d.flags & VIRTQ_DESC_F_NEXT) == 0U)
            break;
        idx = d.next;
    }
    return written;
}

static bool console_queue_notify(struct virtio_mmio_dev *dev, u16 qidx)
{
    if (qidx != VIRTIO_CONSOLE_TX_QUEUE)
        return false;   /* RX-queue notify: buffers consumed by the pump */

    struct virtqueue *vq = &dev->vq[VIRTIO_CONSOLE_TX_QUEUE];
    bool any = false;
    u16 head;

    while (virtqueue_pop(vq, &head)) {
        u32 written = console_tx_chain(vq, head);
        virtqueue_push(vq, head, written);
        any = true;
    }
    return any;
}

void virtio_console_init(void)
{
    virtio_mmio_dev_init(&g_console, VIRTIO_CONSOLE_BASE,
                         &console_backend, VIRTIO_CONSOLE_INTID);

    if (mmio_bus_register(VIRTIO_CONSOLE_BASE, VIRTIO_CONSOLE_LEN,
                          virtio_mmio_handler, &g_console) != 0)
        printk("[hv] virtio-console: bus full, registration failed\n");
    else
        printk("[hv] virtio-console: frame 0x%lx len 0x%lx intid %u\n",
               (unsigned long)VIRTIO_CONSOLE_BASE,
               (unsigned long)VIRTIO_CONSOLE_LEN,
               (unsigned)VIRTIO_CONSOLE_INTID);
}

void virtio_console_rx_poll(void)
{
    struct virtqueue *vq = &g_console.vq[VIRTIO_CONSOLE_RX_QUEUE];

    if (!vq->ready)
        return;

    for (;;) {
        int c = uart_getc();
        if (c < 0)
            return;                     /* no input pending */

        u16 head;
        if (!virtqueue_pop(vq, &head)) {
            /* No posted RX buffer; drop the byte (guest not yet listening). */
            return;
        }

        struct virtq_desc d;
        if (!virtqueue_read_desc(vq, head, &d) ||
            (d.flags & VIRTQ_DESC_F_WRITE) == 0U || d.len == 0U) {
            virtqueue_push(vq, head, 0);
            continue;
        }

        u8 *buf = (u8 *)gpa_to_hva_len(d.addr, (u64)d.len);
        if (buf == NULL) {
            virtqueue_push(vq, head, 0);
            continue;
        }

        buf[0] = (u8)c;
        virtqueue_push(vq, head, 1);
        virtio_mmio_signal(&g_console);
    }
}
