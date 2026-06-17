/* SPDX-License-Identifier: TBD */
#ifndef HV_DM_VIRTQUEUE_H
#define HV_DM_VIRTQUEUE_H

#include <types.h>

#define VIRTQ_MAX_SIZE      64U   /* QueueNumMax we advertise */
#define VIRTQ_DESC_F_NEXT   0x1U  /* descriptor chains to .next */
#define VIRTQ_DESC_F_WRITE  0x2U  /* device-writable buffer (an RX buffer) */

/* Split-ring guest structures (modern virtio). All resident in guest RAM. */
struct virtq_desc {
    u64 addr;       /* guest-physical buffer address */
    u32 len;        /* buffer length in bytes        */
    u16 flags;      /* VIRTQ_DESC_F_*                 */
    u16 next;       /* next desc index if F_NEXT      */
};

struct virtq_avail {
    u16 flags;
    u16 idx;        /* driver's producer index           */
    u16 ring[VIRTQ_MAX_SIZE];   /* heads the driver published */
    /* u16 used_event; (we ignore event-idx) */
};

struct virtq_used_elem {
    u32 id;         /* descriptor head index   */
    u32 len;        /* bytes written by device */
};

struct virtq_used {
    u16 flags;
    u16 idx;        /* device's producer index */
    struct virtq_used_elem ring[VIRTQ_MAX_SIZE];
    /* u16 avail_event; (we ignore event-idx) */
};

/* Per-queue device-side state set up by the transport register writes. */
struct virtqueue {
    u64 desc_gpa;       /* QueueDesc{Low,High}   */
    u64 avail_gpa;      /* QueueDriver{Low,High} (available ring) */
    u64 used_gpa;       /* QueueDevice{Low,High} (used ring)      */
    u16 num;            /* QueueNum (driver-chosen size)          */
    bool ready;         /* QueueReady != 0                        */
    u16 last_avail_idx; /* device's consumer cursor               */
};

void virtqueue_reset(struct virtqueue *vq);

/*
 * Pop the next available descriptor-chain head, if any.
 *   *head_out  - descriptor index of the chain head
 * Returns true if a new chain was available (and consumes it from the avail
 * ring, advancing last_avail_idx), false if nothing new is pending.
 */
bool virtqueue_pop(struct virtqueue *vq, u16 *head_out);

/*
 * Resolve a descriptor index to its fields via gpa_to_hva. Returns true and
 * fills *out on success; false if the index is out of range or the descriptor
 * memory is unmappable.
 */
bool virtqueue_read_desc(struct virtqueue *vq, u16 idx, struct virtq_desc *out);

/*
 * Publish a completed chain: write {head, written} into the used ring and
 * increment used->idx. No-op if the used ring is unmappable.
 */
void virtqueue_push(struct virtqueue *vq, u16 head, u32 written);

#endif /* HV_DM_VIRTQUEUE_H */
