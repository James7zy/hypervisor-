# Hypervisor — M2 (Interrupts + PSCI) Design

- **Date**: 2026-05-31
- **Project**: `hypervisor-`
- **Milestone**: M2 — Interrupts + PSCI
- **Target platform**: QEMU `virt` (AArch64), GICv3, PL011 UART, Cortex-A72
- **Status**: Design approved, ready for implementation planning

> **Note (retroactive doc):** Unlike M0/M1, this design spec was written *after*
> its implementation plan (`docs/superpowers/plans/2026-05-30-hypervisor-m2.md`),
> to restore the per-milestone spec→plan pairing that M2 originally skipped. It is
> **descriptive**: it records the design decisions already baked into the plan and
> their rationale; it does not re-litigate them. Where this spec and the plan
> disagree, the plan is the implementation source of truth and this spec should be
> corrected to match.

---

## 1. Purpose & Positioning

M2 builds on M1's single-vCPU SVM by giving the guest a working **interrupt path**
and a minimal **power-state interface**:

- A **physical GICv3** initialised by the hypervisor (distributor + CPU0
  redistributor + EL2 CPU interface).
- A **vGICv3** that injects a virtual interrupt into the guest by writing a GIC
  list register (`ICH_LR0_EL2`).
- The **EL1 virtual timer**, configured so the guest can read the counter and arm
  `CNTV_*` without trapping, and whose PPI (INTID 27) is delivered to the guest as
  a virtual IRQ.
- A **PSCI dispatcher** reachable over HVC, answering `VERSION`, `FEATURES`,
  `CPU_OFF`, and `SYSTEM_OFF`.

The end-to-end success condition is: the SVM arms its virtual timer, the timer PPI
fires, the hypervisor takes it at EL2 and injects it as a virtual IRQ, the guest's
own EL1 IRQ handler runs, and the guest then signals completion via HVC — all
observable on the UART.

M2 deliberately runs on a **single vCPU**. Multi-vCPU bring-up (PSCI `CPU_ON`, SMP
scheduling) was originally bundled into this milestone's name but is deferred to M3,
where it couples naturally with the Linux guest; M2 only ensures its data structures
and code do not *preclude* it (see §7).

### Roadmap context

| Milestone | Goal |
|---|---|
| **M0 — Hello EL2** *(done)* | EL2 entry, PL011 UART, banner, wfi |
| **M1 — Bare-Metal SVM** *(done)* | Stage-2 MMU, single vCPU, EL1 SVM launch, HVC |
| **M2 — Interrupts + PSCI** *(this spec)* | Physical GICv3, vGICv3 injection, virtual timer, PSCI |
| M3 — Linux Guest | Boot Linux to shell, virtio-console; SMP via PSCI CPU_ON |
| M4 — RK3588 Port | Run on real RK3588 hardware |

---

## 2. Scope

### 2.1 In scope (M2)

- **Physical GICv3 init**: `GICD_CTLR` affinity routing + Group 1 NS; CPU0
  redistributor wake; PPI 27 configured Group 1 NS, priority `0xA0`, enabled;
  `ICC_SRE_EL2.Enable=1`; EL2 priority mask + Group 1 enable so EL2 can ack/EOI.
- **vGICv3 virtual CPU interface**: enable via `ICH_HCR_EL2.En`; per-vCPU
  save/restore of `ICH_HCR_EL2`, `ICH_VMCR_EL2`, and `ICH_LR0..LR3`; injection of a
  pending Group-1 virtual interrupt through `ICH_LR0_EL2`.
- **EL2 IRQ entry**: vector slot `+0x480` (Lower EL AArch64 IRQ) wired to an
  assembly stub that saves guest state, calls a C handler, restores, unmasks
  `DAIF.I`, and `eret`s. The C handler acks the physical IRQ, injects the virtual
  IRQ, and EOIs.
- **DAIF.I unmasking at EL2** on the guest-entry `eret` path (in `vcpu_run`) and on
  the IRQ-return `eret` path, so physical IRQs are actually taken while the guest
  runs.
- **Virtual timer**: `CNTHCTL_EL2 = 0b11` (EL0/EL1 may read the physical counter and
  access the physical timer registers); `CNTVOFF_EL2 = 0` (no virtual/physical time
  skew).
- **PSCI dispatcher**: SMCCC-style HVC dispatch for `VERSION` (returns 1.1),
  `FEATURES`, `CPU_OFF`, `SYSTEM_OFF`, `SYSTEM_RESET`; `CPU_ON` present but returns
  `NOT_SUPPORTED`.
