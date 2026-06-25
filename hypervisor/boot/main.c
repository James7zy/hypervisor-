/* SPDX-License-Identifier: TBD */
#include <types.h>
#include <uart.h>
#include <printk.h>
#include <board.h>
#include <vm.h>
#include <percpu.h>
#include <psci.h>
#include <gic_v3.h>
#include <vtimer.h>

extern u64 read_currentel(void);
extern void cpu_wfi(void);

/* secondary_entry (head.S): EL2 PA the secondary core is powered on at. */
extern char secondary_entry[];

/*
 * Slice-2 bring-up probe (TEMPORARY): power on pCPU1 and wait for it to
 * publish online. This proves the secondary EL2 bring-up path before any
 * guest drives it; Slice 4 moves the CPU_ON into the guest PSCI handler and
 * deletes this.
 */
static void smp_bringup_probe(void)
{
    /*
     * QEMU matches CPU_ON's target against arm_cpu_mp_affinity() (the *affinity*
     * fields, not the raw MPIDR_EL1 read). For virt+GICv3, mp_affinity for cpu
     * index N is arm_build_mp_affinity(N, 16) = N for N<16 — i.e. pCPU1 == 0x1,
     * with NO bit31 (the RES1 bit that MPIDR_EL1 reads back as 0x80000000 is
     * not part of the affinity). So the target is the plain affinity 0x1.
     */
    s64 ret = psci_cpu_on(0x1ULL, (u64)(uintptr_t)secondary_entry, 1ULL);
    if (ret != (s64)PSCI_RET_SUCCESS) {
        printk("[hv] CPU0: physical CPU_ON(pCPU1) failed, ret=%d\n", (int)ret);
        return;
    }

    /* Spin on the online handshake with a bounded timeout. */
    for (u64 i = 0; i < 100000000ULL; i++) {
        if (percpu[1].online) {
            printk("[hv] CPU0: pCPU1 handshake OK (online)\n");
            return;
        }
        asm volatile("dmb ish" ::: "memory");
    }
    printk("[hv] CPU0: pCPU1 handshake TIMEOUT (online never set)\n");
}

void hypervisor_main(uintptr_t dtb_phys)
{
    (void)dtb_phys;

    uart_init(BOARD_UART_BASE);

    printk("\n");
    printk("  H   H Y   Y PPPP  EEEEE RRRR  V   V IIIII SSSSS  OOO  RRRR  \n");
    printk("  H   H  Y Y  P   P E     R   R V   V   I   S     O   O R   R  \n");
    printk("  HHHHH   Y   PPPP  EEE   RRRR  V   V   I   SSSSS O   O RRRR   \n");
    printk("  H   H   Y   P     E     R R    V V    I       S  O   O R R    \n");
    printk("  H   H   Y   P     EEEEE R  R    V   IIIII SSSSS   OOO  R  R   \n");
    printk("\n");

    u64 el = read_currentel();
    printk("[hv] Hello from EL2 on %s, CurrentEL=0x%lx\n", board_name, el);

    gic_init();
    vtimer_init();

    smp_bringup_probe();   /* TEMPORARY (Slice 2): prove pCPU1 EL2 bring-up */

    vm_init();
    vm_run();

    for (;;)
        cpu_wfi();
}
