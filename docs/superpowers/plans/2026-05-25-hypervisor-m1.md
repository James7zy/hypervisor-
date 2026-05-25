# Hypervisor M1 — Bare-Metal SVM Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Enable the hypervisor to launch a Service VM (SVM) at EL1 under Stage-2 MMU control, trap its HVC call, and return cleanly to EL2.

**Architecture:** Static single-vCPU VM launched via `eret` from EL2; Stage-2 uses two 1 GB identity-mapped L1 block entries; HVC dispatch follows SMCCC vendor convention. No memory allocator — all structures in BSS.

**Tech Stack:** AArch64 GAS assembly, C11 freestanding, `aarch64-none-linux-gnu-gcc ≥ 10`, `qemu-system-aarch64 ≥ 6.0`.

**Project root:** `/home/corsair/Music/virtual/hyp-/hypervisor-/`

**Spec:** `docs/superpowers/specs/2026-05-24-m1-bare-metal-svm-design.md`

**Build:** `make defconfig && make` — must produce zero warnings (`-Werror`).

**Verification:** `SVM_BIN=/path/to/svm.bin make run` — observe four output lines in order (see Task 8).

---

## File Map

| Action | Path | Purpose |
|---|---|---|
| Modify | `hypervisor/arch/arm64/board/qemu_virt/board.h` | Add SVM address constants |
| Create | `hypervisor/include/hypercall.h` | SMCCC HVC constants |
| Create | `hypervisor/include/vm.h` | `struct vcpu_regs`, `struct vcpu`, `struct vm`, `struct hv_ctx`, ASM offsets |
| Create | `hypervisor/common/vm/vm_config.h` | `struct vm_config`, static `svm_config` |
| Create | `hypervisor/arch/arm64/mmu/stage2.h` | `stage2_init()` / `stage2_activate()` API |
| Create | `hypervisor/arch/arm64/mmu/stage2.c` | Stage-2 L1 table, VTCR_EL2, VTTBR_EL2 |
| Create | `hypervisor/common/vm/vm.c` | `vm_init()`, `vm_run()`, `g_vm` |
| Create | `hypervisor/arch/arm64/vmexit/vmexit.c` | `handle_exit()`, `handle_hvc()` |
| Create | `hypervisor/arch/arm64/vmexit/vmexit_asm.S` | `vcpu_run`, `el1_sync_handler`, `hv_restore`, `g_hv_ctx` |
| Modify | `hypervisor/arch/arm64/boot/vectors.S` | Wire Lower EL AArch64 sync to `el1_sync_handler` |
| Modify | `hypervisor/boot/main.c` | Call `vm_init()` + `vm_run()` after banner |
| Modify | `hypervisor/Makefile` | Add `common/vm/vm.o` and `-Ihypervisor/common/vm` |
| Modify | `hypervisor/arch/arm64/Makefile` | Add `mmu/stage2.o`, `vmexit/vmexit.o`, `vmexit/vmexit_asm.o` |
| Modify | `scripts/run-qemu.sh` | Add `-device loader` for SVM binary |

---

## Task 1: SVM board constants + `hypercall.h`

**Files:**
- Modify: `hypervisor/arch/arm64/board/qemu_virt/board.h`
- Create: `hypervisor/include/hypercall.h`

- [ ] **Step 1: Add SVM constants to `board.h`**

Append after `#define BOARD_DRAM_BASE`:

```c
/* M1: SVM interface contract — stable ABI between hypervisor and external SVM binary */
#define BOARD_SVM_ENTRY    0x40200000UL
#define BOARD_SVM_MEM_BASE 0x40200000UL
#define BOARD_SVM_MEM_SIZE 0x00200000UL   /* 2 MB */
```

- [ ] **Step 2: Create `hypervisor/include/hypercall.h`**

```c
/* SPDX-License-Identifier: TBD */
#ifndef HV_HYPERCALL_H
#define HV_HYPERCALL_H

#define SMCCC_NOT_SUPPORTED  (~0ULL)
#define HVC_VENDOR_BASE      0x80000000U
#define HC_GUEST_DONE        (HVC_VENDOR_BASE | 0x0001U)

#endif /* HV_HYPERCALL_H */
```