- **Build integration**: new `arch/arm64/irq/`, `arch/arm64/timer/`, and
  `common/psci/` subtrees wired into the Makefiles and include paths.
- **`struct vcpu` extension**: append vGIC state fields without disturbing the
  M1 assembly offset ABI.
- **M2 SVM test payload** (`tests/svm2/`): an EL1 program that configures its GIC
  interface, arms the virtual timer, handles the IRQ, and HVCs done.

### 2.2 Out of scope (M2)

- **Multiple vCPUs / SMP / PSCI `CPU_ON`** (M3). `CPU_ON` returns `NOT_SUPPORTED`.
- **Full vGIC distributor/redistributor MMIO emulation** — the guest touches the
  CPU *system-register* interface (`ICC_*`) directly; trapping and emulating
  `GICD_*`/`GICR_*` MMIO is not done (M3, needed for an unmodified Linux guest).
- **Multiple SPIs, SGIs, MSI/LPI** — only the single virtual-timer PPI (INTID 27)
  is delivered. Only `ICH_LR0` is used for injection.
- **Interrupt prioritisation / preemption logic** beyond a single priority value.
- **Guest-programmable timer offset / time virtualization** — `CNTVOFF_EL2` is a
  fixed 0.
- **PSCI `CPU_SUSPEND`, `MIGRATE`, affinity-info** and full DEN0022 surface.
- **Stage-2 changes** — Stage-2 MMU is unchanged from M1.
- **FP/SIMD context save/restore** — `-mgeneral-regs-only` remains mandatory.

---

## 3. Architecture

### 3.1 End-to-end flow

```
main.c
  gic_init()                         ← physical GICv3: GICD, GICR CPU0, ICC_SRE_EL2
  vtimer_init()                      ← CNTHCTL_EL2=3, CNTVOFF_EL2=0
  vm_init()                          ← HCR/SPSR/SP_EL1, stage2_init, vgic_init
  vm_run()
    │ stage2_activate()
    │ vgic_restore()                 ← load ICH_HCR/VMCR/LR0..3 from vcpu
    │ vcpu_run(&g_vm.vcpu)           ← assembly
    │   ... restore guest state ...
    │   msr daifclr, #2              ← unmask IRQ at EL2 (HCR_EL2.IMO routes here)
    │   eret ──────────────────────────────────────► EL1 SVM
    │                                                    │ ICC_SRE_EL1=7, PMR, IGRPEN1
    │                                                    │ arm CNTV_TVAL/CTL
    │                                                    │ daifclr #2 ; wfi
    │                                                    ▼  [virtual timer PPI fires]
    │◄── el1_irq_handler_asm (vector +0x480) ────────────┘  (physical IRQ via IMO=1)
    │   save guest state
    │   bl el2_irq_handler
    │       intid = gic_ack_irq()  (ICC_IAR1_EL1)
    │       vgic_inject(vcpu, 27, 0xA0)   → write ICH_LR0_EL2 (Pending, Group1)
    │       gic_eoi_irq(27)        (ICC_EOIR1 + ICC_DIR)
    │   restore guest state
    │   msr daifclr, #2
    │   eret ──────────────────────────────────────► EL1 SVM
    │                                                    │ vGIC delivers vIRQ now
    │                                                    ▼
    │                                              svm2_irq_handler (VBAR_EL1 +0x280)
    │                                                    │ ICC_IAR1 → handle → EOI+DIR
    │                                                    │ disarm CNTV_CTL
    │                                                    │ hvc #0  x0=HC_GUEST_DONE
    │◄── el1_sync_handler (vector +0x400) ────────────────┘
    │   handle_exit → EC=0x16 → handle_hvc → HC_GUEST_DONE → hv_restore()
    └─ returns to vm_run() → main.c for(;;) wfi
```

PSCI shares the HVC path: `handle_hvc` inspects the SMCCC service byte
(`func_id >> 24`); `0x84` (SMC32) or `0xC4` (SMC64) routes to `psci_handle`.

### 3.2 GIC ownership policy

The hypervisor **owns the physical GIC** and never lets the guest touch physical
distributor/redistributor state. The guest is given only the *system-register* CPU
interface (`ICC_*`), gated by `ICC_SRE_EL2.Enable=1` set in `gic_init()`. Physical
interrupts are routed to EL2 by `HCR_EL2.IMO/FMO/AMO=1` (inherited from M1); the
guest never sees a physical interrupt directly — it only ever sees the *virtual*
interrupt the hypervisor injects.

