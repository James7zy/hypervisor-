/* SPDX-License-Identifier: TBD */
#include <types.h>
#include <printk.h>
#include <vm.h>
#include <mmio.h>
#include "data_abort.h"

/* ESR_EL2 ISS fields for a Data Abort (EC = 0x24). */
#define ISS_ISV(iss)  (((iss) >> 24) & 0x1U)   /* Instruction Syndrome Valid */
#define ISS_SAS(iss)  (((iss) >> 22) & 0x3U)   /* Access size 00..11 -> 1/2/4/8 */
#define ISS_SRT(iss)  (((iss) >> 16) & 0x1FU)  /* Transfer register 0..31 */
#define ISS_WNR(iss)  (((iss) >> 6)  & 0x1U)   /* Write not Read */
#define ISS_DFSC(iss) ((iss) & 0x3FU)          /* Data Fault Status Code */

/* SAS (00,01,10,11) -> access size in bytes (1,2,4,8). */
static u8 mmio_sas_to_bytes(u32 sas)
{
    return (u8)(1U << sas);
}

/* Read FAR_EL2 / HPFAR_EL2 and reconstruct the faulting guest IPA. */
static u64 mmio_faulting_ipa(void)
{
    u64 far, hpfar;
    asm volatile("mrs %0, far_el2"   : "=r"(far));
    asm volatile("mrs %0, hpfar_el2" : "=r"(hpfar));
    /*
     * HPFAR_EL2[43:4] = IPA[47:12]. (hpfar & 0xFFFFFFFFFFF0) << 8 yields the
     * 4 KB-aligned IPA; merge the page offset from FAR_EL2[11:0].
     */
    return ((hpfar & 0xFFFFFFFFFFF0ULL) << 8) | (far & 0xFFFULL);
}

int mmio_handle_data_abort(struct arch_regs *regs, u64 esr)
{
    u32 iss = (u32)(esr & 0x01FFFFFFU);

    if (ISS_ISV(iss) == 0U) {
        /* No instruction syndrome: would require fetching+decoding the guest
         * instruction. Linux GIC/virtio accesses are simple loads/stores with
         * ISV=1, so we report this as unsupported for M3.x. */
        printk("[hv] MMIO: ISV=0 unsupported, ESR=0x%lx ELR=0x%lx\n",
               esr, regs->elr_el2);
        return -1;
    }

    u32 srt = ISS_SRT(iss);

    struct mmio_access acc;
    acc.addr     = mmio_faulting_ipa();
    acc.offset   = 0;   /* filled in by mmio_bus_dispatch */
    acc.size     = mmio_sas_to_bytes(ISS_SAS(iss));
    acc.is_write = (ISS_WNR(iss) != 0U);
    acc.data     = 0;

    if (acc.is_write) {
        /* Source GPR -> data. SRT=31 is the zero register. */
        acc.data = (srt == 31U) ? 0ULL : regs->x[srt];
    }

    /* No region, or the device failed: caller logs DFSC + parks. */
    if (mmio_bus_dispatch(&acc) != 0)
        return -1;

    if (!acc.is_write && srt != 31U) {
        /* Load: device result -> destination GPR. */
        regs->x[srt] = acc.data;
    }

    /* Advance past the faulting load/store; el1_sync_handler erets back. */
    regs->elr_el2 += 4;
    return 0;
}
