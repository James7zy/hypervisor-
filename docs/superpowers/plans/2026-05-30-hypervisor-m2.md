# Hypervisor M2 — Interrupts + PSCI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Enable the SVM to receive a virtual timer interrupt delivered via the vGICv3, and respond to PSCI queries, producing observable output from both sides.

**Architecture:** Physical GICv3 initialized by the hypervisor; EL2 IRQ handler saves guest state, injects a virtual IRQ via ICH_LR0_EL2, and erets back to the guest. The SVM configures its ICC_* interface, arms the virtual timer, waits for the interrupt, handles it, then signals done via HVC. PSCI is dispatched from `handle_hvc`. Multi-vCPU (PSCI CPU_ON + SMP) is deferred to M3.

**Tech Stack:** AArch64 GAS assembly, C11 freestanding, `aarch64-none-linux-gnu-gcc >= 10`, `qemu-system-aarch64 >= 6.0` (QEMU virt machine with GICv3).

**Baseline required:** Full M1 implementation (stage2, vmexit, vm.c). The master branch must include all M1 commits from the `m1-bare-metal-guest` worktree before starting M2.

**Project root:** `/home/corsair/Music/virtual/hyp-/hypervisor-/`

**Build:** `make defconfig && make` — must produce zero warnings (`-Werror`).

**Verification:** `tests/run_svm2_test.sh` — observe all ten expected output lines in order (see Task 10).

---

## File Map

| Action | Path | Purpose |
|---|---|---|
| Modify | `hypervisor/arch/arm64/board/qemu_virt/board.h` | Add GIC base addresses and vtimer INTID |
| Modify | `hypervisor/include/vm.h` | Extend `struct vcpu` with vGIC fields |
| Create | `hypervisor/common/psci/psci.h` | PSCI function IDs and return codes |
| Create | `hypervisor/common/psci/psci.c` | PSCI dispatcher: VERSION, FEATURES, CPU_OFF, SYSTEM_OFF |
| Create | `hypervisor/arch/arm64/irq/gic_v3.h` | GICv3 MMIO macros and API |
| Create | `hypervisor/arch/arm64/irq/gic_v3.c` | Physical GIC init: GICD, GICR, ICC_SRE_EL2, PPI 27 |
| Create | `hypervisor/arch/arm64/irq/vgic.h` | vGICv3 API: init, inject, save, restore |
| Create | `hypervisor/arch/arm64/irq/vgic.c` | ICH_HCR_EL2, ICH_VMCR_EL2, ICH_LR0..LR3 management |
| Create | `hypervisor/arch/arm64/irq/irq_handler.c` | EL2 IRQ C handler: ack -> inject -> EOI |
| Create | `hypervisor/arch/arm64/irq/irq_handler_asm.S` | `el1_irq_handler_asm`: save guest, call C handler, restore, eret |
| Create | `hypervisor/arch/arm64/timer/vtimer.h` | `vtimer_init()` declaration |
| Create | `hypervisor/arch/arm64/timer/vtimer.c` | CNTHCTL_EL2, CNTVOFF_EL2 configuration |
| Modify | `hypervisor/arch/arm64/vmexit/vmexit.c` | Wire PSCI: replace stub with `psci_handle(regs)` |
| Modify | `hypervisor/arch/arm64/vmexit/vmexit_asm.S` | Add `msr daifclr, #2` before guest eret |
| Modify | `hypervisor/arch/arm64/boot/vectors.S` | Wire Lower EL AArch64 IRQ (+0x480) to `el1_irq_handler_asm` |
| Modify | `hypervisor/boot/main.c` | Call `gic_init()`, `vtimer_init()` before `vm_run()` |
| Modify | `hypervisor/common/vm/vm.c` | Call `vgic_init()` in `vm_init()`; `vgic_restore()` in `vm_run()` |
| Modify | `hypervisor/arch/arm64/Makefile` | Add `irq/` and `timer/` objects; extend `arch-includes` |
| Modify | `hypervisor/Makefile` | Add `common/psci/psci.o`; extend `hv-includes` |
| Create | `tests/svm2/svm2_vectors.S` | EL1 vector table for the M2 SVM test payload |
| Create | `tests/svm2/svm2_main.c` | SVM: GIC init, virtual timer arm, wait, IRQ handler, HVC done |
| Create | `tests/svm2/svm2.lds` | Linker script for SVM2 (entry at 0x40200000) |
| Create | `tests/run_svm2_test.sh` | Build SVM2 binary and invoke `make run` |

---

## Task 1: Board constants + PSCI header

**Files:**
- Modify: `hypervisor/arch/arm64/board/qemu_virt/board.h`
- Create: `hypervisor/common/psci/psci.h`

- [ ] **Step 1: Add GIC and vtimer constants to `board.h`**

Append after `BOARD_SVM_MEM_SIZE`:

```c
/* M2: GIC v3 base addresses for QEMU virt machine */
#define BOARD_GIC_DIST_BASE  0x08000000UL   /* GICD */
#define BOARD_GIC_RDIST_BASE 0x080A0000UL   /* GICR CPU0 RD frame */
#define BOARD_VTIMER_IRQ     27U             /* EL1 virtual timer PPI INTID */
```

- [ ] **Step 2: Create `hypervisor/common/psci/psci.h`**

```c
/* SPDX-License-Identifier: TBD */
#ifndef HV_PSCI_H
#define HV_PSCI_H

#include <vm.h>

/* PSCI function IDs (DEN0022D) */
#define PSCI_FUNC_VERSION      0x84000000U
#define PSCI_FUNC_CPU_OFF      0x84000002U
#define PSCI_FUNC_CPU_ON_32    0x84000003U
#define PSCI_FUNC_CPU_ON_64    0xC4000003U
#define PSCI_FUNC_FEATURES     0x8400000AU
#define PSCI_FUNC_SYSTEM_OFF   0x84000008U
#define PSCI_FUNC_SYSTEM_RESET 0x84000009U

/* Return codes */
#define PSCI_RET_SUCCESS        0
#define PSCI_RET_NOT_SUPPORTED  (-1)
#define PSCI_RET_INVALID_PARAMS (-2)
#define PSCI_RET_DENIED         (-3)

#define PSCI_VERSION_1_1       0x00010001U

void psci_handle(struct vcpu_regs *regs);

#endif /* HV_PSCI_H */
```

- [ ] **Step 3: Build check**

```bash
cd /home/corsair/Music/virtual/hyp-/hypervisor-
make
```

Expected: zero warnings, `build/hypervisor.elf` present.

- [ ] **Step 4: Commit**

```bash
git add hypervisor/arch/arm64/board/qemu_virt/board.h \
        hypervisor/common/psci/psci.h
git commit -m "feat(m2): add GIC board constants and PSCI header"
```

---

## Task 2: PSCI dispatcher (`psci.c`)

**Files:**
- Create: `hypervisor/common/psci/psci.c`

- [ ] **Step 1: Create `hypervisor/common/psci/psci.c`**