### 3.3 HCR_EL2 — delta from M1

The HCR_EL2 value is **unchanged from M1** (`VM | FMO | IMO | AMO | RW`):

| Bit | Field | Value | Role in M2 |
|---|---|---|---|
| 0 | VM | 1 | Stage-2 enabled (M1) |
| 3 | FMO | 1 | Physical FIQ → EL2 |
| 4 | IMO | 1 | **Physical IRQ → EL2 — the mechanism that makes vtimer delivery work** |
| 5 | AMO | 1 | SError → EL2 |
| 31 | RW | 1 | EL1 is AArch64 |
| 29 | HCD | 0 | HVC from EL1 allowed (PSCI + HC_GUEST_DONE) |

What changes in M2 is not HCR_EL2 but **DAIF**: M1 kept `DAIF.I` masked at EL2 for
the whole guest window, so no IRQ was ever taken. M2 unmasks it (see §3.4).

### 3.4 DAIF.I lifecycle (correctness proof)

1. **M0 `head.S`**: `daifset #0xF` — all of D/A/I/F masked on EL2 boot.
2. **`vcpu_run`**: saves HV context, sets `HCR_EL2.IMO=1`, restores guest GPRs,
   **`daifclr #2`** (unmask I), `eret`. The guest runs at EL1 with physical IRQs
   now able to reach EL2.
3. **Physical IRQ fires** → taken at EL2 via `el1_irq_handler_asm` at vector
   `+0x480`. On exception entry the CPU **auto-sets `PSTATE.I`** (IRQs re-masked at
   EL2 during the handler).
4. **IRQ handler** injects the virtual IRQ into `ICH_LR0_EL2`, then **`daifclr #2`**
   and `eret`s back to the guest; the vGIC delivers the pending virtual IRQ to EL1
   immediately.
5. **HVC exit** → `el1_sync_handler` at `+0x400`; exception entry **auto-masks
   `PSTATE.I`** again → `hv_restore()` → returns to `vm_run()` with `DAIF.I` masked.
   EL2 is never left running with IRQs unmasked outside a guest window.

---

## 4. Component Design

### 4.1 New file layout

```
hypervisor/
├── arch/arm64/
│   ├── irq/
│   │   ├── gic_v3.h / gic_v3.c           ← physical GICv3 init, ack, EOI
│   │   ├── vgic.h   / vgic.c             ← vGIC LR management, save/restore, inject
│   │   ├── irq_handler.c                 ← el2_irq_handler() C logic
│   │   └── irq_handler_asm.S             ← el1_irq_handler_asm entry stub
│   ├── timer/
│   │   ├── vtimer.h / vtimer.c           ← CNTHCTL_EL2 / CNTVOFF_EL2 config
│   │   └── (vtimer_init)
│   └── include/asm/sysreg.h              ← +GIC/vGIC/timer S<...> register aliases
└── common/psci/
    ├── psci.h / psci.c                    ← PSCI function IDs, return codes, dispatcher
```

Modified existing files: `board.h` (GIC bases + vtimer INTID), `include/vm.h`
(`struct vcpu` extension), `vmexit/vmexit.c` (wire PSCI), `vmexit/vmexit_asm.S`
(`daifclr #2` before guest eret), `boot/vectors.S` (wire `+0x480`), `boot/main.c`
(call `gic_init`/`vtimer_init`), `common/vm/vm.c` (call `vgic_init`/`vgic_restore`),
and both Makefiles.

### 4.2 Physical GICv3 (`gic_v3.c`)

QEMU `virt` GICv3 bases (in `board.h`): `GICD = 0x08000000`, `GICR CPU0 =
0x080A0000` (RD frame; SGI frame at `+0x10000`); vtimer PPI `INTID = 27`.

`gic_init()` sequence:
1. `GICD_CTLR = ARE_NS | ENGRP1NS`; wait `RWP` clear.
2. Wake CPU0 redistributor: clear `GICR_WAKER.ProcessorSleep`, wait
   `ChildrenAsleep=0`.
3. PPI 27 → Group 1 (`GICR_IGROUPR0`), Non-Secure (`GICR_IGRPMODR0`), priority
   `0xA0` (`GICR_IPRIORITYR`, byte-addressed), enabled (`GICR_ISENABLER0`).