- [ ] **Step 3: Build check**

```bash
cd /home/corsair/Music/virtual/hyp-/hypervisor-
make
```

Expected: zero warnings, `build/hypervisor.elf` produced unchanged.

- [ ] **Step 4: Commit**

```bash
git add hypervisor/arch/arm64/board/qemu_virt/board.h \
        hypervisor/include/hypercall.h
git commit -m "feat(m1): add SVM board constants and hypercall.h"
```

---

## Task 2: VM/vCPU type definitions (`vm.h` + `vm_config.h`)

**Files:**
- Create: `hypervisor/include/vm.h`
- Create: `hypervisor/common/vm/vm_config.h`

- [ ] **Step 1: Create `hypervisor/include/vm.h`**

```c
/* SPDX-License-Identifier: TBD */
#ifndef HV_VM_H
#define HV_VM_H

#include <types.h>
#include <board.h>

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
};

struct vm_config;

struct vm {
    struct vcpu            vcpu;
    const struct vm_config *config;
};

struct hv_ctx {
    u64 x19, x20, x21, x22, x23, x24, x25, x26, x27, x28, x29;
    u64 lr;   /* offset 0x058 */
    u64 sp;   /* offset 0x060 */
};

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
#endif

extern struct vm g_vm;

extern void vcpu_run(struct vcpu *vcpu);
extern void hv_restore(void);

void vm_init(void);
void vm_run(void);

#endif /* HV_VM_H */
```

- [ ] **Step 2: Create `hypervisor/common/vm/vm_config.h`**

```c
/* SPDX-License-Identifier: TBD */
#ifndef HV_VM_CONFIG_H
#define HV_VM_CONFIG_H

#include <types.h>
#include <board.h>

struct vm_config {
    u32       vmid;
    uintptr_t entry;
    uintptr_t mem_base;
    size_t    mem_size;
};

static const struct vm_config svm_config = {
    .vmid     = 1,
    .entry    = BOARD_SVM_ENTRY,
    .mem_base = BOARD_SVM_MEM_BASE,
    .mem_size = BOARD_SVM_MEM_SIZE,
};

#endif /* HV_VM_CONFIG_H */
```

- [ ] **Step 3: Build check**

```bash
make
```

Expected: zero warnings. (Headers only — existing objects recompile cleanly.)

- [ ] **Step 4: Commit**

```bash
git add hypervisor/include/vm.h hypervisor/common/vm/vm_config.h
git commit -m "feat(m1): add vm.h and vm_config.h type definitions"
```

---

## Task 3: Stage-2 page table (`stage2.h` + `stage2.c`)

**Files:**
- Create: `hypervisor/arch/arm64/mmu/stage2.h`
- Create: `hypervisor/arch/arm64/mmu/stage2.c`
- Modify: `hypervisor/arch/arm64/Makefile`

- [ ] **Step 1: Create `hypervisor/arch/arm64/mmu/stage2.h`**

```c
/* SPDX-License-Identifier: TBD */
#ifndef ARCH_STAGE2_H
#define ARCH_STAGE2_H

#include <types.h>
#include <vm.h>

void stage2_init(struct vcpu *vcpu, u32 vmid);
void stage2_activate(const struct vcpu *vcpu);

#endif /* ARCH_STAGE2_H */
```

- [ ] **Step 2: Create `hypervisor/arch/arm64/mmu/stage2.c`**