```c
/* SPDX-License-Identifier: TBD */
#include <types.h>
#include <printk.h>
#include <vm.h>
#include "psci.h"

void psci_handle(struct vcpu_regs *regs)
{
    u32 func_id = (u32)regs->x[0];

    switch (func_id) {
    case PSCI_FUNC_VERSION:
        regs->x[0] = PSCI_VERSION_1_1;
        break;

    case PSCI_FUNC_FEATURES:
        switch ((u32)regs->x[1]) {
        case PSCI_FUNC_VERSION:
        case PSCI_FUNC_CPU_OFF:
        case PSCI_FUNC_FEATURES:
        case PSCI_FUNC_SYSTEM_OFF:
            regs->x[0] = (u64)(s64)PSCI_RET_SUCCESS;
            break;
        default:
            regs->x[0] = (u64)(s64)PSCI_RET_NOT_SUPPORTED;
            break;
        }
        break;

    case PSCI_FUNC_CPU_OFF:
        printk("[hv] PSCI CPU_OFF -- halting vCPU\n");
        for (;;)
            asm volatile("wfi");

    case PSCI_FUNC_SYSTEM_OFF:
        printk("[hv] PSCI SYSTEM_OFF -- halting\n");
        for (;;)
            asm volatile("wfi");

    case PSCI_FUNC_SYSTEM_RESET:
        printk("[hv] PSCI SYSTEM_RESET -- halting\n");
        for (;;)
            asm volatile("wfi");

    case PSCI_FUNC_CPU_ON_32:
    case PSCI_FUNC_CPU_ON_64:
        /* M3: SMP bring-up */
        regs->x[0] = (u64)(s64)PSCI_RET_NOT_SUPPORTED;
        break;

    default:
        regs->x[0] = (u64)(s64)PSCI_RET_NOT_SUPPORTED;
        break;
    }
}
```

- [ ] **Step 2: Compile check**

```bash
aarch64-none-linux-gnu-gcc \
  -ffreestanding -nostdlib -mgeneral-regs-only -mstrict-align \
  -Wall -Wextra -Werror -O2 -g \
  -Ihypervisor/include \
  -Ihypervisor/common/vm \
  -Ihypervisor/common/psci \
  -Ihypervisor/arch/arm64/board/qemu_virt \
  -Ihypervisor/arch/arm64/include \
  -c hypervisor/common/psci/psci.c -o /tmp/psci_check.o
```

Expected: zero warnings.

- [ ] **Step 3: Commit**

```bash
git add hypervisor/common/psci/psci.c
git commit -m "feat(m2): add PSCI dispatcher (psci.c)"
```

---

## Task 3: Physical GICv3 header + sysreg aliases

**Files:**
- Modify: `hypervisor/arch/arm64/include/asm/sysreg.h`
- Create: `hypervisor/arch/arm64/irq/gic_v3.h`

- [ ] **Step 1: Add GIC and vGIC system register aliases to `sysreg.h`**

Append before the final `#endif`:

```c
/*
 * GICv3 CPU interface system registers.
 * S<op0>_<op1>_<Cn>_<Cm>_<op2> encoding works with any binutils version.
 */
#define ICC_SRE_EL2      S3_4_C12_C9_5
#define ICC_PMR_EL1      S3_0_C4_C6_0
#define ICC_IGRPEN1_EL1  S3_0_C12_C12_7
#define ICC_IAR1_EL1     S3_0_C12_C12_0
#define ICC_EOIR1_EL1    S3_0_C12_C12_1
#define ICC_DIR_EL1      S3_0_C12_C11_1
#define ICC_SRE_EL1      S3_0_C12_C12_5

/* vGIC virtual CPU interface registers (EL2 hypervisor view) */
#define ICH_HCR_EL2      S3_4_C12_C11_0
#define ICH_VTR_EL2      S3_4_C12_C11_1
#define ICH_VMCR_EL2     S3_4_C12_C11_7
#define ICH_LR0_EL2      S3_4_C12_C12_0
#define ICH_LR1_EL2      S3_4_C12_C12_1
#define ICH_LR2_EL2      S3_4_C12_C12_2
#define ICH_LR3_EL2      S3_4_C12_C12_3

/* Generic timer */
#define CNTHCTL_EL2      S3_4_C14_C1_0
#define CNTVOFF_EL2      S3_4_C14_C0_3
```

- [ ] **Step 2: Create `hypervisor/arch/arm64/irq/gic_v3.h`**

```c
/* SPDX-License-Identifier: TBD */
#ifndef ARCH_GIC_V3_H
#define ARCH_GIC_V3_H

#include <types.h>
#include <board.h>

/* ---- Distributor (GICD) ------------------------------------------------ */
#define GICD_CTLR         (*(volatile u32 *)(BOARD_GIC_DIST_BASE + 0x000U))
#define GICD_TYPER        (*(volatile u32 *)(BOARD_GIC_DIST_BASE + 0x004U))
#define GICD_ISENABLER(n) (*(volatile u32 *)(BOARD_GIC_DIST_BASE + 0x100U + (u32)(n) * 4U))

#define GICD_CTLR_ENGRP1NS (1U << 1)
#define GICD_CTLR_ARE_NS   (1U << 4)
#define GICD_CTLR_RWP      (1U << 31)

/* ---- Redistributor (GICR) -- CPU0 ------------------------------------- */
/*
 * Each redistributor has two 64 KB frames:
 *   RD frame:  BOARD_GIC_RDIST_BASE + 0x00000
 *   SGI frame: BOARD_GIC_RDIST_BASE + 0x10000
 */
#define GICR_RD_BASE    ((uintptr_t)BOARD_GIC_RDIST_BASE)
#define GICR_SGI_BASE   ((uintptr_t)BOARD_GIC_RDIST_BASE + 0x10000U)

#define GICR_WAKER      (*(volatile u32 *)(GICR_RD_BASE  + 0x014U))
#define GICR_IGROUPR0   (*(volatile u32 *)(GICR_SGI_BASE + 0x080U))
#define GICR_IGRPMODR0  (*(volatile u32 *)(GICR_SGI_BASE + 0xD00U))
#define GICR_ISENABLER0 (*(volatile u32 *)(GICR_SGI_BASE + 0x100U))
#define GICR_IPRIORITYR(n) (*(volatile u32 *)(GICR_SGI_BASE + 0x400U + (u32)(n) * 4U))

#define GICR_WAKER_PS   (1U << 1)
#define GICR_WAKER_CA   (1U << 2)

/* ---- API --------------------------------------------------------------- */
void gic_init(void);
u32  gic_ack_irq(void);
void gic_eoi_irq(u32 intid);

#endif /* ARCH_GIC_V3_H */
```

- [ ] **Step 3: Build check**

```bash
make
```

Expected: zero warnings (header-only changes).

- [ ] **Step 4: Commit**

```bash
git add hypervisor/arch/arm64/include/asm/sysreg.h \
        hypervisor/arch/arm64/irq/gic_v3.h
git commit -m "feat(m2): add GICv3 sysreg aliases and MMIO header"
```

---

## Task 4: Physical GICv3 init (`gic_v3.c`)

**Files:**
- Create: `hypervisor/arch/arm64/irq/gic_v3.c`

- [ ] **Step 1: Create `hypervisor/arch/arm64/irq/gic_v3.c`**

