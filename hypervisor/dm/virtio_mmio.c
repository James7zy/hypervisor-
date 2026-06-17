/* SPDX-License-Identifier: TBD */
#include <types.h>
#include <printk.h>
#include <vm.h>
#include <vgic.h>
#include "virtio_mmio.h"
#include "virtqueue.h"

/* ── virtio-mmio register offsets (VERSION 2 / modern) ── */
#define VIRTIO_MMIO_MAGIC_VALUE        0x000
#define VIRTIO_MMIO_VERSION            0x004
#define VIRTIO_MMIO_DEVICE_ID          0x008
#define VIRTIO_MMIO_VENDOR_ID          0x00C
#define VIRTIO_MMIO_DEVICE_FEATURES    0x010
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL 0x014
#define VIRTIO_MMIO_DRIVER_FEATURES    0x020
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL 0x024
#define VIRTIO_MMIO_QUEUE_SEL          0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX      0x034
#define VIRTIO_MMIO_QUEUE_NUM          0x038
#define VIRTIO_MMIO_QUEUE_READY        0x044
#define VIRTIO_MMIO_QUEUE_NOTIFY       0x050
#define VIRTIO_MMIO_INTERRUPT_STATUS   0x060
#define VIRTIO_MMIO_INTERRUPT_ACK      0x064
#define VIRTIO_MMIO_STATUS             0x070
#define VIRTIO_MMIO_QUEUE_DESC_LOW     0x080
#define VIRTIO_MMIO_QUEUE_DESC_HIGH    0x084
#define VIRTIO_MMIO_QUEUE_DRIVER_LOW   0x090
#define VIRTIO_MMIO_QUEUE_DRIVER_HIGH  0x094
#define VIRTIO_MMIO_QUEUE_DEVICE_LOW   0x0A0
#define VIRTIO_MMIO_QUEUE_DEVICE_HIGH  0x0A4
#define VIRTIO_MMIO_CONFIG_GENERATION  0x0FC
#define VIRTIO_MMIO_CONFIG             0x100

#define VIRTIO_MMIO_MAGIC   0x74726976U   /* "virt" */
#define VIRTIO_MMIO_VERS    2U            /* modern */
#define VIRTIO_VENDOR_QEMU  0x554D4551U   /* "QEMU" */

void virtio_mmio_dev_init(struct virtio_mmio_dev *dev, u64 base,
                          const struct virtio_backend *backend, u32 irq_intid)
{
    dev->base                = base;
    dev->backend             = backend;
    dev->irq_intid           = irq_intid;
    dev->queue_sel           = 0;
    dev->status              = 0;
    dev->interrupt_status    = 0;
    dev->device_features_sel = 0;
    dev->driver_features_sel = 0;
    dev->driver_features     = 0;
    for (u32 i = 0; i < VIRTIO_MMIO_NUM_QUEUES; i++)
        virtqueue_reset(&dev->vq[i]);
}

void virtio_mmio_signal(struct virtio_mmio_dev *dev)
{
    dev->interrupt_status |= VIRTIO_MMIO_INT_VRING;
    vgic_inject_spi(&g_vm.vcpu, dev->irq_intid);
}

static struct virtqueue *cur_vq(struct virtio_mmio_dev *dev)
{
    if (dev->queue_sel >= VIRTIO_MMIO_NUM_QUEUES)
        return NULL;
    return &dev->vq[dev->queue_sel];
}

static u32 virtio_mmio_read(struct virtio_mmio_dev *dev, u64 off)
{
    struct virtqueue *vq;

    switch (off) {
    case VIRTIO_MMIO_MAGIC_VALUE:   return VIRTIO_MMIO_MAGIC;
    case VIRTIO_MMIO_VERSION:       return VIRTIO_MMIO_VERS;
    case VIRTIO_MMIO_DEVICE_ID:     return dev->backend->device_id;
    case VIRTIO_MMIO_VENDOR_ID:     return VIRTIO_VENDOR_QEMU;
    case VIRTIO_MMIO_DEVICE_FEATURES:
        if (dev->device_features_sel == 0U)
            return (u32)(dev->backend->device_features & 0xFFFFFFFFU);
        return (u32)(dev->backend->device_features >> 32);
    case VIRTIO_MMIO_QUEUE_NUM_MAX: return VIRTQ_MAX_SIZE;
    case VIRTIO_MMIO_QUEUE_READY:
        vq = cur_vq(dev);
        return (vq != NULL && vq->ready) ? 1U : 0U;
    case VIRTIO_MMIO_INTERRUPT_STATUS: return dev->interrupt_status;
    case VIRTIO_MMIO_STATUS:           return dev->status;
    case VIRTIO_MMIO_CONFIG_GENERATION: return 0U;
    default:
        return 0U;   /* unknown / config space: read as zero */
    }
}