```c
/* SPDX-License-Identifier: TBD */
#include <types.h>
#include <vm.h>
#include "stage2.h"

/*
 * VTCR_EL2: T0SZ=25 (39-bit IPA), SL0=1 (L1 start), IRGN0/ORGN0=1 (WB RA-WA),
 * SH0=3 (Inner Shareable), TG0=0 (4KB granule), PS=2 (40-bit PA).
 */
#define VTCR_T0SZ   (25ULL << 0)
#define VTCR_SL0    (1ULL  << 6)
#define VTCR_IRGN0  (1ULL  << 8)
#define VTCR_ORGN0  (1ULL  << 10)
#define VTCR_SH0    (3ULL  << 12)
#define VTCR_TG0    (0ULL  << 14)
#define VTCR_PS     (2ULL  << 16)
#define VTCR_EL2_VALUE \
    (VTCR_T0SZ | VTCR_SL0 | VTCR_IRGN0 | VTCR_ORGN0 | VTCR_SH0 | VTCR_TG0 | VTCR_PS)

/* Stage-2 Level-1 block descriptor fields */
#define S2_BLOCK        0x1ULL
#define S2_MEMATTR_DEV  (0x1ULL << 2)   /* Device-nGnRE: MemAttr[3:0]=0001 */
#define S2_MEMATTR_NORM (0xFULL << 2)   /* Normal WB inner+outer: MemAttr=1111 */
#define S2_S2AP_RW      (0x3ULL << 6)   /* R/W EL0+EL1 */
#define S2_SH_OSH       (0x2ULL << 8)   /* Outer Shareable */
#define S2_SH_ISH       (0x3ULL << 8)   /* Inner Shareable */
#define S2_AF           (1ULL   << 10)  /* Access Flag */
#define S2_XN           (1ULL   << 54)  /* Execute-never */

static u64 l1_table[512];   /* 4 KB, BSS */

void stage2_init(struct vcpu *vcpu, u32 vmid)
{
    /* IPA 0x00000000–0x3FFFFFFF → PA 0x00000000: Device (covers UART @ 0x09000000) */
    l1_table[0] = 0x00000000UL |
                  S2_BLOCK | S2_MEMATTR_DEV | S2_S2AP_RW | S2_SH_OSH | S2_AF | S2_XN;

    /* IPA 0x40000000–0x7FFFFFFF → PA 0x40000000: Normal WB (covers all DRAM) */
    l1_table[1] = 0x40000000UL |
                  S2_BLOCK | S2_MEMATTR_NORM | S2_S2AP_RW | S2_SH_ISH | S2_AF;

    vcpu->vttbr_el2 = ((u64)vmid << 48) | (u64)(uintptr_t)l1_table;
}

void stage2_activate(const struct vcpu *vcpu)
{
    asm volatile(
        "msr vtcr_el2,  %0\n"
        "msr vttbr_el2, %1\n"
        "isb\n"
        :
        : "r"((u64)VTCR_EL2_VALUE), "r"(vcpu->vttbr_el2)
        : "memory"
    );
}
```

- [ ] **Step 3: Add `stage2.o` to `hypervisor/arch/arm64/Makefile`**

```make
arch-objs := \
    arch/arm64/boot/head.o \
    arch/arm64/boot/vectors.o \
    arch/arm64/cpu/cpu.o \
    arch/arm64/board/$(BOARD)/board.o \
    arch/arm64/mmu/stage2.o
```

- [ ] **Step 4: Build check**

```bash
make
```

Expected: zero warnings. `build/obj/arch/arm64/mmu/stage2.o` present.

- [ ] **Step 5: Commit**

```bash
git add hypervisor/arch/arm64/mmu/stage2.h \
        hypervisor/arch/arm64/mmu/stage2.c \
        hypervisor/arch/arm64/Makefile
git commit -m "feat(m1): add Stage-2 page table (stage2.c)"
```

---

## Task 4: VM init and run (`vm.c`)

**Files:**
- Create: `hypervisor/common/vm/vm.c`
- Modify: `hypervisor/Makefile`

- [ ] **Step 1: Create `hypervisor/common/vm/vm.c`**