```c
/* SPDX-License-Identifier: TBD */
#include <types.h>
#include <printk.h>
#include <asm/sysreg.h>
#include "gic_v3.h"

static void gicd_wait_rwp(void)
{
    while (GICD_CTLR & GICD_CTLR_RWP)
        asm volatile("nop");
}

void gic_init(void)
{
    u32 word_idx, byte_off, shift, prio_reg;

    /* 1. Enable affinity routing and Group 1 NS at distributor */
    GICD_CTLR = GICD_CTLR_ARE_NS | GICD_CTLR_ENGRP1NS;
    gicd_wait_rwp();

    /* 2. Wake CPU0 redistributor: clear ProcessorSleep, wait for ChildrenAsleep=0 */
    GICR_WAKER &= ~GICR_WAKER_PS;
    while (GICR_WAKER & GICR_WAKER_CA)
        asm volatile("nop");

    /* 3. Configure PPI 27 (EL1 virtual timer): Group 1 NS, priority 0xA0 */
    GICR_IGROUPR0  |=  (1U << BOARD_VTIMER_IRQ);   /* Group 1 */
    GICR_IGRPMODR0 &= ~(1U << BOARD_VTIMER_IRQ);   /* Non-Secure */

    /* IPRIORITYR: one byte per INTID, word-addressed */
    word_idx = BOARD_VTIMER_IRQ / 4U;   /* 27/4 = 6 */
    byte_off = BOARD_VTIMER_IRQ % 4U;   /* 27%4 = 3 */
    shift    = byte_off * 8U;           /* bit offset 24 */
    prio_reg = GICR_IPRIORITYR(word_idx);
    prio_reg = (prio_reg & ~(0xFFU << shift)) | (0xA0U << shift);
    GICR_IPRIORITYR(word_idx) = prio_reg;

    /* Enable INTID 27 in redistributor */
    GICR_ISENABLER0 = (1U << BOARD_VTIMER_IRQ);

    /* 4. Enable GIC CPU interface system registers at EL2 */
    SYSREG_WRITE(ICC_SRE_EL2, 0xFULL);   /* SRE=1 | DIL=1 | DFB=1 | Enable=1 */
    asm volatile("isb");

    /* 5. Set EL2 priority mask and enable Group 1 (needed to ack/EOI IRQs at EL2) */
    SYSREG_WRITE(ICC_PMR_EL1,     0xFFULL);
    SYSREG_WRITE(ICC_IGRPEN1_EL1, 1ULL);
    asm volatile("isb");

    printk("[hv] GIC: initialized (dist=0x%lx rdist=0x%lx PPI=%u)\n",
           (u64)BOARD_GIC_DIST_BASE, (u64)BOARD_GIC_RDIST_BASE,
           (unsigned)BOARD_VTIMER_IRQ);
}

u32 gic_ack_irq(void)
{
    return (u32)(SYSREG_READ(ICC_IAR1_EL1) & 0x00FFFFFFU);
}

void gic_eoi_irq(u32 intid)
{
    SYSREG_WRITE(ICC_EOIR1_EL1, (u64)intid);   /* priority drop */
    SYSREG_WRITE(ICC_DIR_EL1,   (u64)intid);   /* deactivate */
    asm volatile("isb");
}
```

- [ ] **Step 2: Compile check**

```bash
aarch64-none-linux-gnu-gcc \
  -ffreestanding -nostdlib -mgeneral-regs-only -mstrict-align \
  -Wall -Wextra -Werror -O2 -g \
  -Ihypervisor/include \
  -Ihypervisor/arch/arm64/board/qemu_virt \
  -Ihypervisor/arch/arm64/include \
  -Ihypervisor/arch/arm64/irq \
  -c hypervisor/arch/arm64/irq/gic_v3.c -o /tmp/gic_v3_check.o
```

Expected: zero warnings.

- [ ] **Step 3: Commit**

```bash
git add hypervisor/arch/arm64/irq/gic_v3.c
git commit -m "feat(m2): add physical GICv3 init (gic_v3.c)"
```

---

## Task 5: Extend `struct vcpu` + vGICv3 (`vgic.h` + `vgic.c`)

**Files:**
- Modify: `hypervisor/include/vm.h`
- Create: `hypervisor/arch/arm64/irq/vgic.h`
- Create: `hypervisor/arch/arm64/irq/vgic.c`

- [ ] **Step 1: Extend `struct vcpu` in `hypervisor/include/vm.h`**

Replace the `struct vcpu` definition. New fields are appended after `vttbr_el2`; the assembly offsets `VCPU_HCR_EL2=0x110` and `VCPU_VTTBR_EL2=0x118` remain unchanged.

```c
struct vcpu {
    struct vcpu_regs regs;      /* MUST be first: assembly ABI */
    u64 hcr_el2;                /* offset 0x110 */
    u64 vttbr_el2;              /* offset 0x118 */
    /* vGICv3 virtual CPU interface state (M2) */
    u64 ich_hcr_el2;            /* offset 0x120 */
    u64 ich_vmcr_el2;           /* offset 0x128 */
    u64 ich_lr[4];              /* offset 0x130: ICH_LR0..LR3 */
};
```

- [ ] **Step 2: Create `hypervisor/arch/arm64/irq/vgic.h`**

```c
/* SPDX-License-Identifier: TBD */
#ifndef ARCH_VGIC_H
#define ARCH_VGIC_H

#include <types.h>
#include <vm.h>

/*
 * ICH_LR*_EL2 format (GICv3 Architecture Supplement section 8.4.7):
 *   [63:62] State:  00=Invalid, 01=Pending, 10=Active, 11=Pend+Active
 *   [61]    HW:     0 = software-generated
 *   [60]    Group:  1 = Group 1
 *   [55:48] Priority
 *   [31:0]  vINTID
 */
#define VGIC_LR_PENDING  (1ULL << 62)
#define VGIC_LR_GROUP1   (1ULL << 60)

void vgic_init(struct vcpu *vcpu);
void vgic_inject(struct vcpu *vcpu, u32 intid, u8 prio);
void vgic_save(struct vcpu *vcpu);
void vgic_restore(const struct vcpu *vcpu);

#endif /* ARCH_VGIC_H */
```

- [ ] **Step 3: Create `hypervisor/arch/arm64/irq/vgic.c`**

