/* SPDX-License-Identifier: TBD */
#ifndef HV_SPINLOCK_H
#define HV_SPINLOCK_H

#include <types.h>

/* Arch hooks, defined (static inline) in <arch/spinlock.h>. */
static inline u32 atomic_arch_fetch_inc_u32(u32 *p);   /* return (*p)++ atomically */
static inline void cpu_arch_relax(void);               /* spin-wait hint */
#include <arch/spinlock.h>

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
    /* Atomically take the next ticket: my = next++. */
    u32 my = atomic_arch_fetch_inc_u32(&lock->next);

    /* Busy-wait until our ticket is served. Plain spin (no WFE/SEV): critical
     * sections are tiny and there are at most 4 cores, so a simple, provably
     * correct spin beats the WFE/event-register race. */
    while (__atomic_load_n(&lock->owner, __ATOMIC_ACQUIRE) != my)
        cpu_arch_relax();
}

static inline void spin_unlock(struct spinlock *lock)
{
    /* Release: publish the owner advance with release ordering so the critical
     * section's writes are visible to the next holder. */
    __atomic_store_n(&lock->owner, lock->owner + 1U, __ATOMIC_RELEASE);
}

#endif /* HV_SPINLOCK_H */