```c
/* SPDX-License-Identifier: TBD */
#include <types.h>
#include <printk.h>
#include <vm.h>
#include "vm_config.h"
#include "../../../arch/arm64/mmu/stage2.h"

/* Non-static: vmexit_asm.S references g_vm by symbol */
struct vm g_vm;

void vm_init(void)
{
    g_vm.config = &svm_config;

    /*
     * SPSR_EL2 = 0x3C5:
     *   M[4:0] = 0b00101 = EL1h (use SP_EL1)
     *   DAIF   = 0b1111  (bits[9:6], all interrupts masked)
     */
    g_vm.vcpu.regs.elr_el2  = svm_config.entry;
    g_vm.vcpu.regs.spsr_el2 = 0x3C5ULL;
    g_vm.vcpu.regs.sp_el1   = svm_config.mem_base + svm_config.mem_size - 0x10UL;

    /*
     * HCR_EL2: VM(0)|FMO(3)|IMO(4)|AMO(5) set; HCD(29) clear (allow HVC).
     */
    g_vm.vcpu.hcr_el2 = (1ULL << 0) | (1ULL << 3) | (1ULL << 4) | (1ULL << 5);

    stage2_init(&g_vm.vcpu, (u32)svm_config.vmid);

    printk("[hv] SVM: launching VMID=%u entry=0x%lx\n",
           (unsigned)svm_config.vmid, svm_config.entry);
}

void vm_run(void)
{
    stage2_activate(&g_vm.vcpu);
    vcpu_run(&g_vm.vcpu);
    /* Returns here after hv_restore() is called from HVC handler */
}
```

- [ ] **Step 2: Update `hypervisor/Makefile`**

```make
hv-objs := \
    boot/main.o \
    debug/uart_pl011.o \
    lib/string.o \
    lib/print.o \
    common/vm/vm.o

hv-includes := \
    -Ihypervisor/include \
    -Ihypervisor/common/vm
```

- [ ] **Step 3: Compile check (vm.c only, before full link)**

```bash
cd /home/corsair/Music/virtual/hyp-/hypervisor-
aarch64-none-linux-gnu-gcc \
  -ffreestanding -nostdlib -mgeneral-regs-only -mstrict-align \
  -Wall -Wextra -Werror -O2 -g \
  -Ihypervisor/include \
  -Ihypervisor/arch/arm64/board/qemu_virt \
  -Ihypervisor/arch/arm64/include \
  -Ihypervisor/common/vm \
  -c hypervisor/common/vm/vm.c -o /tmp/vm_check.o
```

Expected: zero warnings, `/tmp/vm_check.o` produced. (`vcpu_run` is extern — linker error comes later, full link deferred to Task 7.)

- [ ] **Step 4: Commit**

```bash
git add hypervisor/common/vm/vm.c hypervisor/Makefile
git commit -m "feat(m1): add vm_init/vm_run (vm.c)"
```

---

## Task 5: HVC exit handler (`vmexit.c`)

**Files:**
- Create: `hypervisor/arch/arm64/vmexit/vmexit.c`

- [ ] **Step 1: Create `hypervisor/arch/arm64/vmexit/vmexit.c`**

```c
/* SPDX-License-Identifier: TBD */
#include <types.h>
#include <printk.h>
#include <vm.h>
#include <hypercall.h>

/* Defined in vmexit_asm.S; does not return */
extern void hv_restore(void);

static void handle_hvc(struct vcpu_regs *regs)
{
    u32 func_id = (u32)regs->x[0];
    u8  svc     = (u8)(func_id >> 24);

    if (svc == 0x84) {
        /* M2: psci_handle(regs); return; */
        regs->x[0] = SMCCC_NOT_SUPPORTED;
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
        /* ELR_EL2 already points past the HVC instruction */
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

- [ ] **Step 2: Compile check**

```bash
aarch64-none-linux-gnu-gcc \
  -ffreestanding -nostdlib -mgeneral-regs-only -mstrict-align \
  -Wall -Wextra -Werror -O2 -g \
  -Ihypervisor/include \
  -Ihypervisor/arch/arm64/board/qemu_virt \
  -Ihypervisor/arch/arm64/include \
  -c hypervisor/arch/arm64/vmexit/vmexit.c -o /tmp/vmexit_check.o
```

Expected: zero warnings.

- [ ] **Step 3: Commit**

```bash
git add hypervisor/arch/arm64/vmexit/vmexit.c
git commit -m "feat(m1): add HVC exit handler (vmexit.c)"
```

---

## Task 6: Context-switch assembly (`vmexit_asm.S`)

**Files:**
- Create: `hypervisor/arch/arm64/vmexit/vmexit_asm.S`

- [ ] **Step 1: Create `hypervisor/arch/arm64/vmexit/vmexit_asm.S`**

```asm
/* SPDX-License-Identifier: TBD */
/*
 * vmexit_asm.S — EL2↔EL1 context switch.
 *
 * vcpu_run(struct vcpu*)  — save HV callee-saved, load guest regs, eret to EL1
 * el1_sync_handler        — save guest regs, call handle_exit()
 * hv_restore()            — restore HV callee-saved, ret to vm_run()
 *
 * g_hv_ctx is defined here (BSS) — only accessed from this file.
 */