```c
/* SPDX-License-Identifier: TBD */
#include <types.h>
#include <asm/sysreg.h>
#include "vgic.h"

void vgic_init(struct vcpu *vcpu)
{
    int i;

    vcpu->ich_hcr_el2  = 1ULL;  /* ICH_HCR_EL2.En=1: enable virtual CPU interface */
    vcpu->ich_vmcr_el2 = 0ULL;  /* guest configures VPMR/VENG1 via ICC_* writes */
    for (i = 0; i < 4; i++)
        vcpu->ich_lr[i] = 0ULL;

    SYSREG_WRITE(ICH_HCR_EL2,  vcpu->ich_hcr_el2);
    SYSREG_WRITE(ICH_VMCR_EL2, vcpu->ich_vmcr_el2);
    SYSREG_WRITE(ICH_LR0_EL2,  0ULL);
    SYSREG_WRITE(ICH_LR1_EL2,  0ULL);
    SYSREG_WRITE(ICH_LR2_EL2,  0ULL);
    SYSREG_WRITE(ICH_LR3_EL2,  0ULL);
    asm volatile("isb");
}

void vgic_inject(struct vcpu *vcpu, u32 intid, u8 prio)
{
    u64 lr = VGIC_LR_PENDING
           | VGIC_LR_GROUP1
           | ((u64)prio << 48)
           | (u64)intid;

    vcpu->ich_lr[0] = lr;
    SYSREG_WRITE(ICH_LR0_EL2, lr);
    /* No isb: the eret in the caller will synchronize vIRQ delivery */
}

void vgic_save(struct vcpu *vcpu)
{
    vcpu->ich_hcr_el2  = SYSREG_READ(ICH_HCR_EL2);
    vcpu->ich_vmcr_el2 = SYSREG_READ(ICH_VMCR_EL2);
    vcpu->ich_lr[0]    = SYSREG_READ(ICH_LR0_EL2);
    vcpu->ich_lr[1]    = SYSREG_READ(ICH_LR1_EL2);
    vcpu->ich_lr[2]    = SYSREG_READ(ICH_LR2_EL2);
    vcpu->ich_lr[3]    = SYSREG_READ(ICH_LR3_EL2);
}

void vgic_restore(const struct vcpu *vcpu)
{
    SYSREG_WRITE(ICH_HCR_EL2,  vcpu->ich_hcr_el2);
    SYSREG_WRITE(ICH_VMCR_EL2, vcpu->ich_vmcr_el2);
    SYSREG_WRITE(ICH_LR0_EL2,  vcpu->ich_lr[0]);
    SYSREG_WRITE(ICH_LR1_EL2,  vcpu->ich_lr[1]);
    SYSREG_WRITE(ICH_LR2_EL2,  vcpu->ich_lr[2]);
    SYSREG_WRITE(ICH_LR3_EL2,  vcpu->ich_lr[3]);
    asm volatile("isb");
}
```

- [ ] **Step 4: Compile check**

```bash
aarch64-none-linux-gnu-gcc \
  -ffreestanding -nostdlib -mgeneral-regs-only -mstrict-align \
  -Wall -Wextra -Werror -O2 -g \
  -Ihypervisor/include \
  -Ihypervisor/arch/arm64/board/qemu_virt \
  -Ihypervisor/arch/arm64/include \
  -Ihypervisor/arch/arm64/irq \
  -c hypervisor/arch/arm64/irq/vgic.c -o /tmp/vgic_check.o
```

Expected: zero warnings.

- [ ] **Step 5: Commit**

```bash
git add hypervisor/include/vm.h \
        hypervisor/arch/arm64/irq/vgic.h \
        hypervisor/arch/arm64/irq/vgic.c
git commit -m "feat(m2): extend struct vcpu with vGIC state; add vgic.c"
```

---

## Task 6: Virtual timer init (`vtimer.h` + `vtimer.c`)

**Files:**
- Create: `hypervisor/arch/arm64/timer/vtimer.h`
- Create: `hypervisor/arch/arm64/timer/vtimer.c`

- [ ] **Step 1: Create `hypervisor/arch/arm64/timer/vtimer.h`**

```c
/* SPDX-License-Identifier: TBD */
#ifndef ARCH_VTIMER_H
#define ARCH_VTIMER_H

void vtimer_init(void);

#endif /* ARCH_VTIMER_H */
```

- [ ] **Step 2: Create `hypervisor/arch/arm64/timer/vtimer.c`**

```c
/* SPDX-License-Identifier: TBD */
#include <types.h>
#include <printk.h>
#include <asm/sysreg.h>
#include "vtimer.h"

void vtimer_init(void)
{
    /*
     * CNTHCTL_EL2[1:0]:
     *   EL1PCTEN[0]=1: allow EL1/EL0 to read CNTPCT_EL0 (physical counter)
     *   EL1PCEN[1]=1:  allow EL1/EL0 to access physical timer registers
     * Without this, SVM reads of CNTPCT_EL0 trap to EL2 as EC=0x18.
     */
    SYSREG_WRITE(CNTHCTL_EL2, 3ULL);

    /*
     * CNTVOFF_EL2 = 0: virtual count == physical count.
     * Guest CNTVCT_EL0 = CNTPCT_EL0 (no time skew).
     */
    SYSREG_WRITE(CNTVOFF_EL2, 0ULL);

    asm volatile("isb");

    printk("[hv] vtimer: CNTHCTL_EL2=0x3, CNTVOFF_EL2=0x0\n");
}
```

- [ ] **Step 3: Compile check**

```bash
aarch64-none-linux-gnu-gcc \
  -ffreestanding -nostdlib -mgeneral-regs-only -mstrict-align \
  -Wall -Wextra -Werror -O2 -g \
  -Ihypervisor/include \
  -Ihypervisor/arch/arm64/board/qemu_virt \
  -Ihypervisor/arch/arm64/include \
  -Ihypervisor/arch/arm64/timer \
  -c hypervisor/arch/arm64/timer/vtimer.c -o /tmp/vtimer_check.o
```

Expected: zero warnings.

- [ ] **Step 4: Commit**

```bash
git add hypervisor/arch/arm64/timer/vtimer.h \
        hypervisor/arch/arm64/timer/vtimer.c
git commit -m "feat(m2): add virtual timer init (vtimer.c)"
```

---

## Task 7: EL2 IRQ C handler + wire PSCI in `vmexit.c`

**Files:**
- Create: `hypervisor/arch/arm64/irq/irq_handler.c`
- Modify: `hypervisor/arch/arm64/vmexit/vmexit.c`

- [ ] **Step 1: Create `hypervisor/arch/arm64/irq/irq_handler.c`**

```c
/* SPDX-License-Identifier: TBD */
#include <types.h>
#include <printk.h>
#include <board.h>
#include <vm.h>
#include "gic_v3.h"
#include "vgic.h"

extern struct vm g_vm;

void el2_irq_handler(void)
{
    u32 intid = gic_ack_irq();

    if (intid == BOARD_VTIMER_IRQ) {
        /*
         * Physical EL1 virtual-timer IRQ (INTID 27) taken at EL2 via
         * HCR_EL2.IMO=1.  Inject it as a Pending Group-1 virtual interrupt
         * (priority 0xA0) via ICH_LR0_EL2 so the vGIC delivers it to the
         * guest's EL1 IRQ handler immediately after we eret.
         */
        vgic_inject(&g_vm.vcpu, BOARD_VTIMER_IRQ, 0xA0U);
        gic_eoi_irq(intid);
        return;
    }

    printk("[hv] IRQ: unexpected INTID=%u\n", (unsigned)intid);
    gic_eoi_irq(intid);
}
```

- [ ] **Step 2: Replace `vmexit.c` with PSCI wired in**

```c
/* SPDX-License-Identifier: TBD */
#include <types.h>
#include <printk.h>
#include <vm.h>
#include <hypercall.h>
#include <psci.h>

/* Defined in vmexit_asm.S; does not return */
extern void hv_restore(void);

static void handle_hvc(struct vcpu_regs *regs)
{
    u32 func_id = (u32)regs->x[0];
    u8  svc     = (u8)(func_id >> 24);

    if (svc == 0x84 || svc == 0xC4) {   /* 32-bit or 64-bit PSCI */
        psci_handle(regs);
        return;
    }

    switch (func_id) {
    case HC_GUEST_DONE:
        printk("[hv] SVM HVC: done (x1=0x%lx)\n", regs->x[1]);
        hv_restore();   /* no return */
        break;
    default:
        printk("[hv] HVC: unknown func_id=0x%x\n", (unsigned)func_id);
        regs->x[0] = SMCCC_NOT_SUPPORTED;
        break;
    }
}

void handle_exit(struct vcpu_regs *regs, u64 esr)
{
    u32 ec = (u32)(esr >> 26) & 0x3FU;

    switch (ec) {
    case 0x16:   /* HVC from AArch64 EL1 */
        handle_hvc(regs);
        return;
    default:
        printk("[hv] unexpected exit EC=0x%x ESR=0x%lx ELR=0x%lx\n",
               (unsigned)ec, esr, regs->elr_el2);
        for (;;)
            asm volatile("wfi");
    }
}
```

