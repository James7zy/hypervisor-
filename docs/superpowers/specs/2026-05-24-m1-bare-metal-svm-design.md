# Hypervisor — M1 (Bare-Metal SVM) Design

- **Date**: 2026-05-24
- **Project**: `hypervisor-`
- **Milestone**: M1 — Bare-Metal SVM
- **Target platform**: QEMU `virt` (AArch64), GICv3, PL011 UART, Cortex-A72
- **Status**: Design approved, ready for implementation planning

---

## 1. Purpose & Positioning

M1 adds the minimal viable virtualization path on top of M0's EL2 foundation:
Stage-2 MMU, a single static vCPU, and the ability to launch and trap from a
Service VM (SVM) running at EL1.

The SVM binary is external to this repository. It is loaded into physical
memory by QEMU (`-device loader`) before the hypervisor starts. The hypervisor
assumes the SVM has been placed at `BOARD_SVM_ENTRY` and jumps to it after
Stage-2 is active.

### Roadmap context

| Milestone | Goal |
|---|---|
| **M0 — Hello EL2** *(done)* | EL2 entry, PL011 UART, banner, wfi |
| **M1 — Bare-Metal SVM** *(this spec)* | Stage-2 MMU, single vCPU, EL1 SVM launch, HVC |
| M2 — Multi-vCPU + Interrupts | vGICv3, virtual timer, PSCI |
| M3 — Linux Guest | Boot Linux to shell, virtio-console |
| M4 — RK3588 Port | Run on real RK3588 hardware |

---

## 2. Scope

### 2.1 In scope (M1)

- Stage-2 address translation: flat identity map, two 1 GB L1 block entries.
- Static `struct vm` / `struct vcpu` / `struct vm_config` in BSS (no allocator).
- `vcpu_run()` assembly: save HV context → set HCR_EL2 → `eret` to EL1 SVM.
- `el1_sync_handler` assembly: save guest context → call C `handle_exit`.
- HVC dispatch: SMCCC-style, PSCI slot pre-reserved, `HC_GUEST_DONE` handled.
- `hv_restore()` assembly: restore HV context after `HC_GUEST_DONE`, return to `main.c`.
- `run-qemu.sh` update: load SVM binary via `-device loader` using `$SVM_BIN`.
- Build system: no `guests/` subtree; `SVM_BIN` is an external variable.

### 2.2 Out of scope (M1)

- GIC initialisation (M2).
- Virtual timer (M2).
- PSCI implementation (M2; dispatch slot reserved, returns `NOT_SUPPORTED`).
- Multiple vCPUs or multiple VMs (M2+).
- SVM binary in this repository (external, loaded by QEMU).
- Stage-2 fault handler beyond panic (unmapped IPA → `panic_vector`).
- EL2 MMU / Stage-1 (unchanged from M0).
- FP/SIMD context save/restore (`-mgeneral-regs-only` remains mandatory).

---

## 3. Architecture

### 3.1 End-to-end flow

```
main.c
  vm_run(&g_vm)
    │ stage2_activate()           ← write VTCR_EL2 / VTTBR_EL2 / isb
    │ vcpu_run(&g_vm.vcpu)        ← assembly
    │   ① save HV callee-saved → g_hv_ctx
    │   ② msr HCR_EL2, vcpu->hcr_el2
    │   ③ restore ELR_EL2 / SPSR_EL2 / SP_EL1 from vcpu->regs
    │   ④ restore x0–x30 from vcpu->regs
    │   ⑤ eret ──────────────────────────────────► EL1 SVM
    │                                                  │ [SVM prints to UART]
    │                                                  │ hvc #0  x0=HC_GUEST_DONE
    │◄── el1_sync_handler (vector offset +0x400) ──────┘
    │   ⑥ save guest x0–x30 / SP_EL1 / ELR_EL2 / SPSR_EL2
    │   ⑦ bl handle_exit(regs, esr_el2)
    │       EC=0x16 → handle_hvc(regs)
    │           HC_GUEST_DONE → printk → hv_restore()
    │               restore g_hv_ctx (SP / x19–x29 / LR)
    │               ret ──────────────────────────────► return to vm_run()
    └─ for(;;) cpu_wfi()
```

### 3.2 GIC policy

M1 does **not** initialise the GIC. Two settings prevent physical interrupts
from reaching the SVM or hitting the panic vector during the brief EL1 window:

1. `HCR_EL2.IMO=1 | FMO=1 | AMO=1` — route physical IRQ/FIQ/SError to EL2.
2. `SPSR_EL2[9:6] = 0b1111` at guest entry — SVM runs with DAIF masked.

EL2 itself keeps DAIF masked from M0's `daifset #0xF` and is never unmasked in M1.

### 3.3 HCR_EL2 value for SVM vCPU

| Bit | Field | Value | Reason |
|---|---|---|---|
| 0 | VM | 1 | Enable Stage-2 translation |
| 3 | FMO | 1 | Route physical FIQ to EL2 |
| 4 | IMO | 1 | Route physical IRQ to EL2 |
| 5 | AMO | 1 | Route SError to EL2 |
| 29 | HCD | 0 | Allow HVC from EL1 |

All other bits zero for M1.

---

## 4. New Files

```
hypervisor/
├── arch/arm64/
│   ├── mmu/
│   │   ├── stage2.c      ← VTCR_EL2, L1 table fill, VTTBR_EL2
│   │   └── stage2.h      ← stage2_init() / stage2_activate()
│   └── vmexit/
│       ├── vmexit.S      ← vcpu_run, el1_sync_handler, hv_restore
│       └── vmexit.c      ← handle_exit(), handle_hvc()
│
├── common/vm/
│   ├── vm.c              ← vm_init(), vm_run()
│   ├── vm.h              ← struct vcpu_regs/vcpu/vm + ASM offset macros
│   └── vm_config.h       ← struct vm_config; static svm_config
│
└── include/
    └── hypercall.h       ← SMCCC constants, HC_GUEST_DONE
```

### Modified existing files

| File | Change |
|---|---|
| `arch/arm64/boot/vectors.S` | Offset +0x400: `b panic_vector` → `b el1_sync_handler` |
| `arch/arm64/board/qemu_virt/board.h` | Add `BOARD_SVM_ENTRY`, `BOARD_SVM_MEM_BASE`, `BOARD_SVM_MEM_SIZE` |
| `hypervisor/boot/main.c` | After banner: call `vm_init()` then `vm_run()` |
| `scripts/run-qemu.sh` | Add `-device loader,file=${SVM_BIN:?},addr=0x40200000` |
| Top-level `Makefile` | Validate `SVM_BIN` is set before `make run`; no `guests/` subtarget |

---

## 5. Stage-2 Page Tables

### 5.1 Translation regime

| Parameter | Value | Notes |
|---|---|---|
| Granule | 4 KB (`TG0=00`) | Consistent with future Linux guest |
| IPA width | 39-bit (`T0SZ=25`) | Clean 9-bit L1 index, 512 entries |
| Start level | Level 1 (`SL0=01`) | 1 GB per L1 entry |
| PA size | 40-bit (`PS=010`) | Covers DRAM at `0x40000000` |
| Cacheability | WB RA-WA (`IRGN0=ORGN0=01`) | Normal memory |
| Shareability | Inner Shareable (`SH0=11`) | Normal memory |

`VTCR_EL2_VALUE` is assembled from the above in a single `#define` in `stage2.c`.

### 5.2 L1 table layout

Static 512×u64 array in BSS; only two entries are valid:

| Index | IPA range | PA | Attributes | Reason |
|---|---|---|---|---|
| L1[0] | `0x00000000`–`0x3FFFFFFF` | `0x00000000` | Device-nGnRE, XN, RW | Covers UART @ `0x09000000` |
| L1[1] | `0x40000000`–`0x7FFFFFFF` | `0x40000000` | Normal WB, Inner Shareable, RW | Covers all DRAM (HV + SVM) |
| L1[2..511] | — | — | 0 (invalid) | Stage-2 abort → `panic_vector` |

### 5.3 VTTBR_EL2

```
VTTBR_EL2[63:48] = VMID = 1        (SVM; VMID=0 reserved for HV)
VTTBR_EL2[47:1]  = L1 table PA     (static BSS array; identity-mapped → addr = PA)
VTTBR_EL2[0]     = CnP = 0         (single-core M1)
```

### 5.4 API

```c
void stage2_init(void);      /* fill L1 table, compute VTCR/VTTBR — no hardware write */
void stage2_activate(void);  /* write VTCR_EL2, VTTBR_EL2, isb — Stage-2 goes live */
```