4. `ICC_SRE_EL2 = 0xF` (SRE | DIL | DFB | Enable); `isb`.
5. `ICC_PMR_EL1 = 0xFF`, `ICC_IGRPEN1_EL1 = 1`, **`ICC_CTLR_EL1.EOImode = 1`**
   (split priority-drop / deactivate — required so EL2 can priority-drop a
   *forwarded* interrupt without deactivating it; see §5.1). Setting the physical
   EOImode at EL2 does not affect the guest's virtual CPU interface, which is
   governed independently by `ICH_VMCR_EL2.VEOIM`.

API: `void gic_init(void)`, `u32 gic_ack_irq(void)` (reads `ICC_IAR1_EL1`, masks to
24-bit INTID), `void gic_priority_drop(u32 intid)` (`ICC_EOIR1_EL1`),
`void gic_deactivate(u32 intid)` (`ICC_DIR_EL1`). With `EOImode=1` the two are
separate operations; the HW-forwarded timer path uses **priority-drop only**.

### 4.3 vGICv3 (`vgic.c`)

State lives in `struct vcpu` (§4.6). All operations are keyed on `struct vcpu *` so
they are ready for per-vCPU scheduling later. The vGIC exposes **two injection
paths**, because a hypervisor needs both (see §5.1):

- `vgic_inject_sw(vcpu, vintid, prio)` — a **purely virtual** interrupt with no
  physical source (`HW=0`). `ICH_LR` = State=Pending (`bit 62`) | Group 1 (`bit 60`)
  | priority (`[55:48]`) | vINTID (`[31:0]`). Reserved for future sources (virtio,
  SGIs); **not** used by the M2 timer path.
- `vgic_inject_hw(vcpu, vintid, pintid, prio)` — a **hardware-forwarded** interrupt
  (`HW=1`, bit 61) carrying the physical INTID in the pINTID field (`[44:32]`). Used
  for the timer PPI so the guest's deactivate of the *virtual* interrupt deactivates
  the *physical* INTID via the LR linkage.
- `vgic_init(vcpu)` — `ICH_HCR_EL2.En=1`, `ICH_VMCR_EL2=0` (the guest configures
  VPMR/VENG1 itself via `ICC_*` writes), all LRs cleared; programs the registers.
- `vgic_save(vcpu)` / `vgic_restore(vcpu)` — read/write `ICH_HCR_EL2`,
  `ICH_VMCR_EL2`, `ICH_LR0..LR3` to/from the vcpu struct.

Both inject functions write `ICH_LR0_EL2` and store into `vcpu->ich_lr[0]`; no `isb`
— the caller's `eret` synchronises delivery.

### 4.4 EL2 IRQ handler (`irq_handler.c` + `irq_handler_asm.S`)

`el1_irq_handler_asm` mirrors `el1_sync_handler`'s save/restore against
`g_vm.vcpu.regs`, with one difference: it ends with `msr daifclr, #2` before `eret`.
The C handler `el2_irq_handler()`:

```
intid = gic_ack_irq()                         # ICC_IAR1 → phys INTID 27 now Active
if intid == BOARD_VTIMER_IRQ:
    vgic_inject_hw(&g_vm.vcpu, 27, 27, 0xA0)  # HW=1: vINTID = pINTID = 27
    gic_priority_drop(intid)                  # ICC_EOIR1 only — do NOT deactivate
else:
    printk("unexpected INTID")
    gic_priority_drop(intid); gic_deactivate(intid)
```

The physical INTID 27 is deliberately left **Active** across the guest window: a
level-triggered timer line that is deactivated while still asserted re-pends
immediately and, being routed to EL2, storms the guest before it can run (§5.1).
The guest's deactivate of the *virtual* interrupt releases the physical INTID
through the `HW=1` linkage.

### 4.5 Virtual timer (`vtimer.c`)

`vtimer_init()` sets `CNTHCTL_EL2 = 0b11` (`EL1PCTEN | EL1PCEN`: guest may read
`CNTPCT_EL0` and access physical timer registers without trapping as `EC=0x18`) and
`CNTVOFF_EL2 = 0` (guest `CNTVCT_EL0 == CNTPCT_EL0`), then `isb`. It must be called
**before** `vm_run()`; otherwise the SVM's first counter read traps.

### 4.6 `struct vcpu` ABI extension

The M1 assembly offset ABI (`VCPU_HCR_EL2 = 0x110`, `VCPU_VTTBR_EL2 = 0x118`) is
**preserved**. New vGIC fields are appended after `vttbr_el2`:

```c
struct vcpu {
    struct vcpu_regs regs;   /* MUST be first: assembly ABI (offsets 0x000–0x10F) */
    u64 hcr_el2;             /* 0x110 (unchanged) */
    u64 vttbr_el2;           /* 0x118 (unchanged) */
    u64 ich_hcr_el2;         /* 0x120 (new) */
    u64 ich_vmcr_el2;        /* 0x128 (new) */
    u64 ich_lr[4];           /* 0x130 (new): ICH_LR0..LR3 */
};
```

Because the new fields sit beyond every offset the assembly references, **no
assembly offset macros change** — the M1 `vmexit_asm.S` contract is untouched.

### 4.7 PSCI dispatcher (`psci.c`)

`psci_handle(regs)` switches on `regs->x[0]` (the SMCCC function ID):

| Function | ID | Behaviour |
|---|---|---|
| `VERSION` | `0x84000000` | return `0x00010001` (PSCI 1.1) |
| `FEATURES` | `0x8400000A` | `SUCCESS` for the implemented set, else `NOT_SUPPORTED` |
| `CPU_OFF` | `0x84000002` | print + `wfi` loop (single vCPU) |
| `SYSTEM_OFF` | `0x84000008` | print + `wfi` loop |
| `SYSTEM_RESET` | `0x84000009` | print + `wfi` loop |
| `CPU_ON` (32/64) | `0x84000003`/`0xC4000003` | `NOT_SUPPORTED` (M3) |
| default | — | `NOT_SUPPORTED` |

---

## 5. Design Decisions / Alternatives Considered

Each entry records the decision *as made* in the plan, the rejected alternative, and
the condition under which the decision must be revisited.

### 5.1 Single list register; software vs hardware-forwarded injection

**Decision:** Keep a single active list register (`ICH_LR0`) and no distributor MMIO
emulation, but support **both** injection encodings: software (`HW=0`) for purely
virtual sources, and **hardware-forwarded (`HW=1`)** for interrupts that originate
from a real physical INTID. The M2 timer PPI uses the **HW=1** path.

**Rejected:** (a) Software injection only — see the storm below. (b) Full
`GICD_*`/`GICR_*` MMIO trap-and-emulate.

**Why HW=1 for the timer:** The EL1 virtual-timer PPI (INTID 27) is
**level-sensitive** — its line stays asserted until the guest writes `CNTV_CTL`.
With software injection the EL2 handler would have to deactivate the physical INTID
at the GIC, but the still-asserted line re-pends it immediately. Because INTID 27 is
routed to EL2 (`HCR_EL2.IMO=1`) and an EL2-targeted interrupt preempts a lower EL
*regardless of that EL's `PSTATE.I`*, the `eret` to EL1 is taken straight back into
EL2 before the guest executes a single instruction — a re-pend storm in which the
guest never reaches its handler to disarm the timer. Hardware forwarding breaks the
loop: EL2 only **priority-drops**, the physical INTID stays **Active** (so it cannot
re-pend), and the guest's deactivate of the *virtual* interrupt releases the
physical one through the LR's `HW`/pINTID linkage. This requires
`ICC_CTLR_EL1.EOImode = 1` at EL2 (§4.2).

**Why keep software injection too:** future virtual sources (virtio devices,
SGIs/IPIs) have no physical INTID to forward and must be injected purely in
software. Adding `vgic_inject_sw` now fixes the API shape so M3 adds *sources*, not
*signatures*.

**Revisit when:** M3 boots Linux — many INTIDs and MMIO distributor programming
require a real distributor model and multi-LR (`ICH_LR0..LRn`) allocation/overflow
handling.

### 5.2 Hypervisor-owned physical GIC vs guest passthrough

**Decision:** The hypervisor initialises and owns the physical GIC; the guest gets
only the `ICC_*` system-register interface, enabled via `ICC_SRE_EL2.Enable=1`.

**Rejected:** Passing the physical GIC through to the guest.

**Why:** A type-1 hypervisor must mediate physical interrupts to multiplex them
across VMs and to keep a guest from controlling another VM's or the hypervisor's
interrupts. `HCR_EL2.IMO=1` (already set in M1) makes EL2 the sole recipient of
physical IRQs; injection is the only path to the guest.

**Revisit when:** never for this design line — this is foundational type-1 policy.

### 5.3 `CNTVOFF_EL2 = 0` vs a non-zero virtual offset

**Decision:** `CNTVOFF_EL2 = 0`, so guest virtual time equals physical time.

**Rejected:** A per-VM virtual offset to give each guest an independent timebase.

**Why:** With one VM and no migration/suspend, an offset adds state and arithmetic
with no observable benefit, and keeps the timer math in the test payload trivial.