- [ ] **Step 3: Commit**

```bash
git add hypervisor/arch/arm64/irq/irq_handler.c \
        hypervisor/arch/arm64/vmexit/vmexit.c
git commit -m "feat(m2): add EL2 IRQ C handler; wire PSCI dispatch in vmexit"
```

---

## Task 8: EL2 IRQ entry assembly + DAIF unmasking + vectors wiring

**Files:**
- Create: `hypervisor/arch/arm64/irq/irq_handler_asm.S`
- Modify: `hypervisor/arch/arm64/vmexit/vmexit_asm.S`
- Modify: `hypervisor/arch/arm64/boot/vectors.S`

- [ ] **Step 1: Create `hypervisor/arch/arm64/irq/irq_handler_asm.S`**

This save/restore sequence mirrors `el1_sync_handler` from `vmexit_asm.S`. Key difference: after returning from the C handler we re-enable IRQ at EL2 (`daifclr #2`) before ereting so the injected virtual IRQ reaches the guest.

```asm
/* SPDX-License-Identifier: TBD */
/*
 * irq_handler_asm.S -- EL2 IRQ entry while guest (EL1) is running.
 *
 * Physical IRQ taken at EL2 via HCR_EL2.IMO=1.  PSTATE.I is automatically
 * set on exception entry (IRQs masked at EL2).  Steps:
 *   1. Save all guest GPRs + sysregs into g_vm.vcpu.regs
 *   2. Call el2_irq_handler() -- injects virtual IRQ via ICH_LR0_EL2
 *   3. Restore guest state
 *   4. Re-enable IRQ at EL2 (daifclr #2)
 *   5. eret back to guest -- vGIC delivers the virtual IRQ immediately
 */

#include <vm.h>   /* VCPU_* macros */

    .section .text
    .globl el1_irq_handler_asm
el1_irq_handler_asm:
    /* Save live guest x0 onto EL2 stack */
    str     x0, [sp, #-16]!

    /* x0 = &g_vm.vcpu.regs (first field of first field = same addr as g_vm) */
    adrp    x0, g_vm
    add     x0, x0, :lo12:g_vm

    /* Save guest GPRs x1-x30 */
    str     x1,       [x0, #0x08]
    stp     x2,  x3,  [x0, #0x10]
    stp     x4,  x5,  [x0, #0x20]
    stp     x6,  x7,  [x0, #0x30]
    stp     x8,  x9,  [x0, #0x40]
    stp     x10, x11, [x0, #0x50]
    stp     x12, x13, [x0, #0x60]
    stp     x14, x15, [x0, #0x70]
    stp     x16, x17, [x0, #0x80]
    stp     x18, x19, [x0, #0x90]
    stp     x20, x21, [x0, #0xA0]
    stp     x22, x23, [x0, #0xB0]
    stp     x24, x25, [x0, #0xC0]
    stp     x26, x27, [x0, #0xD0]
    stp     x28, x29, [x0, #0xE0]
    str     x30,      [x0, #0xF0]

    /* Retrieve guest x0 from EL2 stack and save */
    ldr     x1, [sp], #16
    str     x1, [x0, #0x00]

    /* Save guest system registers */
    mrs     x1, sp_el1;   str x1, [x0, #VCPU_SP_EL1]
    mrs     x1, elr_el2;  str x1, [x0, #VCPU_ELR]
    mrs     x1, spsr_el2; str x1, [x0, #VCPU_SPSR]

    /* Call EL2 IRQ C handler */
    bl      el2_irq_handler

    /* Restore guest sysregs */
    adrp    x0, g_vm
    add     x0, x0, :lo12:g_vm
    ldr     x1, [x0, #VCPU_ELR];    msr elr_el2, x1
    ldr     x1, [x0, #VCPU_SPSR];   msr spsr_el2, x1
    ldr     x1, [x0, #VCPU_SP_EL1]; msr sp_el1, x1

    /* Restore guest GPRs */
    ldr     x1,       [x0, #0x08]
    ldp     x2,  x3,  [x0, #0x10]
    ldp     x4,  x5,  [x0, #0x20]
    ldp     x6,  x7,  [x0, #0x30]
    ldp     x8,  x9,  [x0, #0x40]
    ldp     x10, x11, [x0, #0x50]
    ldp     x12, x13, [x0, #0x60]
    ldp     x14, x15, [x0, #0x70]
    ldp     x16, x17, [x0, #0x80]
    ldp     x18, x19, [x0, #0x90]
    ldp     x20, x21, [x0, #0xA0]
    ldp     x22, x23, [x0, #0xB0]
    ldp     x24, x25, [x0, #0xC0]
    ldp     x26, x27, [x0, #0xD0]
    ldp     x28, x29, [x0, #0xE0]
    ldr     x30,      [x0, #0xF0]
    ldr     x0,       [x0, #0x00]

    /* Re-enable IRQ at EL2 before returning to guest */
    msr     daifclr, #2
    eret
```

- [ ] **Step 2: Add `msr daifclr, #2` before guest `eret` in `vmexit_asm.S`**

In `hypervisor/arch/arm64/vmexit/vmexit_asm.S`, find the final two instructions of `vcpu_run`:

```asm
    ldr     x0,       [x0, #0x00]   /* x0 last */

    eret
```

Change to:

```asm
    ldr     x0,       [x0, #0x00]   /* x0 last */

    /* Unmask IRQ at EL2: physical IRQs will be taken here via HCR_EL2.IMO=1 */
    msr     daifclr, #2
    eret
```

- [ ] **Step 3: Wire IRQ vector in `hypervisor/arch/arm64/boot/vectors.S`**

Change the `Lower EL, AArch64` block from:

```asm
    /* Lower EL, AArch64 */
    VECTOR_ENTRY el1_sync_handler
    VECTOR_ENTRY panic_vector
    VECTOR_ENTRY panic_vector
    VECTOR_ENTRY panic_vector
```

to:

```asm
    /* Lower EL, AArch64 */
    VECTOR_ENTRY el1_sync_handler    /* +0x400: sync  (HVC trap) */
    VECTOR_ENTRY el1_irq_handler_asm /* +0x480: IRQ   (phys IRQ via IMO=1) */
    VECTOR_ENTRY panic_vector        /* +0x500: FIQ */
    VECTOR_ENTRY panic_vector        /* +0x580: SError */
```

- [ ] **Step 4: Commit**

```bash
git add hypervisor/arch/arm64/irq/irq_handler_asm.S \
        hypervisor/arch/arm64/vmexit/vmexit_asm.S \
        hypervisor/arch/arm64/boot/vectors.S
git commit -m "feat(m2): add EL2 IRQ entry asm; unmask DAIF.I for guest; wire IRQ vector"
```

---