`stage2_init` is called from `vm_init()`; `stage2_activate` is called from `vm_run()`
just before `vcpu_run()`, minimising the window between activation and guest entry.

---

## 6. VM / vCPU Structures

### 6.1 `struct vcpu_regs` (assembly-visible; field order is an ABI)

```c
struct vcpu_regs {
    u64 x[31];      /* x0–x30  offset 0x000 */
    u64 sp_el1;     /*          offset 0x0F8 */
    u64 elr_el2;    /*          offset 0x100 */
    u64 spsr_el2;   /*          offset 0x108 */
};                  /* total 0x110 = 272 bytes */
```

Assembly offset macros in `vm.h`, guarded by `#ifdef __ASSEMBLER__`:

```c
/* struct vcpu_regs offsets */
#define VCPU_X0          0x000
#define VCPU_SP_EL1      0x0F8
#define VCPU_ELR         0x100
#define VCPU_SPSR        0x108

/* struct vcpu offsets (regs is first, so hcr_el2 follows at sizeof(vcpu_regs)) */
#define VCPU_HCR_EL2     0x110
#define VCPU_VTTBR_EL2   0x118

/* struct hv_ctx offsets */
#define HV_X19           0x000   /* x19–x29: 11 × 8 = 0x58 bytes */
#define HV_LR            0x058
#define HV_SP            0x060
```

### 6.2 `struct vcpu`

```c
struct vcpu {
    struct vcpu_regs regs;   /* MUST be first: vmexit.S casts vcpu* to vcpu_regs* */
    u64 hcr_el2;
    u64 vttbr_el2;
};
```

### 6.3 `struct vm`

```c
struct vm {
    struct vcpu          vcpu;
    const struct vm_config *config;
};
```

### 6.4 `struct vm_config` (vm_config.h)

```c
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
```

### 6.5 `struct hv_ctx` (hypervisor return context)

```c
struct hv_ctx {
    u64 x19, x20, x21, x22, x23, x24, x25, x26, x27, x28, x29;
    u64 lr;    /* return address inside vm_run() */
    u64 sp;    /* hypervisor stack pointer */
};
```

### 6.6 Static allocation

```c
/* vm.c */
static struct vm     g_vm;
static struct hv_ctx g_hv_ctx;

struct vm *vm_get_svm(void) { return &g_vm; }
```

Total BSS footprint: `struct vm` ≈ 296 B, `struct hv_ctx` = 104 B, L1 table = 4 KB.

---

## 7. Context Switch

### 7.1 `vcpu_run(struct vcpu *vcpu)` — assembly

```
1. adr_l x9, g_hv_ctx
   stp x19,x20, [x9, ...]      save x19–x29
   str x30,     [x9, #HV_LR]   save return address
   mov x10, sp
   str x10,     [x9, #HV_SP]   save hypervisor SP

2. ldr x1, [x0, #VCPU_HCR_EL2]
   msr hcr_el2, x1 ; isb

3. ldr x1, [x0, #VCPU_ELR]  ; msr elr_el2, x1
   ldr x1, [x0, #VCPU_SPSR] ; msr spsr_el2, x1
   ldr x1, [x0, #VCPU_SP_EL1]; msr sp_el1, x1

4. ldp x2,x3, [x0, #VCPU_X0+16]  ... restore x2–x30
   ldr x1,    [x0, #VCPU_X0+8]
   ldr x0,    [x0, #VCPU_X0]      x0 last (overwrites vcpu pointer)

5. eret
```

### 7.2 `el1_sync_handler` — assembly (vectors.S offset +0x400)

```
1. str x0, [sp, #-8]!            save guest x0 onto EL2 stack
   adr_l x0, g_vm                x0 = &vcpu_regs (first field of first field)
   ldr x1, [sp], #8
   str x1, [x0, #VCPU_X0]        store guest x0

2. stp x1,x2, [x0, #VCPU_X0+8]  ... store x1–x30
   str x30, [x0, #VCPU_X0+240]

3. mrs x1, sp_el1  ; str x1, [x0, #VCPU_SP_EL1]
   mrs x1, elr_el2 ; str x1, [x0, #VCPU_ELR]
   mrs x1, spsr_el2; str x1, [x0, #VCPU_SPSR]

4. mrs x1, esr_el2
   bl  handle_exit                handle_exit(vcpu_regs*, esr)

5. adr_l x0, g_vm
   ... restore guest regs from vcpu_regs ...
   eret                           eret-back path (future use)
```

