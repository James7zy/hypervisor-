/* SPDX-License-Identifier: TBD */
#ifndef HV_VM_H
#define HV_VM_H

#ifndef __ASSEMBLER__
#include <types.h>
#include <spinlock.h>
#include <board.h>
#include <percpu.h>   /* NR_CPUS, for the NR_VMS/VCPUS_PER_VM static_assert
                         below (percpu.h forward-declares struct vcpu) */
#include <vuart.h>    /* struct vuart, embedded by value in struct vm below */

struct vcpu_regs {
    u64 x[31];      /* x0–x30   offset 0x000 */
    u64 sp_el1;     /*           offset 0x0F8 */
    u64 elr_el2;    /*           offset 0x100 */
    u64 spsr_el2;   /*           offset 0x108 */
};

/* Compile-time VM count. 1 until M5 slice 3; the Makefile overrides it
 * per guest profile (-DCONFIG_NR_VMS=N). */
#ifndef CONFIG_NR_VMS
#define CONFIG_NR_VMS 1
#endif
#define NR_VMS       CONFIG_NR_VMS
#define VCPUS_PER_VM 2   /* static 2-vCPU VMs; NR_CPUS = NR_VMS * VCPUS_PER_VM */

/*
 * secondary_main derives a woken pCPU's (VM, vCPU) purely from its pCPU id
 * (id / VCPUS_PER_VM, id % VCPUS_PER_VM); vm_configs[]/pcpu_base must agree
 * with that structural mapping (see vm_init's boot-time check in vm.c). This
 * only makes sense if every VM's static pCPU slot fits within the physical
 * machine, i.e. NR_VMS * VCPUS_PER_VM <= NR_CPUS -- catch a mismatch (e.g. a
 * future CONFIG_NR_VMS bump without a matching NR_CPUS bump) at compile time
 * instead of silently indexing percpu[]/sgi_pending[] out of bounds.
 *
 * NR_CPUS (percpu.h) is a fixed PLATFORM constant sized for the largest build
 * profile (4, since M5 slice 3: -smp 4 in run-qemu.sh, 2 VMs x 2 vCPUs). It is
 * intentionally NOT required to equal NR_VMS * VCPUS_PER_VM: single-VM build
 * profiles (HV_GUEST=svm/svm2/svm3, NR_VMS=1, the pre-slice-3 SVM regression
 * tests) only ever PSCI CPU_ON pCPUs 0/1 -- pCPUs 2/3 are simply never woken
 * under those profiles, so under-using the physical core budget is safe; the
 * dangerous direction (a VM's slot overrunning NR_CPUS) is what this guards.
 */
_Static_assert(NR_VMS * VCPUS_PER_VM <= NR_CPUS,
               "NR_VMS * VCPUS_PER_VM must fit within NR_CPUS");

struct vm;

struct vcpu {
    struct vcpu_regs regs;   /* MUST be first */
    u64 hcr_el2;             /* offset 0x110 */
    u64 vttbr_el2;           /* offset 0x118 */
    u64 ich_hcr_el2;         /* offset 0x120 */
    u64 ich_vmcr_el2;        /* offset 0x128 */
    u64 ich_lr[4];           /* offset 0x130 (LR0..LR3, 0x130..0x14F) */
    /* Fields below are NOT read by the exception-entry asm — append only. */
    struct vm *owner;        /* back-pointer: trap handlers navigate via
                                current_vcpu()->owner instead of globals */
    u32        vcpu_idx;     /* affinity inside the VM (VMPIDR Aff0) */
    /* M5 slice 3 fix: set by remote vgic_inject_spi() before kicking the
     * owning pCPU, test-and-cleared by vgic_reload_spi_lr() on that pCPU.
     * Distinguishes "this kick-SGI carries a freshly-shadowed PL011 SPI" from
     * "this kick-SGI is an ordinary cross-core IPI/park-check with nothing
     * new in ich_lr[1]" -- without it, vgic_reload_spi_lr() would blindly
     * replay a stale shadow (already consumed by the guest) back into the
     * live ICH_LR1_EL2 on every unrelated kick. Mirrors sgi_pending[]'s role
     * for the SGI/IPI case (vgic_sgi.c) but is per-vcpu, separate state --
     * does not interact with sgi_pending[]. */
    bool spi_shadow_pending;
    /* Serializes LR1 payload/pending/live completion; never reset at runtime. */
    struct spinlock spi_lock;
};

struct vm_config;

struct vm {
    struct vcpu            vcpu[VCPUS_PER_VM];
    u32                    id;        /* index into vm[]; keys the per-VM
                                         file-static state in stage2.c /
                                         vgic_v3_mmio.c */
    const struct vm_config *config;
    struct vuart            vuart;    /* emulated PL011; see hypervisor/dm/vuart.c */
    /* M5 slice 3: set by psci_power_down() when this VM calls CPU_OFF/
     * SYSTEM_OFF/SYSTEM_RESET. Not read by any asm path (struct vm has no
     * __ASSEMBLER__ offset macros, unlike struct vcpu/struct hv_ctx below) —
     * only the kick-SGI branch of el2_irq_handler polls it, to park this VM's
     * other pCPU(s) instead of re-entering their guest. */
    volatile u32            off;
};

struct hv_ctx {
    u64 x19, x20, x21, x22, x23, x24, x25, x26, x27, x28, x29;
    u64 lr;   /* offset 0x058 */
    u64 sp;   /* offset 0x060 */
};

extern struct vm vm[NR_VMS];

extern void vcpu_run(struct vcpu *vcpu);
extern void hv_restore(void);

void vm_init(void);
void vm_run(void);
#endif /* !__ASSEMBLER__ */

#ifdef __ASSEMBLER__
#define VCPU_X0         0x000
#define VCPU_SP_EL1     0x0F8
#define VCPU_ELR        0x100
#define VCPU_SPSR       0x108
#define VCPU_HCR_EL2    0x110
#define VCPU_VTTBR_EL2  0x118
#define HV_LR           0x058
#define HV_SP           0x060
#define HV_CTX_SIZE     0x068
#endif /* __ASSEMBLER__ */

#endif /* HV_VM_H */