## Task 9: Wire everything -- Makefiles + `main.c` + `vm.c`

**Files:**
- Modify: `hypervisor/arch/arm64/Makefile`
- Modify: `hypervisor/Makefile`
- Modify: `hypervisor/boot/main.c`
- Modify: `hypervisor/common/vm/vm.c`

- [ ] **Step 1: Update `hypervisor/arch/arm64/Makefile`**

```make
# SPDX-License-Identifier: TBD
arch-objs := \
    arch/arm64/boot/head.o \
    arch/arm64/boot/vectors.o \
    arch/arm64/cpu/cpu.o \
    arch/arm64/board/$(BOARD)/board.o \
    arch/arm64/mmu/stage2.o \
    arch/arm64/vmexit/vmexit.o \
    arch/arm64/vmexit/vmexit_asm.o \
    arch/arm64/irq/gic_v3.o \
    arch/arm64/irq/vgic.o \
    arch/arm64/irq/irq_handler.o \
    arch/arm64/irq/irq_handler_asm.o \
    arch/arm64/timer/vtimer.o

arch-includes := \
    -Ihypervisor/arch/arm64/board/$(BOARD) \
    -Ihypervisor/arch/arm64/include \
    -Ihypervisor/arch/arm64/mmu \
    -Ihypervisor/arch/arm64/irq \
    -Ihypervisor/arch/arm64/timer

arch-ldscript := hypervisor/arch/arm64/board/$(BOARD)/linker.lds
```

- [ ] **Step 2: Update `hypervisor/Makefile`**

```make
# SPDX-License-Identifier: TBD
hv-objs := \
    boot/main.o \
    debug/uart_pl011.o \
    lib/string.o \
    lib/print.o \
    common/vm/vm.o \
    common/psci/psci.o

hv-includes := \
    -Ihypervisor/include \
    -Ihypervisor/common/vm \
    -Ihypervisor/common/psci
```

- [ ] **Step 3: Update `hypervisor/boot/main.c`**

```c
/* SPDX-License-Identifier: TBD */
#include <types.h>
#include <uart.h>
#include <printk.h>
#include <board.h>
#include <vm.h>
#include <gic_v3.h>
#include <vtimer.h>

extern u64 read_currentel(void);
extern void cpu_wfi(void);

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
    vm_init();    /* calls vgic_init() internally */
    vm_run();

    for (;;)
        cpu_wfi();
}
```

- [ ] **Step 4: Update `hypervisor/common/vm/vm.c`**

```c
/* SPDX-License-Identifier: TBD */
#include <types.h>
#include <printk.h>
#include <vm.h>
#include <vgic.h>
#include "vm_config.h"
#include "stage2.h"

/* Non-static: referenced by symbol from vmexit_asm.S and irq_handler_asm.S */
struct vm g_vm;

void vm_init(void)
{
    g_vm.config = &svm_config;

    /*
     * SPSR_EL2 = 0x3C5:
     *   M[4:0] = 0b00101 = EL1h (use SP_EL1)
     *   DAIF   = 0b1111  (bits[9:6], all masked -- SVM unmasks itself)
     */
    g_vm.vcpu.regs.elr_el2  = svm_config.entry;
    g_vm.vcpu.regs.spsr_el2 = 0x3C5ULL;
    g_vm.vcpu.regs.sp_el1   = svm_config.mem_base + svm_config.mem_size - 0x10UL;

    /*
     * HCR_EL2 bits:
     *   VM  (0)  = 1: Stage-2 translation enabled
     *   FMO (3)  = 1: route physical FIQ to EL2
     *   IMO (4)  = 1: route physical IRQ to EL2 (critical for vtimer delivery)
     *   AMO (5)  = 1: route SError to EL2
     *   RW  (31) = 1: EL1 executes AArch64
     *   HCD (29) = 0: HVC from EL1 allowed
     */
    g_vm.vcpu.hcr_el2 = (1ULL << 0) | (1ULL << 3) | (1ULL << 4) |
                        (1ULL << 5) | (1ULL << 31);

    stage2_init(&g_vm.vcpu, (u32)svm_config.vmid);
    vgic_init(&g_vm.vcpu);

    printk("[hv] SVM: launching VMID=%u entry=0x%lx\n",
           (unsigned)svm_config.vmid, svm_config.entry);
}

void vm_run(void)
{
    stage2_activate(&g_vm.vcpu);
    vgic_restore(&g_vm.vcpu);   /* load vGIC state into ICH_* registers */
    vcpu_run(&g_vm.vcpu);
    /* Returns here after hv_restore() is called from HC_GUEST_DONE */
}
```

- [ ] **Step 5: Full build**

```bash
cd /home/corsair/Music/virtual/hyp-/hypervisor-
make
```

Expected: zero warnings. Verify new objects exist:

```
build/obj/arch/arm64/irq/gic_v3.o
build/obj/arch/arm64/irq/vgic.o
build/obj/arch/arm64/irq/irq_handler.o
build/obj/arch/arm64/irq/irq_handler_asm.o
build/obj/arch/arm64/timer/vtimer.o
build/obj/common/psci/psci.o
```

**Common failures:**
- `gic_init undeclared` in `main.c`: the top-level Makefile must combine `arch-includes` and `hv-includes` for all translation units — verify the top-level `Makefile` does this (it did in M1).
- `unknown register name` in assembler: the sysreg.h aliases from Task 3 were not applied.
- `vgic.h: No such file`: `arch-includes` is missing `-Ihypervisor/arch/arm64/irq`.

- [ ] **Step 6: readelf sanity check**

```bash
aarch64-none-linux-gnu-readelf -h build/hypervisor.elf | grep 'Entry point'
```

Expected: `Entry point address: 0x40080000`

- [ ] **Step 7: Commit**

```bash
git add hypervisor/arch/arm64/Makefile \
        hypervisor/Makefile \
        hypervisor/boot/main.c \
        hypervisor/common/vm/vm.c
git commit -m "feat(m2): wire GIC/vGIC/vtimer/PSCI into build; update main.c and vm.c"
```

---

## Task 10: M2 SVM test binary + full verification

**Files:**
- Create: `tests/svm2/svm2_vectors.S`
- Create: `tests/svm2/svm2_main.c`
- Create: `tests/svm2/svm2.lds`
- Create: `tests/run_svm2_test.sh`

The SVM2 payload demonstrates the complete virtual timer IRQ cycle at EL1.

- [ ] **Step 1: Create `tests/svm2/svm2_vectors.S`**