### 7.3 `hv_restore()` — assembly (called from C, does not return)

```
1. adr_l x0, g_hv_ctx
2. ldr x10, [x0, #HV_SP] ; mov sp, x10
3. ldp x19,x20, [x0, ...]  ... restore x19–x29
4. ldr x30, [x0, #HV_LR]
5. ret                      returns into vm_run() → main.c for(;;) wfi
```

---

## 8. HVC Dispatch

### 8.1 Constants (`hypercall.h`)

```c
#define SMCCC_NOT_SUPPORTED   (~0ULL)
#define HVC_VENDOR_BASE        0x80000000U
#define HC_GUEST_DONE         (HVC_VENDOR_BASE | 0x0001U)
```

### 8.2 `handle_exit` (vmexit.c)

```c
void handle_exit(struct vcpu_regs *regs, u64 esr) {
    u32 ec = (u32)(esr >> 26) & 0x3F;
    switch (ec) {
    case 0x16:   /* HVC from AArch64 EL1 */
        handle_hvc(regs);
        return;
    default:
        printk("[hv] unexpected exit EC=0x%x ESR=0x%lx ELR=0x%lx\n",
               ec, esr, regs->elr_el2);
        for (;;) cpu_wfi();
    }
}
```

### 8.3 `handle_hvc` (vmexit.c)

```c
void handle_hvc(struct vcpu_regs *regs) {
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
    default:
        printk("[hv] HVC: unknown func_id=0x%x\n", func_id);
        regs->x[0] = SMCCC_NOT_SUPPORTED;
        /* ELR_EL2 already points past HVC instruction */
        return;
    }
}
```

Note: `ELR_EL2` is not incremented on HVC — the ARM architecture guarantees
ELR_EL2 points to the instruction after the HVC when the exception is taken.

---

## 9. Board Constants (qemu_virt/board.h additions)

```c
/* SVM interface contract — stable ABI between hypervisor and external SVM binary */
#define BOARD_SVM_ENTRY    0x40200000UL   /* SVM entry IPA (= PA, flat map) */
#define BOARD_SVM_MEM_BASE 0x40200000UL   /* SVM memory region IPA base */
#define BOARD_SVM_MEM_SIZE 0x00200000UL   /* SVM memory region size: 2 MB */
```

---

## 10. Expected Output

```
  H   H Y   Y PPPP  EEEEE RRRR  V   V IIIII SSSSS  OOO  RRRR
  ... (M0 ASCII banner) ...

[hv] Hello from EL2 on qemu_virt, CurrentEL=0x8
[hv] SVM: launching VMID=1 entry=0x40200000
<SVM output, e.g.: [svm] Hello from EL1, CurrentEL=0x4>
[hv] SVM HVC: done (x1=0x0)
```

---

## 11. Verification Checklist

- [ ] `make` succeeds with zero warnings (`-Werror`).
- [ ] `readelf -h build/hypervisor.elf` — entry still `0x40080000`.
- [ ] `SVM_BIN=/path/to/svm.bin make run` — terminal shows all four output lines (§10).
- [ ] SVM output shows `CurrentEL=0x4` (EL1, not EL2).
- [ ] Final hypervisor line is `[hv] SVM HVC: done`.
- [ ] **Stage-2 isolation**: temporarily make SVM access IPA `0x50000000` (unmapped) →
      hypervisor prints `unexpected exit EC=...` and halts. Revert after verifying.
- [ ] `Ctrl-A x` exits QEMU cleanly.

---

## 12. Forward-Compatibility Contracts for M2

| What | Location | M2 use |
|---|---|---|
| `svc == 0x84` slot in `handle_hvc` | `vmexit.c` | Replace comment with `psci_handle(regs)` |
| `VMID=1` in `svm_config` | `vm_config.h` | Extend to array; VMID=2 for second VM |
| `struct vm` / `struct vcpu` layout | `vm.h` | Add fields; `regs`-first invariant preserved |
| `HCR_EL2` value in `vcpu` | `vm.c` | Add `TWI=1` for WFI trap, vGIC bits |
| `g_hv_ctx` global | `vmexit.S` | Becomes per-CPU when SMP arrives |

End of M1 design.
