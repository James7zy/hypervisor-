/* SPDX-License-Identifier: TBD */
#include <types.h>
#include "gpa.h"
#include "virtqueue.h"

void virtqueue_reset(struct virtqueue *vq)
{
    vq->desc_gpa       = 0;
    vq->avail_gpa      = 0;
    vq->used_gpa       = 0;
    vq->num            = 0;
    vq->ready          = false;
    vq->last_avail_idx = 0;
}

bool virtqueue_read_desc(struct virtqueue *vq, u16 idx, struct virtq_desc *out)
{
    if (vq->num == 0U || idx >= vq->num)
        return false;

    struct virtq_desc *table =
        (struct virtq_desc *)gpa_to_hva_len(vq->desc_gpa,
                                            (u64)vq->num * sizeof(struct virtq_desc));
    if (table == NULL)
        return false;

    *out = table[idx];

    /* The buffer the descriptor points at must also be mappable. */
    if (gpa_to_hva_len(out->addr, (u64)out->len) == NULL && out->len != 0U)
        return false;

    return true;
}

bool virtqueue_pop(struct virtqueue *vq, u16 *head_out)
{
    if (!vq->ready || vq->num == 0U)
        return false;

    struct virtq_avail *avail =
        (struct virtq_avail *)gpa_to_hva_len(vq->avail_gpa,
                                             sizeof(struct virtq_avail));
    if (avail == NULL)
        return false;

    /* Volatile read: the driver updates avail->idx asynchronously. */
    u16 avail_idx = *(volatile u16 *)&avail->idx;
    if (vq->last_avail_idx == avail_idx)
        return false;                       /* nothing new */

    u16 slot = (u16)(vq->last_avail_idx % vq->num);
    *head_out = avail->ring[slot];
    vq->last_avail_idx++;
    return true;
}

void virtqueue_push(struct virtqueue *vq, u16 head, u32 written)
{
    struct virtq_used *used =
        (struct virtq_used *)gpa_to_hva_len(vq->used_gpa,
                                            sizeof(struct virtq_used));
    if (used == NULL)
        return;

    u16 uidx = *(volatile u16 *)&used->idx;
    u16 slot = (u16)(uidx % vq->num);

    used->ring[slot].id  = (u32)head;
    used->ring[slot].len = written;

    /* Publish the entry before advancing idx so the driver sees a complete
     * element when it observes the new idx. */
    __asm__ volatile("dmb ish" ::: "memory");
    *(volatile u16 *)&used->idx = (u16)(uidx + 1U);
}