static void virtio_mmio_write(struct virtio_mmio_dev *dev, u64 off, u32 val)
{
    struct virtqueue *vq;

    switch (off) {
    case VIRTIO_MMIO_DEVICE_FEATURES_SEL:
        dev->device_features_sel = val;
        break;
    case VIRTIO_MMIO_DRIVER_FEATURES_SEL:
        dev->driver_features_sel = val;
        break;
    case VIRTIO_MMIO_DRIVER_FEATURES:
        if (dev->driver_features_sel == 0U)
            dev->driver_features =
                (dev->driver_features & 0xFFFFFFFF00000000ULL) | (u64)val;
        else
            dev->driver_features =
                (dev->driver_features & 0x00000000FFFFFFFFULL) | ((u64)val << 32);
        break;
    case VIRTIO_MMIO_QUEUE_SEL:
        dev->queue_sel = val;
        break;
    case VIRTIO_MMIO_QUEUE_NUM:
        vq = cur_vq(dev);
        if (vq != NULL)
            vq->num = (val <= VIRTQ_MAX_SIZE) ? (u16)val : (u16)VIRTQ_MAX_SIZE;
        break;
    case VIRTIO_MMIO_QUEUE_READY:
        vq = cur_vq(dev);
        if (vq != NULL)
            vq->ready = (val != 0U);
        break;
    case VIRTIO_MMIO_QUEUE_DESC_LOW:
        vq = cur_vq(dev);
        if (vq != NULL)
            vq->desc_gpa = (vq->desc_gpa & 0xFFFFFFFF00000000ULL) | (u64)val;
        break;
    case VIRTIO_MMIO_QUEUE_DESC_HIGH:
        vq = cur_vq(dev);
        if (vq != NULL)
            vq->desc_gpa = (vq->desc_gpa & 0x00000000FFFFFFFFULL) | ((u64)val << 32);
        break;
    case VIRTIO_MMIO_QUEUE_DRIVER_LOW:
        vq = cur_vq(dev);
        if (vq != NULL)
            vq->avail_gpa = (vq->avail_gpa & 0xFFFFFFFF00000000ULL) | (u64)val;
        break;
    case VIRTIO_MMIO_QUEUE_DRIVER_HIGH:
        vq = cur_vq(dev);
        if (vq != NULL)
            vq->avail_gpa = (vq->avail_gpa & 0x00000000FFFFFFFFULL) | ((u64)val << 32);
        break;
    case VIRTIO_MMIO_QUEUE_DEVICE_LOW:
        vq = cur_vq(dev);
        if (vq != NULL)
            vq->used_gpa = (vq->used_gpa & 0xFFFFFFFF00000000ULL) | (u64)val;
        break;
    case VIRTIO_MMIO_QUEUE_DEVICE_HIGH:
        vq = cur_vq(dev);
        if (vq != NULL)
            vq->used_gpa = (vq->used_gpa & 0x00000000FFFFFFFFULL) | ((u64)val << 32);
        break;
    case VIRTIO_MMIO_QUEUE_NOTIFY:
        if (val < VIRTIO_MMIO_NUM_QUEUES && dev->vq[val].ready) {
            if (dev->backend->queue_notify(dev, (u16)val))
                virtio_mmio_signal(dev);
        }
        break;
    case VIRTIO_MMIO_INTERRUPT_ACK:
        dev->interrupt_status &= ~val;
        break;
    case VIRTIO_MMIO_STATUS:
        if (val == 0U) {
            /* Device reset. */
            dev->status           = 0;
            dev->interrupt_status = 0;
            dev->driver_features  = 0;
            for (u32 i = 0; i < VIRTIO_MMIO_NUM_QUEUES; i++)
                virtqueue_reset(&dev->vq[i]);
        } else {
            dev->status = val;
        }
        break;
    default:
        break;   /* unknown / config space: ignore */
    }
}

int virtio_mmio_handler(struct mmio_access *acc, void *ctx)
{
    struct virtio_mmio_dev *dev = (struct virtio_mmio_dev *)ctx;

    /* All transport registers are 32-bit. Ignore other widths cleanly. */
    if (acc->size != 4U) {
        if (!acc->is_write)
            acc->data = 0;
        return 0;
    }

    if (acc->is_write)
        virtio_mmio_write(dev, acc->offset, (u32)acc->data);
    else
        acc->data = virtio_mmio_read(dev, acc->offset);

    return 0;
}
