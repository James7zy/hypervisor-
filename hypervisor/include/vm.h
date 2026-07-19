/* SPDX-License-Identifier: TBD */
#ifndef HV_VM_H
#define HV_VM_H

#ifndef __ASSEMBLER__
#include <types.h>
#include <board.h>
#include <percpu.h>   /* NR_CPUS (percpu.h forward-declares struct vcpu) */

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
};

struct vm_config;

struct vm {
    struct vcpu            vcpu[VCPUS_PER_VM];
    u32                    id;        /* index into vm[]; keys the per-VM
                                         file-static state in stage2.c /
                                         vgic_v3_mmio.c */
    const struct vm_config *config;
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