**Revisit when:** multiple VMs need independent or paused timebases, or when
save/restore/migration requires preserving guest-perceived time across a stop.

### 5.4 Hard-coded GIC bases & PPI 27 vs Device Tree parsing

**Decision:** `BOARD_GIC_DIST_BASE`, `BOARD_GIC_RDIST_BASE`, and
`BOARD_VTIMER_IRQ = 27` are `#define`s in the board header.

**Rejected:** Parse the DTB (`dtb_phys` is already passed to `hypervisor_main`) to
discover GIC bases and the timer interrupt.

**Why:** The values are fixed and known for the QEMU `virt` machine, and the
board-header pattern is the project's established convention for board constants
(consistent with the UART base in M0/M1). INTID 27 is the architectural EL1 virtual
timer PPI.

**Revisit when:** M4 (RK3588) or any board whose GIC layout differs — at that point
a DT/ACPI discovery path belongs in the board layer, not the GIC driver.

### 5.5 PSCI as an HVC dispatch slot vs a separate SMC path

**Decision:** PSCI is dispatched from the existing HVC handler, distinguished from
vendor hypercalls by the SMCCC service byte (`0x84`/`0xC4`).

**Rejected:** A dedicated SMC conduit / separate trap path for PSCI.

**Why:** M1 already reserved the PSCI slot in `handle_hvc`; routing by service byte
reuses the existing trap-and-dispatch plumbing with no new entry path. The guest
issues PSCI over HVC, which is a valid SMCCC conduit.

**Revisit when:** a guest expects the SMC conduit specifically, or firmware-level
PSCI (EL3) needs to be forwarded.

### 5.6 Single vCPU for an "interrupts" milestone

**Decision:** Implement the full interrupt + PSCI path on one vCPU; defer `CPU_ON`
and SMP to M3.

**Rejected:** Bring up multi-vCPU now (the milestone's nominal title).

**Why:** The interrupt path (physical GIC → EL2 → vGIC injection → EL1 delivery) is
the genuinely new and risky mechanism; proving it on one vCPU isolates that risk
from SMP scheduling concerns. The data structures are kept SMP-ready (§7) so M3 is
additive.

**Revisit:** M3, which implements `CPU_ON` and per-CPU state.

---

## 6. Acceptance Criteria

1. **Build**: `make defconfig && make` succeeds with **zero warnings** (`-Werror`),
   producing `build/hypervisor.elf`. New objects exist under
   `build/obj/arch/arm64/irq/`, `.../timer/`, and `build/obj/common/psci/`.
2. **Static**: `readelf -h build/hypervisor.elf` reports entry point `0x40080000`.
3. **Functional** (`tests/run_svm2_test.sh`): the following lines appear on the
   UART **in order**:

   ```
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

   (Preceded by the five-line ASCII banner.) QEMU then idles in `wfi`.
4. **PSCI (optional probe)**: with the temporary `PSCI_VERSION` HVC in the SVM
   payload, the guest observes `[svm] PSCI VERSION=0x00010001`.

The single most diagnostic line is `[svm] virtual timer IRQ received` — it proves
the entire physical-IRQ → EL2 → inject → EL1-delivery chain end to end.

---

## 7. Forward Compatibility (M3)

The design keeps SMP bring-up additive rather than a rewrite:

- `g_hv_ctx` (in `vmexit_asm.S`) and `g_vm` (in `vm.c` / `irq_handler.c`) are
  single instances today. Making them per-CPU/per-vCPU is **not** just array
  indexing: the exception entry stubs (`el1_sync_handler`, `el1_irq_handler_asm`)
  resolve the vCPU via a fixed `adrp g_vm`, and on SMP an exception can fire on any
  physical CPU. M3 must introduce a per-physical-CPU **"current vCPU" pointer**
  (idiomatically `TPIDR_EL2`, loaded in `vcpu_run` and read at the top of every
  stub) to replace the fixed symbol. The C-level `struct vcpu *` signatures are
  already shaped for this — the work is in the asm entry paths, not the signatures.
- PSCI `CPU_ON` already exists as a dispatch case returning `NOT_SUPPORTED` with an
  "M3" comment — M3 fills it in.
- `vgic_init/inject/save/restore` already take `struct vcpu *`, so per-vCPU vGIC
  state needs no signature change.
- `el2_irq_handler` currently special-cases only INTID 27; a real INTID→handler
  routing table and multi-LR allocation are the M3 extensions for a Linux guest
  (see §5.1).
