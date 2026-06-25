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

struct vcpu {
    struct vcpu_regs regs;   /* MUST be first */
    u64 hcr_el2;             /* offset 0x110 */
    u64 vttbr_el2;           /* offset 0x118 */
    u64 ich_hcr_el2;         /* offset 0x120 */
    u64 ich_vmcr_el2;        /* offset 0x128 */
    u64 ich_lr[4];           /* offset 0x130 (LR0..LR3, 0x130..0x14F) */
};

struct vm_config;

struct vm {
    struct vcpu            vcpu[NR_CPUS];   /* M3.5: 2 vCPUs, static 1:1 pinned */
    const struct vm_config *config;
};

struct hv_ctx {
    u64 x19, x20, x21, x22, x23, x24, x25, x26, x27, x28, x29;
    u64 lr;   /* offset 0x058 */
    u64 sp;   /* offset 0x060 */
};

extern struct vm g_vm;

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
