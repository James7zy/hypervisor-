# Hypervisor — M2 (Multi-vCPU Foundations: Interrupts + PSCI) Design

- **Date**: 2026-05-30
- **Project**: `hypervisor-`
- **Milestone**: M2 — vGICv3, virtual timer, PSCI
- **Target platform**: QEMU `virt` (AArch64), GICv3, PL011 UART, Cortex-A72
- **Status**: Design approved, ready for implementation planning
- **Implementation plan**: `docs/superpowers/plans/2026-05-30-hypervisor-m2.md`

---

## 1. Purpose & Positioning

M2 makes the single M1 vCPU **interrupt-capable** without yet adding a second
CPU. It introduces the three subsystems that every subsequent milestone depends
on: a physical GICv3 owned by the hypervisor, a virtual GICv3 CPU interface
(vGICv3) that injects virtual interrupts into the guest, and a PSCI dispatcher.

The end-to-end demonstration is a complete virtual-timer interrupt cycle: the
SVM arms its EL1 virtual timer, the physical PPI 27 fires and traps to EL2 (via
`HCR_EL2.IMO`), the hypervisor injects a pending virtual IRQ via `ICH_LR0_EL2`,
and the guest's own EL1 IRQ handler runs after `eret`. The SVM then queries PSCI
and signals completion via the existing `HC_GUEST_DONE` HVC.