#include <vm.h>   /* VCPU_* and HV_* macros (active only in __ASSEMBLER__ block) */

    .section .bss
    .align 3
    .globl g_hv_ctx
g_hv_ctx:
    .zero HV_CTX_SIZE       /* 0x68 = 104 bytes */

/* ------------------------------------------------------------------ */
/*  vcpu_run(struct vcpu *vcpu)   x0 = vcpu*                          */
/* ------------------------------------------------------------------ */
    .section .text
    .globl vcpu_run
vcpu_run:
    /* 1. Save hypervisor callee-saved registers */
    adr_l   x9, g_hv_ctx
    stp     x19, x20, [x9, #0x00]
    stp     x21, x22, [x9, #0x10]
    stp     x23, x24, [x9, #0x20]
    stp     x25, x26, [x9, #0x30]
    stp     x27, x28, [x9, #0x40]
    str     x29,      [x9, #0x50]
    str     x30,      [x9, #HV_LR]
    mov     x10, sp
    str     x10,      [x9, #HV_SP]

    /* 2. Configure HCR_EL2 for this vCPU */
    ldr     x1, [x0, #VCPU_HCR_EL2]
    msr     hcr_el2, x1
    isb

    /* 3. Load guest system registers */
    ldr     x1, [x0, #VCPU_ELR]
    msr     elr_el2, x1
    ldr     x1, [x0, #VCPU_SPSR]
    msr     spsr_el2, x1
    ldr     x1, [x0, #VCPU_SP_EL1]
    msr     sp_el1, x1

    /* 4. Load guest GPRs (x0 last to preserve pointer) */
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
    ldr     x0,       [x0, #0x00]   /* x0 last */

    eret

/* ------------------------------------------------------------------ */
/*  el1_sync_handler — Lower EL AArch64 synchronous exception         */
/* ------------------------------------------------------------------ */
    .globl el1_sync_handler
el1_sync_handler:
    /* Save x0 on EL2 stack to free it as pointer */
    str     x0, [sp, #-16]!

    /* Load &g_vm.vcpu.regs (first field of first field = same address as g_vm) */
    adr_l   x0, g_vm

    /* Save x1–x30 (genuine guest values, untouched since exception fired) */
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

    /* Retrieve guest x0 from stack and save it */
    ldr     x1, [sp], #16
    str     x1, [x0, #0x00]

    /* Save guest system registers */
    mrs     x1, sp_el1
    str     x1, [x0, #VCPU_SP_EL1]
    mrs     x1, elr_el2
    str     x1, [x0, #VCPU_ELR]
    mrs     x1, spsr_el2
    str     x1, [x0, #VCPU_SPSR]

    /* handle_exit(struct vcpu_regs *regs, u64 esr) */
    mrs     x1, esr_el2
    bl      handle_exit

    /* Eret-back path (future milestones): restore and return to guest */
    adr_l   x0, g_vm
    ldr     x1, [x0, #VCPU_ELR]  ;  msr elr_el2, x1
    ldr     x1, [x0, #VCPU_SPSR] ;  msr spsr_el2, x1
    ldr     x1, [x0, #VCPU_SP_EL1]; msr sp_el1, x1
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
    eret

/* ------------------------------------------------------------------ */
/*  hv_restore() — restore hypervisor context; does not return        */
/* ------------------------------------------------------------------ */
    .globl hv_restore
hv_restore:
    adr_l   x0, g_hv_ctx
    ldr     x1,       [x0, #HV_SP]
    mov     sp, x1
    ldp     x19, x20, [x0, #0x00]
    ldp     x21, x22, [x0, #0x10]
    ldp     x23, x24, [x0, #0x20]
    ldp     x25, x26, [x0, #0x30]
    ldp     x27, x28, [x0, #0x40]
    ldr     x29,      [x0, #0x50]
    ldr     x30,      [x0, #HV_LR]
    ret
```

- [ ] **Step 2: Commit**

```bash
git add hypervisor/arch/arm64/vmexit/vmexit_asm.S
git commit -m "feat(m1): add vcpu_run/el1_sync_handler/hv_restore (vmexit_asm.S)"
```

---

## Task 7: Wire vectors, main.c, and Makefiles — full build

**Files:**
- Modify: `hypervisor/arch/arm64/boot/vectors.S`
- Modify: `hypervisor/boot/main.c`
- Modify: `hypervisor/arch/arm64/Makefile`

- [ ] **Step 1: Update `hypervisor/arch/arm64/boot/vectors.S`**

Change only the first entry in the `Lower EL, AArch64` block (line 24):

```asm
    /* Lower EL, AArch64 */
    VECTOR_ENTRY el1_sync_handler
    VECTOR_ENTRY panic_vector
    VECTOR_ENTRY panic_vector
    VECTOR_ENTRY panic_vector
```

- [ ] **Step 2: Update `hypervisor/boot/main.c`**

Add `#include <vm.h>` and the two calls after the banner block:

```c
/* SPDX-License-Identifier: TBD */
#include <types.h>
#include <uart.h>
#include <printk.h>
#include <board.h>
#include <vm.h>

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

    vm_init();
    vm_run();

    for (;;)
        cpu_wfi();
}
```

- [ ] **Step 3: Update `hypervisor/arch/arm64/Makefile`**

```make
arch-objs := \
    arch/arm64/boot/head.o \
    arch/arm64/boot/vectors.o \
    arch/arm64/cpu/cpu.o \
    arch/arm64/board/$(BOARD)/board.o \
    arch/arm64/mmu/stage2.o \
    arch/arm64/vmexit/vmexit.o \
    arch/arm64/vmexit/vmexit_asm.o

arch-includes := \
    -Ihypervisor/arch/arm64/board/$(BOARD) \
    -Ihypervisor/arch/arm64/include

arch-ldscript := hypervisor/arch/arm64/board/$(BOARD)/linker.lds
```

- [ ] **Step 4: Full build**

```bash
make
```

Expected: zero warnings. Objects present:
```
build/obj/arch/arm64/mmu/stage2.o
build/obj/arch/arm64/vmexit/vmexit.o
build/obj/arch/arm64/vmexit/vmexit_asm.o
build/obj/common/vm/vm.o
```

If build fails with `undefined reference`, check:
- `g_vm` is non-static in `vm.c` and `extern` in `vm.h`
- `el1_sync_handler` is `.globl` in `vmexit_asm.S`
- Both `vmexit.o` and `vmexit_asm.o` are in `arch-objs`

- [ ] **Step 5: readelf sanity check**

```bash
aarch64-none-linux-gnu-readelf -h build/hypervisor.elf | grep 'Entry point'
```

Expected:
```
  Entry point address:               0x40080000
```

- [ ] **Step 6: Commit**

```bash
git add hypervisor/arch/arm64/boot/vectors.S \
        hypervisor/boot/main.c \
        hypervisor/arch/arm64/Makefile
git commit -m "feat(m1): wire vmexit into vectors, main.c, and Makefiles"
```

---

## Task 8: `run-qemu.sh` update + final verification

**Files:**
- Modify: `scripts/run-qemu.sh`

- [ ] **Step 1: Update `scripts/run-qemu.sh`**

```sh
#!/bin/sh
set -eu
: "${SVM_BIN:?SVM_BIN must point to the SVM binary, e.g.: SVM_BIN=/path/to/svm.bin make run}"
exec qemu-system-aarch64 \
  -machine virt,virtualization=on,gic-version=3 \
  -cpu cortex-a72 -smp 1 -m 1G \
  -nographic -serial mon:stdio \
  -kernel build/hypervisor.elf \
  -device loader,file="${SVM_BIN}",addr=0x40200000 \
  ${QEMU_EXTRA_ARGS:-}
```

- [ ] **Step 2: Commit**

```bash
git add scripts/run-qemu.sh
git commit -m "feat(m1): add SVM_BIN loader to run-qemu.sh"
```

- [ ] **Step 3: Obtain or build a minimal SVM binary**

The SVM binary must be a raw AArch64 binary (not ELF) that:
1. At offset 0: starts executing (entry IPA = `0x40200000`).
2. Writes a message to UART: poll `*(u32*)0x09000018` (FR register) until bit 5 (TXFF) is clear, then write each byte to `*(u32*)0x09000000` (DR register).
3. Executes `hvc #0` with `x0 = 0x80000001`.
4. Falls into `wfi`.

A minimal example (build with `aarch64-none-linux-gnu-gcc -nostdlib -nostartfiles -T`):

```c
/* svm_main.c — minimal EL1 SVM payload for M1 verification */
#define UART_DR  ((volatile unsigned int *)0x09000000)
#define UART_FR  ((volatile unsigned int *)0x09000018)

static void uart_puts(const char *s) {
    while (*s) {
        while (*UART_FR & (1 << 5)) {}
        *UART_DR = (unsigned int)*s++;
    }
}

void svm_start(void) {
    uart_puts("[svm] Hello from EL1\n");
    __asm__ volatile(
        "mov x0, #0x80000001\n"
        "hvc #0\n"
        ::: "x0"
    );
    for (;;) __asm__ volatile("wfi");
}
```

Linker script (`svm.lds`):
```
ENTRY(svm_start)
SECTIONS {
    . = 0x40200000;
    .text : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(.bss*) *(COMMON) }
}
```

Build:
```bash
aarch64-none-linux-gnu-gcc -ffreestanding -nostdlib -nostartfiles \
  -mgeneral-regs-only -O2 \
  -T svm.lds svm_main.c -o svm.elf
aarch64-none-linux-gnu-objcopy -O binary svm.elf svm.bin
```

- [ ] **Step 4: Run functional verification**

```bash
cd /home/corsair/Music/virtual/hyp-/hypervisor-
SVM_BIN=/path/to/svm.bin make run
```

Expected output (exact order):
```
  H   H Y   Y PPPP  EEEEE RRRR  V   V IIIII SSSSS  OOO  RRRR
  ...banner lines...

[hv] Hello from EL2 on qemu_virt, CurrentEL=0x8
[hv] SVM: launching VMID=1 entry=0x40200000
[svm] Hello from EL1
[hv] SVM HVC: done (x1=0x0)
```

Then QEMU halts (hypervisor in `wfi`). Press `Ctrl-A x` to exit.

- [ ] **Step 5: Stage-2 isolation test**

Temporarily add a bogus memory access in `svm_main.c` before `uart_puts`:
```c
volatile unsigned int *bad = (volatile unsigned int *)0x50000000;
(void)*bad;   /* accesses unmapped IPA → Stage-2 Data Abort */
```

Rebuild SVM, re-run. Expected: hypervisor prints:
```
[hv] unexpected exit EC=0x24 ESR=0x...
```
Then hangs (wfi). Revert the SVM source and rebuild.

- [ ] **Step 6: Exit QEMU cleanly**

`Ctrl-A x` — no zombie process:
```bash
pgrep qemu-system-aarch64   # should return nothing
```

---

## Self-Review

**Spec coverage:**
- §3 (flow) → Tasks 4, 6, 7
- §5 (Stage-2) → Task 3
- §6 (structures) → Task 2
- §7 (context switch) → Task 6
- §8 (HVC dispatch) → Task 5
- §9 (board constants) → Task 1
- §11 (verification) → Task 8

**Name collision resolved:** `vmexit.S` renamed to `vmexit_asm.S` throughout to avoid object-file conflict with `vmexit.c` under the Makefile's `%.o: %.S` / `%.o: %.c` pattern rules.

**`g_hv_ctx` visibility:** Defined as `.globl` in BSS of `vmexit_asm.S`. Not declared in any C header — exclusively accessed from assembly.

**`g_vm` visibility:** Non-static in `vm.c`; declared `extern struct vm g_vm` in `vm.h` — C and assembly both access it cleanly.

**SPSR_EL2 = 0x3C5:** EL1h (M=0b00101=5) + DAIF masked (bits[9:6]=0b1111=0x3C0). Sum = 0x3C5. ✓