```asm
/* SPDX-License-Identifier: TBD */
/* Minimal EL1 vector table for the M2 SVM test payload */

    .section .text.vectors
    .align 11           /* VBAR_EL1 requires 2048-byte alignment */
    .globl svm2_vectors
svm2_vectors:
    /* Current EL with SP_EL0 (offsets 0x000-0x1FF) */
    .align 7; b .
    .align 7; b .
    .align 7; b .
    .align 7; b .
    /* Current EL with SP_ELx (offsets 0x200-0x3FF) -- SVM runs at EL1h */
    .align 7; b .                    /* +0x200: sync   */
    .align 7; b svm2_irq_handler     /* +0x280: IRQ    <- virtual timer */
    .align 7; b .                    /* +0x300: FIQ    */
    .align 7; b .                    /* +0x380: SError */
    /* Lower EL AArch64 (offsets 0x400-0x5FF) */
    .align 7; b .
    .align 7; b .
    .align 7; b .
    .align 7; b .
    /* Lower EL AArch32 (offsets 0x600-0x7FF) */
    .align 7; b .
    .align 7; b .
    .align 7; b .
    .align 7; b .

/* ---- EL1 IRQ handler (SP_ELx) ---------------------------------------- */
    .section .text
    .globl svm2_irq_handler
svm2_irq_handler:
    stp     x29, x30, [sp, #-16]!

    /* Acknowledge virtual interrupt: ICC_IAR1_EL1 -> x0 = vINTID */
    mrs     x0, S3_0_C12_C12_0     /* ICC_IAR1_EL1 */
    and     x0, x0, #0xFFFFFF      /* bits[23:0] = INTID */

    /* Call C handler: svm2_handle_irq(u64 intid) */
    bl      svm2_handle_irq

    ldp     x29, x30, [sp], #16
    eret
```

- [ ] **Step 2: Create `tests/svm2/svm2_main.c`**

```c
/* SPDX-License-Identifier: TBD */
/*
 * svm2_main.c -- M2 SVM test payload.
 *
 * Configures the vGIC at EL1, arms the virtual timer, waits for the EL1 IRQ
 * (delivered via EL2 injection into ICH_LR0_EL2), handles it, and signals
 * completion to the hypervisor via HC_GUEST_DONE HVC.
 */

#define UART_DR ((volatile unsigned int *)0x09000000U)
#define UART_FR ((volatile unsigned int *)0x09000018U)

typedef unsigned long long u64;

/*
 * Use S<op0>_<op1>_<Cn>_<Cm>_<op2> encodings for maximum binutils compatibility.
 * Named forms (ICC_SRE_EL1 etc.) require binutils >= 2.34.
 */
#define ICC_SRE_EL1_ENC      "S3_0_C12_C12_5"
#define ICC_PMR_EL1_ENC      "S3_0_C4_C6_0"
#define ICC_IGRPEN1_EL1_ENC  "S3_0_C12_C12_7"
#define ICC_EOIR1_EL1_ENC    "S3_0_C12_C12_1"
#define ICC_DIR_EL1_ENC      "S3_0_C12_C11_1"

static void uart_putc(char c)
{
    while (*UART_FR & (1U << 5)) {}
    *UART_DR = (unsigned int)c;
}

static void uart_puts(const char *s)
{
    while (*s)
        uart_putc(*s++);
}

/* Called from svm2_irq_handler (assembly) with vINTID in first argument (x0) */
void svm2_handle_irq(u64 intid)
{
    uart_puts("[svm] virtual timer IRQ received (INTID=27)\n");

    /* Disarm virtual timer to prevent re-fire */
    asm volatile("msr cntv_ctl_el0, xzr");

    /* EOI: priority drop then deactivate */
    asm volatile("msr " ICC_EOIR1_EL1_ENC ", %0" :: "r"(intid));
    asm volatile("msr " ICC_DIR_EL1_ENC   ", %0" :: "r"(intid));
    asm volatile("isb");
}

void svm2_start(void)
{
    extern char svm2_vectors[];

    /* 1. Install EL1 vector table */
    asm volatile("msr vbar_el1, %0\nisb" :: "r"(svm2_vectors));

    uart_puts("[svm] EL1 init\n");

    /* 2. Enable GIC system register interface at EL1
     *    ICC_SRE_EL1 = 7 (SRE=1, DIL=1, DFB=1)
     *    Requires ICC_SRE_EL2.Enable=1 set by hypervisor in gic_init() */
    asm volatile("msr " ICC_SRE_EL1_ENC ", %0\nisb" :: "r"(7ULL));

    /* 3. Allow all interrupt priorities; enable Group 1 */
    asm volatile("msr " ICC_PMR_EL1_ENC     ", %0" :: "r"(0xFFULL));
    asm volatile("msr " ICC_IGRPEN1_EL1_ENC ", %0" :: "r"(1ULL));
    asm volatile("isb");

    uart_puts("[svm] GIC EL1 configured\n");

    /*
     * 4. Arm EL1 virtual timer for ~83ms.
     *    QEMU virt Cortex-A72: CNTFRQ_EL0 = 62500000 Hz.
     *    62500000 / 12 = 5208333 ticks ~= 83ms.
     *    CNTV_CTL_EL0 = 1: ENABLE=1, IMASK=0.
     */
    asm volatile("msr cntv_tval_el0, %0" :: "r"(5208333ULL));
    asm volatile("msr cntv_ctl_el0,  %0" :: "r"(1ULL));

    uart_puts("[svm] virtual timer armed\n");

    /* 5. Unmask IRQ at EL1 and wait for interrupt */
    asm volatile("msr daifclr, #2");
    asm volatile("wfi");

    /*
     * 6. Signal done.
     *    HC_GUEST_DONE = 0x80000001.  mov #1 + movk fills the upper bits.
     */
    uart_puts("[svm] signalling HVC done\n");
    asm volatile(
        "mov  x0, #1\n"
        "movk x0, #0x8000, lsl #16\n"
        "hvc  #0\n"
        ::: "x0"
    );

    for (;;)
        asm volatile("wfi");
}
```

- [ ] **Step 3: Create `tests/svm2/svm2.lds`**

```ld
ENTRY(svm2_start)
SECTIONS {
    . = 0x40200000;
    /* Vector table first: VBAR_EL1 = 0x40200000 (2048-byte aligned by .align 11) */
    .text.vectors : { *(.text.vectors) }
    .text         : { *(.text*) }
    .rodata       : { *(.rodata*) }
    .data         : { *(.data*) }
    .bss          : {
        __svm2_bss_start = .;
        *(.bss*) *(COMMON)
        __svm2_bss_end = .;
    }
    /* Stack provided by hypervisor: SP_EL1 = 0x40400000 - 0x10 */
}
```

- [ ] **Step 4: Create `tests/run_svm2_test.sh`**

```sh
#!/bin/sh
set -eu

PROJ_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$PROJ_ROOT/build/svm2"

mkdir -p "$BUILD_DIR"

echo "[run_svm2] Building SVM2 payload..."
aarch64-none-linux-gnu-gcc \
    -ffreestanding -nostdlib -nostartfiles \
    -mgeneral-regs-only -mstrict-align \
    -Wall -Wextra -Werror -O2 -g \
    -T "$PROJ_ROOT/tests/svm2/svm2.lds" \
    "$PROJ_ROOT/tests/svm2/svm2_vectors.S" \
    "$PROJ_ROOT/tests/svm2/svm2_main.c" \
    -o "$BUILD_DIR/svm2.elf"

aarch64-none-linux-gnu-objcopy -O binary "$BUILD_DIR/svm2.elf" "$BUILD_DIR/svm2.bin"

echo "[run_svm2] Binary: $BUILD_DIR/svm2.bin"
echo "[run_svm2] Starting QEMU (Ctrl-A x to exit)..."
SVM_BIN="$BUILD_DIR/svm2.bin" make -C "$PROJ_ROOT" run
```

```bash
chmod +x tests/run_svm2_test.sh
```

- [ ] **Step 5: Build SVM2**

```bash
cd /home/corsair/Music/virtual/hyp-/hypervisor-
bash tests/run_svm2_test.sh 2>&1 | head -6
```

