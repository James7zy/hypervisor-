/* SPDX-License-Identifier: TBD */
#ifndef HV_SPINLOCK_H
#define HV_SPINLOCK_H

#include <types.h>

/*
 * Minimal SMP spinlock (M3.5). The repo had zero synchronization primitives
 * through M3.4 (a single core never needed any); this is the first. Kept
 * deliberately small: a ticket lock with the required acquire/release barriers.
 * Used only for the few truly-shared structures (printk/UART and the SGI
 * pending bitmap, later slices); everything else stays lock-free by being
 * build-once-read-only (Stage-2) or per-CPU (vcpu/vGIC/timer).
 *
 * Ticket lock: `next` hands out tickets, `owner` is the ticket currently
 * served. A waiter spins until owner == its ticket, giving FIFO fairness.
 */
struct spinlock {
    u32 owner;
    u32 next;
};

#define SPINLOCK_INIT { 0, 0 }

static inline void spin_lock(struct spinlock *lock)
{
    u32 my, cur, tmp, ok;

    /* Atomically take the next ticket: my = next++; (LDAXR/STXR retry loop). */
    __asm__ volatile(
        "1: ldaxr   %w0, [%3]\n"        /* my = next                  */
        "   add     %w1, %w0, #1\n"     /* tmp = my + 1               */
        "   stxr    %w2, %w1, [%3]\n"   /* try next = tmp; ok==0 wins */
        "   cbnz    %w2, 1b\n"
        : "=&r"(my), "=&r"(tmp), "=&r"(ok)
        : "r"(&lock->next)
        : "memory");

    /* Spin (with acquire ordering) until our ticket is the one being served. */
    do {
        __asm__ volatile("ldaxr %w0, [%1]"
                         : "=r"(cur) : "r"(&lock->owner) : "memory");
        if (cur == my)
            break;
        __asm__ volatile("wfe");
    } while (1);
}

static inline void spin_unlock(struct spinlock *lock)
{
    /* Release barrier, then advance owner so the next ticket is served. */
    __asm__ volatile("dmb ish" ::: "memory");
    lock->owner = lock->owner + 1U;
    __asm__ volatile("dsb ish" ::: "memory");
    __asm__ volatile("sev");   /* wake any WFE waiters */
}

#endif /* HV_SPINLOCK_H */
