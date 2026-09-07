/* SPDX-License-Identifier: TBD */
#ifndef HV_SPINLOCK_H
#define HV_SPINLOCK_H

#include <types.h>

/*
 * Minimal SMP spinlock (M3.5). The repo had zero synchronization primitives
 * through M3.4 (a single core never needed any); this is the first. Kept
 * deliberately small: a ticket lock with the required acquire/release barriers.
 * Protects printk, SGI pending bitmaps, vuart device state, per-VM GICD/GICR
 * MMIO shadows, and per-vCPU SPI publication/consumption. Stage-2 remains
 * build-once-read-only; timer/SGI live registers remain target-PE-owned.
 * Locks do NOT mask interrupts. Current callers run with EL2 IRQs masked
 * (boot or exception entry), and must not unmask while holding a lock.
 * No device/SPI/MMIO lock is held across console output, kicks or guest waits.
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
    u32 my, tmp, ok;

    /* Atomically take the next ticket: my = next++; (LDXR/STXR retry loop). */
    __asm__ volatile(
        "1: ldxr    %w0, [%3]\n"        /* my = next                  */
        "   add     %w1, %w0, #1\n"     /* tmp = my + 1               */
        "   stxr    %w2, %w1, [%3]\n"   /* try next = tmp; ok==0 wins */
        "   cbnz    %w2, 1b\n"
        : "=&r"(my), "=&r"(tmp), "=&r"(ok)
        : "r"(&lock->next)
        : "memory");

    /* Busy-wait until our ticket is served. Plain spin (no WFE/SEV): critical
     * sections are tiny and there are at most 4 cores, so a simple, provably
     * correct spin beats the WFE/event-register race. */
    while (__atomic_load_n(&lock->owner, __ATOMIC_ACQUIRE) != my)
        __asm__ volatile("yield");
}

static inline void spin_unlock(struct spinlock *lock)
{
    /* Release: publish the owner advance with release ordering so the critical
     * section's writes are visible to the next holder. */
    __atomic_store_n(&lock->owner, lock->owner + 1U, __ATOMIC_RELEASE);
}

#endif /* HV_SPINLOCK_H */