Expected: `[run_svm2] Binary: .../svm2.bin` with no compiler errors.

If `cntv_tval_el0` or `cntv_ctl_el0` are rejected, replace in `svm2_main.c`:
- `cntv_tval_el0` -> `S3_3_C14_C3_0`
- `cntv_ctl_el0` -> `S3_3_C14_C3_1`

- [ ] **Step 6: Functional verification -- observe all ten output lines**

```bash
tests/run_svm2_test.sh
```

Expected output (order must match):

```
  H   H Y   Y PPPP  EEEEE RRRR  V   V IIIII SSSSS  OOO  RRRR
  H   H  Y Y  P   P E     R   R V   V   I   S     O   O R   R
  HHHHH   Y   PPPP  EEE   RRRR  V   V   I   SSSSS O   O RRRR
  H   H   Y   P     E     R R    V V    I       S  O   O R R
  H   H   Y   P     EEEEE R  R    V   IIIII SSSSS   OOO  R  R

[hv] Hello from EL2 on qemu_virt, CurrentEL=0x8
[hv] GIC: initialized (dist=0x8000000 rdist=0x80a0000 PPI=27)
[hv] vtimer: CNTHCTL_EL2=0x3, CNTVOFF_EL2=0x0
[hv] SVM: launching VMID=1 entry=0x40200000
[svm] EL1 init
[svm] GIC EL1 configured
[svm] virtual timer armed
[svm] virtual timer IRQ received (INTID=27)
[svm] signalling HVC done
[hv] SVM HVC: done (x1=0x0)
```

QEMU then halts (HV in `wfi`). Press `Ctrl-A x`.

**Troubleshooting:**
- Hangs after `[svm] virtual timer armed` with no IRQ: `daifclr #2` missing from `vcpu_run` eret path in `vmexit_asm.S`.
- `[hv] unexpected exit EC=0x18`: `vtimer_init()` not called before `vm_run()`, or called after.
- `[hv] IRQ: unexpected INTID=N`: GICR_ISENABLER0 bit 27 not set, or PPI not in Group 1 NS.
- `[hv] unexpected exit EC=0x00` (unknown sync): SVM faulting on ICC_* access; `ICC_SRE_EL2.Enable` not set (check `gic_init` runs before `vm_run`).

- [ ] **Step 7: PSCI verification (optional)**

Temporarily add before GIC init in `svm2_start()`:

```c
    u64 ver;
    asm volatile(
        "mov  x0, #0\n"
        "movk x0, #0x8400, lsl #16\n"   /* 0x84000000 = PSCI_VERSION */
        "hvc  #0\n"
        "mov  %0, x0\n"
        : "=r"(ver) :: "x0"
    );
    uart_puts("[svm] PSCI VERSION=0x");
    for (int s = 28; s >= 0; s -= 4) {
        unsigned d = (unsigned)((ver >> s) & 0xFU);
        uart_putc((char)(d < 10 ? '0' + d : 'a' + d - 10));
    }
    uart_putc('\n');
```

Expected new first line: `[svm] PSCI VERSION=0x00010001`. Revert after confirming.

- [ ] **Step 8: Commit test files**

```bash
git add tests/svm2/svm2_vectors.S \
        tests/svm2/svm2_main.c \
        tests/svm2/svm2.lds \
        tests/run_svm2_test.sh
git commit -m "feat(m2): add SVM2 test payload (virtual timer IRQ end-to-end)"
```

---

## Self-Review

**Spec coverage (CLAUDE.md M2 goal: vGICv3, virtual timer, PSCI):**

| Requirement | Task |
|---|---|
| Physical GICv3: GICD, GICR, ICC_SRE_EL2, PPI 27 enabled | 3, 4 |
| vGICv3: ICH_HCR_EL2, ICH_VMCR_EL2, ICH_LR0..LR3 | 5 |
| EL2 IRQ handler: save guest, inject virtual, restore, eret | 7, 8 |
| DAIF.I unmasked at EL2 before guest entry | 8 |
| Virtual timer: CNTHCTL_EL2=3, CNTVOFF_EL2=0 | 6 |
| PSCI: VERSION, FEATURES, CPU_OFF, SYSTEM_OFF, CPU_ON stub | 1, 2 |
| PSCI wired into `handle_hvc` | 7 |
| End-to-end: SVM receives INTID 27, handles, exits cleanly | 10 |
| Makefiles, include paths, build integration | 9 |

**Name consistency check:**
- `gic_init()`: declared in `gic_v3.h`, defined in `gic_v3.c`, called from `main.c` via `<gic_v3.h>` include. The header lands in compiler search path via `arch-includes`. ✓
- `vgic_init()`: declared in `vgic.h`, defined in `vgic.c`, called from `vm_init()` via `<vgic.h>`. `vm.c` is compiled with `arch-includes` (which contains `-Ihypervisor/arch/arm64/irq`). ✓
- `vgic_restore()`: same header path. ✓
- `vtimer_init()`: declared in `vtimer.h`, defined in `vtimer.c`, called from `main.c` via `<vtimer.h>`. ✓
- `psci_handle()`: declared in `psci.h`, defined in `psci.c`, included in `vmexit.c` via `<psci.h>`. ✓
- `el1_irq_handler_asm`: `.globl` in `irq_handler_asm.S`, referenced in `vectors.S`. ✓
- `el2_irq_handler()`: defined in `irq_handler.c`, called from `irq_handler_asm.S` via `bl`. ✓

**Struct vcpu ABI:** New fields (`ich_hcr_el2` at 0x120, `ich_vmcr_el2` at 0x128, `ich_lr[4]` at 0x130) are appended after `vttbr_el2`. Assembly constants `VCPU_HCR_EL2=0x110` and `VCPU_VTTBR_EL2=0x118` are unchanged. No assembly offset macro updates needed. ✓

**DAIF.I lifecycle (correctness proof):**
1. M0 head.S: `daifset #0xF` -- all masked on EL2 boot
2. `vcpu_run`: saves HV context, configures HCR_EL2 (IMO=1), loads guest, **`daifclr #2`**, `eret`
3. Guest runs; physical IRQ fires -> `el1_irq_handler_asm` (DAIF.I auto-re-masked on exception entry)
4. IRQ handler: inject virtual IRQ -> **`daifclr #2`** -> `eret` back to guest with pending vIRQ
5. HVC exit -> `el1_sync_handler` (DAIF.I auto-masked on exception entry) -> `hv_restore` -> returns to `vm_run()` with DAIF.I still masked ✓

**Forward compat (M3 SMP hooks):**
- `g_hv_ctx` in `vmexit_asm.S` -- must become per-CPU array
- `g_vm` in `vm.c` / `irq_handler.c` -- must become array indexed by vCPU
- PSCI `CPU_ON` returns `NOT_SUPPORTED` with "M3" comment
- `vgic_save/restore` use `struct vcpu *` -- ready for multi-vCPU scheduling

---

**Plan complete and saved to `docs/superpowers/plans/2026-05-30-hypervisor-m2.md`.**

Two execution options:

**1. Subagent-Driven (recommended)** -- I dispatch a fresh subagent per task, review between tasks, fast iteration

**2. Inline Execution** -- Execute tasks in this session using executing-plans, batch with checkpoints

Which approach?