The SVM test payload (`tests/svm2/`) is in-repo here (unlike M1's external SVM)
because it exercises GIC/timer behaviour that must be co-versioned with the
hypervisor's injection contract.

### Roadmap context

| Milestone | Goal |
|---|---|
| M0 — Hello EL2 *(done)* | EL2 entry, PL011 UART, banner, wfi |
| M1 — Bare-Metal SVM *(done)* | Stage-2 MMU, single vCPU, EL1 SVM launch, HVC |
| **M2 — Interrupts + PSCI** *(this spec)* | Physical GICv3, vGICv3 injection, virtual timer, PSCI dispatch |
| M3 — Linux Guest | Boot Linux to shell, virtio-console; PSCI `CPU_ON` + SMP |
| M4 — RK3588 Port | Run on real RK3588 hardware |

---

## 2. Scope

### 2.1 In scope (M2)

- **Physical GICv3 init**: GICD (affinity routing + Group 1 NS), CPU0
  redistributor wake, PPI 27 configured Group 1 NS at priority `0xA0`,
  `ICC_SRE_EL2` enabled so the EL2 CPU interface can ack/EOI.
- **vGICv3 CPU interface**: per-vCPU `ICH_HCR_EL2`, `ICH_VMCR_EL2`, and
  `ICH_LR0..LR3` save/restore; single-LR injection (`ICH_LR0_EL2`) of one
  pending Group-1 virtual interrupt.
- **EL2 IRQ trap-and-inject path**: physical IRQ taken at EL2 → ack → inject
  virtual IRQ → physical EOI → `eret` so the vGIC delivers it to the guest.
- **DAIF.I lifecycle**: EL2 IRQs unmasked only on the guest-entry `eret` paths.
- **Virtual timer plumbing**: `CNTHCTL_EL2` allows EL1 physical counter/timer
  access; `CNTVOFF_EL2 = 0` (no time skew). The timer itself is armed by the
  guest, not the hypervisor.
- **PSCI dispatcher**: `VERSION`, `FEATURES`, `CPU_OFF`, `SYSTEM_OFF`,
  `SYSTEM_RESET`; `CPU_ON` reserved (returns `NOT_SUPPORTED`).
- **In-repo SVM2 test payload** demonstrating the full cycle.

### 2.2 Out of scope (M2)

- **Multi-vCPU / SMP** — only one vCPU. `g_vm`, `g_hv_ctx`, and the GIC CPU0
  redistributor frame are single-instance (M3 makes them per-CPU).
- **PSCI `CPU_ON`** — dispatch slot returns `NOT_SUPPORTED` (M3 SMP bring-up).
- **Full vGIC distributor emulation** — no trapped GICD/GICR MMIO emulation; the
  guest drives its own CPU interface (`ICC_*`) directly, and only PPI 27 is
  wired. SPI routing and IPI/SGI emulation are deferred.
- **List-register overflow / maintenance interrupts** — one in-flight virtual
  interrupt at a time; `ICH_LR1..LR3` are saved/restored but unused.
- **Timer offset virtualization / multiple timers** — only the EL1 virtual
  timer, `CNTVOFF_EL2 = 0`.
- **FP/SIMD context** — `-mgeneral-regs-only` remains mandatory.

---

## 3. Architecture

### 3.1 End-to-end flow

```
main.c
  gic_init()                       ← physical GICv3: GICD, GICR CPU0, ICC_SRE_EL2
  vtimer_init()                    ← CNTHCTL_EL2=3, CNTVOFF_EL2=0
  vm_init()                        ← HCR_EL2 (IMO=1), stage2_init, vgic_init
  vm_run()
    │ stage2_activate()
    │ vgic_restore()               ← load ICH_HCR/VMCR/LR* into EL2 sysregs
    │ vcpu_run(&g_vm.vcpu)         ← assembly
    │   restore guest ctx ; daifclr #2 ; eret ─────────► EL1 SVM
    │                                                       │ configures ICC_*,
    │                                                       │ arms cntv timer,
    │                                                       │ daifclr #2 ; wfi
    │                                  ┌── PPI 27 fires ────┘
    │◄── el1_irq_handler_asm (+0x480) ─┤  (taken at EL2 via HCR_EL2.IMO=1)
    │   save guest ctx                 │
    │   bl el2_irq_handler             │
    │     intid = gic_ack_irq()        │
    │     vgic_inject(LR0, INTID 27)   │  pending Group-1 vIRQ
    │     gic_eoi_irq(intid)           │
    │   restore guest ctx ; daifclr #2 ; eret ──────────► EL1 SVM
    │                                                       │ vGIC delivers vIRQ
    │                                                       │ → svm2_irq_handler
    │                                                       │   ack/EOI, disarm
    │                                                       │ hvc HC_GUEST_DONE
    │◄── el1_sync_handler (+0x400) ─────────────────────────┘
    │   handle_exit → handle_hvc → HC_GUEST_DONE → hv_restore()
    └─ returns to vm_run() → main.c for(;;) wfi
```

### 3.2 Interrupt model: trap-and-inject via list registers

M2 uses the GICv3 **list-register** injection model rather than full
distributor emulation:

1. All physical IRQs route to EL2 (`HCR_EL2.IMO=1`, inherited from M1).
2. The hypervisor acks the physical interrupt at its own EL2 CPU interface
   (`ICC_IAR1_EL1`), decides which virtual INTID to deliver, and writes a
   single pending Group-1 entry into `ICH_LR0_EL2`.
3. The hypervisor performs the **physical** priority-drop + deactivate
   (`ICC_EOIR1_EL1` then `ICC_DIR_EL1`) so the physical interrupt does not
   re-fire.
4. On `eret`, the vGIC presents the list-register entry to the guest's virtual
   CPU interface; the guest acks/EOIs it through its own `ICC_*` registers.

**Why list registers, not trapped MMIO emulation:** for a single PPI the LR path
is a few register writes with no fault-decode machinery, it matches how Linux's
KVM and Xvisor drive GICv3, and it scales cleanly to M3 (more LRs, maintenance
interrupts) without re-architecting. Trapped GICD/GICR emulation is only needed
once a guest must see a virtual *distributor* — deferred to when a Linux guest
requires it.

**Single-LR limitation:** exactly one virtual interrupt may be in flight.
Acceptable for M2's one-PPI demonstration; `ICH_LR1..LR3` are saved/restored so
M3 can fan out without an ABI change.

### 3.3 DAIF.I lifecycle (correctness argument)

Physical IRQs must be masked at EL2 *except* across the guest-entry `eret`, so
that an IRQ fired while the guest runs is the only thing that re-enters EL2
through the IRQ vector.

1. `head.S` (M0): `daifset #0xF` — all masked on EL2 boot; never globally
   unmasked.
2. `vcpu_run` (guest entry): restore guest ctx → **`daifclr #2`** → `eret`.
3. Guest runs with EL2 IRQs unmasked; PPI 27 fires → `el1_irq_handler_asm`.
   Exception entry **auto-sets `PSTATE.I`** (EL2 IRQs masked again).
4. IRQ handler: inject vIRQ → **`daifclr #2`** → `eret` back to guest with the
   pending virtual interrupt.
5. Guest HVC → `el1_sync_handler`. Exception entry auto-masks `PSTATE.I`.
   `hv_restore` returns to `vm_run()` with EL2 IRQs **still masked** — the
   hypervisor's `wfi` loop runs with interrupts masked, as in M0/M1.

Net: EL2 IRQs are unmasked only on the two `eret`-to-guest edges; the hypervisor
itself never runs with IRQs unmasked.

### 3.4 HCR_EL2 value for the SVM vCPU

Unchanged from M1 — M1 already routed physical IRQ/FIQ/SError to EL2, which is
exactly what the M2 inject path needs. No new HCR bits.

| Bit | Field | Value | Reason |
|---|---|---|---|
| 0 | VM | 1 | Stage-2 translation |
| 3 | FMO | 1 | Route physical FIQ to EL2 |
| 4 | IMO | 1 | **Route physical IRQ to EL2 — drives the M2 inject path** |
| 5 | AMO | 1 | Route SError to EL2 |
| 31 | RW | 1 | EL1 executes AArch64 |
| 29 | HCD | 0 | Allow HVC from EL1 |

---

## 4. New Files

```
hypervisor/
├── arch/arm64/
│   ├── irq/
│   │   ├── gic_v3.h            ← GICD/GICR MMIO macros, gic_init/ack/eoi API
│   │   ├── gic_v3.c            ← physical GICv3 init, ack, EOI
│   │   ├── vgic.h             ← ICH_LR layout macros, vgic_* API
│   │   ├── vgic.c             ← ICH_HCR/VMCR/LR save/restore/inject
│   │   ├── irq_handler.c      ← el2_irq_handler(): ack → inject → EOI
│   │   └── irq_handler_asm.S  ← el1_irq_handler_asm: save/restore + daifclr
│   └── timer/
│       ├── vtimer.h           ← vtimer_init() declaration
│       └── vtimer.c           ← CNTHCTL_EL2, CNTVOFF_EL2
│
├── common/psci/
│   ├── psci.h                 ← PSCI function IDs, return codes
│   └── psci.c                 ← psci_handle() dispatcher
│
tests/svm2/
├── svm2_vectors.S             ← EL1 vector table (IRQ at +0x280)
├── svm2_main.c                ← arm timer, wait, handle IRQ, HVC done
└── svm2.lds                   ← entry 0x40200000, VBAR-aligned vectors
tests/run_svm2_test.sh         ← build SVM2 + make run
```

### Modified existing files

| File | Change |
|---|---|
| `arch/arm64/include/asm/sysreg.h` | Add `ICC_*`, `ICH_*`, `CNTHCTL_EL2`, `CNTVOFF_EL2` `S<op>` aliases |
| `arch/arm64/board/qemu_virt/board.h` | Add `BOARD_GIC_DIST_BASE`, `BOARD_GIC_RDIST_BASE`, `BOARD_VTIMER_IRQ` |
| `arch/arm64/boot/vectors.S` | Offset +0x480 (Lower EL AArch64 IRQ): `panic_vector` → `el1_irq_handler_asm` |
| `arch/arm64/vmexit/vmexit_asm.S` | Add `msr daifclr, #2` before the guest-entry `eret` |
| `arch/arm64/vmexit/vmexit.c` | `handle_hvc`: PSCI `svc 0x84/0xC4` slot calls `psci_handle()` |
| `include/vm.h` | Extend `struct vcpu` with vGIC fields (append-only) |
| `common/vm/vm.c` | `vgic_init()` in `vm_init()`; `vgic_restore()` in `vm_run()` |
| `arch/arm64/Makefile` | Add `irq/` + `timer/` objects; extend `arch-includes` |
| `Makefile` (hv) | Add `common/psci/psci.o`; extend `hv-includes` |

---

## 5. Physical GICv3

### 5.1 Initialisation sequence (`gic_init`)

1. **Distributor**: `GICD_CTLR = ARE_NS | ENGRP1NS`, then poll `RWP` clear.
2. **Redistributor (CPU0)**: clear `GICR_WAKER.ProcessorSleep`, poll
   `ChildrenAsleep` clear.
3. **PPI 27**: set Group 1 (`GICR_IGROUPR0`), Non-Secure
   (`GICR_IGRPMODR0` clear), priority `0xA0` (`GICR_IPRIORITYR`, byte-addressed
   inside a word: index `27/4=6`, byte `27%4=3`, shift 24), enable
   (`GICR_ISENABLER0` bit 27).
4. **EL2 CPU interface**: `ICC_SRE_EL2 = 0xF` (SRE|DIL|DFB|Enable), `isb`.
   The `Enable` bit is the prerequisite for the guest's later `ICC_SRE_EL1`
   write to succeed without trapping.
5. **EL2 priority/group**: `ICC_PMR_EL1 = 0xFF` (allow all), `ICC_IGRPEN1_EL1 =
   1`, `isb` — required for EL2 to ack/EOI.

### 5.2 Ack / EOI policy

`gic_ack_irq()` reads `ICC_IAR1_EL1` and masks to bits[23:0] (INTID).
`gic_eoi_irq()` does the **split EOI**: `ICC_EOIR1_EL1` (priority drop) then
`ICC_DIR_EL1` (deactivate). The split (rather than combined) form is used so the
hypervisor's physical deactivate is explicit and independent of the guest's
own virtual EOI later — this keeps the physical and virtual interrupt
lifecycles cleanly separated.

### 5.3 Board constants (`qemu_virt/board.h`)

```c
#define BOARD_GIC_DIST_BASE  0x08000000UL   /* GICD */
#define BOARD_GIC_RDIST_BASE 0x080A0000UL   /* GICR CPU0 RD frame */
#define BOARD_VTIMER_IRQ     27U            /* EL1 virtual timer PPI INTID */
```

Redistributor frames: RD at `+0x00000`, SGI at `+0x10000`.

---

## 6. vGICv3 (virtual CPU interface)

### 6.1 Per-vCPU state

The vGIC hypervisor view is saved/restored per vCPU so that M3's scheduler can
context-switch interrupt state. State held in `struct vcpu`:

- `ich_hcr_el2` — `En=1`, enabling the virtual CPU interface.
- `ich_vmcr_el2` — `0` at init; the guest programs `VPMR`/`VENG1` indirectly via
  its own `ICC_PMR_EL1`/`ICC_IGRPEN1_EL1` writes, which the vGIC mirrors.
- `ich_lr[4]` — `ICH_LR0..LR3`; only `LR0` is used in M2.

### 6.2 Injection (`vgic_inject`)

Build one list-register entry and write `ICH_LR0_EL2`:

```
ICH_LR0_EL2 = State(Pending=01)<<62 | Group1(1)<<60 | prio<<48 | vINTID
```

No `isb` after the LR write — the guest-entry `eret` in the caller synchronises
virtual interrupt delivery. (Documented inline so it is not "fixed" later.)

### 6.3 Save / restore lifecycle

- `vgic_init(vcpu)` — set initial `ich_hcr_el2=1`, zero VMCR + LRs, and write
  them through to the EL2 sysregs.
- `vgic_restore(vcpu)` — called from `vm_run()` before `vcpu_run()`; loads the
  saved hypervisor-view registers into `ICH_*`.
- `vgic_save(vcpu)` — reads `ICH_*` back into the struct. Defined for M3
  scheduling; M2's single-run flow does not strictly require it, but it is part
  of the stable API.

### 6.4 `ICH_LR` bit layout (reference)

| Bits | Field | M2 value |
|---|---|---|
| 63:62 | State | `01` Pending |
| 61 | HW | `0` software-generated |
| 60 | Group | `1` Group 1 |
| 55:48 | Priority | `0xA0` |
| 31:0 | vINTID | 27 |

---

## 7. Virtual Timer

The hypervisor does **not** arm or own the timer; it only removes the EL1 traps
so the guest can drive its own EL1 virtual timer.

| Register | Value | Reason |
|---|---|---|
| `CNTHCTL_EL2` | `0x3` (`EL1PCTEN`\|`EL1PCEN`) | Let EL1/EL0 read `CNTPCT_EL0` and access physical timer regs without trapping to EL2 (`EC=0x18`) |
| `CNTVOFF_EL2` | `0` | Virtual count == physical count; guest `CNTVCT_EL0` has no skew |

The SVM arms `CNTV_TVAL_EL0` / `CNTV_CTL_EL0` itself. When the timer expires it
raises PPI 27 physically, which is the entry point to the §3.2 inject path.

Reachability note: `vtimer_init()` **must** run before `vm_run()`. If it does
not, the guest's first `CNTPCT_EL0` read traps as `EC=0x18` and the run aborts —
called out as a known failure mode for the implementer.

---

## 8. PSCI

### 8.1 Dispatch entry

`handle_hvc` recognises PSCI by the SMCCC service field: `func_id>>24 == 0x84`
(32-bit) or `0xC4` (64-bit) routes to `psci_handle()`. This *replaces* the M1
forward-compat stub that returned `SMCCC_NOT_SUPPORTED` for `svc==0x84` — the
slot was reserved in M1's `handle_hvc` precisely for this.

### 8.2 Supported functions (DEN0022D)

| Function | ID | M2 behaviour |
|---|---|---|
| `PSCI_VERSION` | `0x84000000` | Return `0x00010001` (PSCI 1.1) |
| `PSCI_FEATURES` | `0x8400000A` | `SUCCESS` for VERSION/CPU_OFF/FEATURES/SYSTEM_OFF; else `NOT_SUPPORTED` |
| `CPU_OFF` | `0x84000002` | Print + halt vCPU (`wfi` loop) |
| `SYSTEM_OFF` | `0x84000008` | Print + halt |
| `SYSTEM_RESET` | `0x84000009` | Print + halt |
| `CPU_ON` | `0x84000003` / `0xC4000003` | **Reserved** — `NOT_SUPPORTED` (M3 SMP) |
| default | — | `NOT_SUPPORTED` |

Return values are written to `regs->x[0]` as a sign-extended `s64`. `CPU_OFF` /
`SYSTEM_OFF` / `SYSTEM_RESET` do not return to the guest in M2 (single vCPU →
nothing to schedule); they halt in a `wfi` loop, which is observable on the
console.

---

## 9. `struct vcpu` extension (ABI preservation)

vGIC fields are **appended** after `vttbr_el2`. The assembly-visible offsets that
M1 froze (`VCPU_HCR_EL2 = 0x110`, `VCPU_VTTBR_EL2 = 0x118`) are unchanged, so no
assembly offset macros need editing and `regs`-first remains the ABI invariant.

```c
struct vcpu {
    struct vcpu_regs regs;   /* MUST be first — assembly ABI            */
    u64 hcr_el2;             /* 0x110 (unchanged)                       */
    u64 vttbr_el2;           /* 0x118 (unchanged)                       */
    u64 ich_hcr_el2;         /* 0x120  (M2)                             */
    u64 ich_vmcr_el2;        /* 0x128  (M2)                             */
    u64 ich_lr[4];           /* 0x130: ICH_LR0..LR3 (M2)                */
};
```

`g_vm` is **non-static** in `vm.c` so both `vmexit_asm.S` and
`irq_handler_asm.S` can take its address by symbol.

---

## 10. Expected Output

```
  ... (M0 ASCII banner) ...

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

Order matters — it traces the §3.1 flow. QEMU then halts in the HV `wfi` loop;
exit with `Ctrl-A x`.

---

## 11. Verification Checklist

- [ ] `make` succeeds with zero warnings (`-Werror`); new objects present
      (`irq/gic_v3.o`, `irq/vgic.o`, `irq/irq_handler.o`, `irq/irq_handler_asm.o`,
      `timer/vtimer.o`, `common/psci/psci.o`).
- [ ] `readelf -h build/hypervisor.elf` — entry still `0x40080000`.
- [ ] `tests/run_svm2_test.sh` — all ten output lines in §10 order.
- [ ] SVM IRQ line proves end-to-end injection: `virtual timer IRQ received`.
- [ ] Final HV line is `[hv] SVM HVC: done`.
- [ ] **PSCI probe** (optional): SVM `PSCI_VERSION` HVC returns `0x00010001`.
- [ ] `Ctrl-A x` exits QEMU cleanly.

### Known failure modes (design-level)

| Symptom | Root cause |
|---|---|
| Hang after `virtual timer armed`, no IRQ | `daifclr #2` missing on `vcpu_run` eret path |
| `unexpected exit EC=0x18` | `vtimer_init()` not run before `vm_run()` |
| `IRQ: unexpected INTID=N` | PPI 27 not enabled / not Group 1 NS in GICR |
| `unexpected exit EC=0x00` on ICC_* access | `ICC_SRE_EL2.Enable` not set before guest entry |

---

## 12. Forward-Compatibility Contracts for M3

| What | Location | M3 use |
|---|---|---|
| `CPU_ON` slot in `psci_handle` | `psci.c` | Replace `NOT_SUPPORTED` with SMP bring-up |
| `g_vm` global | `vm.c` / `irq_handler.c` | Becomes array indexed by vCPU |
| `g_hv_ctx` global | `vmexit_asm.S` | Becomes per-CPU |
| `ich_lr[4]` + `vgic_save`/`restore` | `vgic.c` | Multi-LR fan-out, scheduler context-switch |
| Single GICR CPU0 frame | `gic_v3.c` | Per-CPU redistributor frames |
| `ICH_HCR_EL2` (En only) | `vgic.c` | Add maintenance-interrupt (`UIE`/`LRENPIE`) bits |
| PPI-only routing | `irq_handler.c` | Add SPI/SGI demux + virtual distributor |

End of M2 design.
