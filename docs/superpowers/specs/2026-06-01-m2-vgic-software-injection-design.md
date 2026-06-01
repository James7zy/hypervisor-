# Hypervisor — M2 (vGIC Software Injection) Design

- **Date**: 2026-06-01
- **Project**: `hypervisor-`
- **Milestone**: M2 — vGIC software injection
- **Target platform**: QEMU `virt` (AArch64), GICv3
- **Status**: Design approved, ready for implementation planning

> Split out of the original combined M2 spec (now superseded). This milestone proves
> the **virtual interrupt delivery path in isolation**, using a software-injected
> virtual interrupt triggered by an HVC. It deliberately contains **no physical GIC
> init, no timer, no EL2 physical-IRQ vector, and no HW-forwarding** — those move to
> M2.5. The point is to validate the one genuinely novel mechanism (the vGIC
> delivering a vIRQ to the guest) with the fewest possible moving parts.

---

## 1. Purpose & Positioning

M2 answers a single question: **can the hypervisor make a virtual interrupt appear
in the guest's EL1 IRQ handler?** It does so without any real interrupt source — the
guest asks for one via HVC, the hypervisor injects it into a vGIC list register, and
on return the virtual CPU interface delivers it to the guest.

Because the trigger is an HVC (a synchronous exception that already works since M1),
M2 needs **no new EL2 exception path** and **no physical interrupt routing**. That is
the deliberate design: prove the hard idea (virtual delivery) on top of proven
plumbing, then add real hardware in M2.5.

### Roadmap context

| Milestone | Goal |
|---|---|
| M1 — Bare-Metal SVM *(done)* | Stage-2 MMU, single vCPU, EL1 SVM launch, HVC |
| M1.5 — PSCI | PSCI over HVC (independent) |
| **M2 — vGIC software injection** *(this spec)* | HVC → `vgic_inject_sw` → guest EL1 IRQ handler |
| M2.5 — Physical timer + GIC + HW-forwarding | real timer drives the proven vGIC path |
| M3 — Linux Guest | distributor MMIO emulation, many INTIDs, SMP |

---

## 2. Scope

### 2.1 In scope (M2)

- **vGIC virtual CPU interface enable**: `ICH_HCR_EL2.En=1`; `ICH_VMCR_EL2=0` at
  init (the guest configures its own `VPMR`/`VENG1` via `ICC_*` writes).
- **`ICC_SRE_EL2.Enable=1`** so the guest may use the `ICC_*` system-register
  interface at EL1. This is the *only* GIC register the hypervisor touches — there is
  **no `GICD`/`GICR` distributor/redistributor init**.
- **Software injection**: `vgic_inject_sw(vcpu, vintid, prio)` writes a Pending,
  Group-1, `HW=0` virtual interrupt into `ICH_LR0_EL2`.
- **Per-vCPU vGIC state**: save/restore of `ICH_HCR_EL2`, `ICH_VMCR_EL2`,
  `ICH_LR0..LR3`; `struct vcpu` extended (M1 asm offset ABI preserved).
- **Injection trigger**: a new vendor HVC (`HC_INJECT_TEST`) handled in `handle_hvc`
  calls `vgic_inject_sw` on the current vCPU.
- **M2 SVM test payload**: an EL1 program that installs `VBAR_EL1`, enables its
  `ICC_*` interface, unmasks `PSTATE.I`, issues `HC_INJECT_TEST`, runs its own EL1
  IRQ handler, EOIs, and HVCs done.

### 2.2 Out of scope (M2 — all in M2.5 unless noted)

- **Physical GICv3 init** (`GICD`/`GICR`, PPI config) — M2.5.
- **EL2 physical-IRQ vector** (`+0x480`) and **EL2 `DAIF.I` unmasking** — not needed,
  because no physical interrupt is taken at EL2; the vIRQ is delivered to EL1 on the
  HVC eret-back. M2.5.
- **Virtual timer** (`CNTHCTL_EL2`/`CNTVOFF_EL2`) — M2.5.
- **Hardware-forwarded injection** (`HW=1`, `ICC_CTLR_EL1.EOImode`, ADR-0001) — M2.5.
- **Multiple INTIDs / LRs, distributor MMIO emulation, SMP** — M3.

---

## 3. Architecture

### 3.1 End-to-end flow (no physical interrupt anywhere)

```
main.c
  vm_init()                       ← HCR/SPSR/SP_EL1, stage2_init, vgic_init
  vm_run()
    │ stage2_activate()
    │ vgic_restore()              ← load ICH_HCR/VMCR/LR0..3
    │ vcpu_run(&g_vm.vcpu)        ← (M1 asm, unchanged: no daifclr needed)
    │   eret ───────────────────────────────────► EL1 SVM
    │                                                │ msr vbar_el1
    │                                                │ ICC_SRE_EL1=7, PMR=0xFF, IGRPEN1=1
    │                                                │ msr daifclr,#2   (guest unmasks its own IRQ)
    │                                                │ hvc x0=HC_INJECT_TEST, x1=vintid
    │◄── el1_sync_handler (+0x400) ───────────────────┘   (existing M1 sync path)
    │   handle_exit → EC=0x16 → handle_hvc
    │       HC_INJECT_TEST → vgic_inject_sw(&g_vm.vcpu, vintid, 0xA0)  → write ICH_LR0
    │   el1_sync_handler eret-back path
    │   eret ───────────────────────────────────► EL1 SVM
    │                                                │ vGIC presents the pending vIRQ
    │                                                ▼
    │                                          svm2_irq_handler (VBAR_EL1 +0x280)
    │                                                │ ICC_IAR1 → handle → ICC_EOIR1
    │                                                │ hvc x0=HC_GUEST_DONE
    │◄── el1_sync_handler (+0x400) ───────────────────┘
    │   HC_GUEST_DONE → hv_restore()
    └─ returns to vm_run() → for(;;) wfi
```

The crucial point: the vIRQ is delivered on the **eret-back from the inject HVC**,
gated only by the guest's `PSTATE.I` and its virtual `VENG1`/`VPMR` — the EL2 DAIF
and the `+0x480` vector play no role.

### 3.2 vGIC enablement

`vgic_init` sets `ICH_HCR_EL2.En=1` so the guest's `ICC_*` accesses are redirected to
the *virtual* CPU interface (`ICV_*`) and `ICH_LR*` entries are presented as virtual
interrupts. `ICH_VMCR_EL2=0`; the guest then sets virtual `VPMR`/`VENG1` by writing
`ICC_PMR_EL1`/`ICC_IGRPEN1_EL1` at EL1. `ICC_SRE_EL2.Enable=1` (set once, hypervisor
side) lets the guest use `ICC_SRE_EL1`.

## 4. Component Design

### 4.1 New / modified files

```
hypervisor/arch/arm64/irq/vgic.h / vgic.c   ← vgic_init, vgic_inject_sw, save/restore
hypervisor/arch/arm64/include/asm/sysreg.h  ← +ICH_HCR/VMCR/LR0..3, ICC_SRE_EL2/EL1,
                                               ICC_PMR/IGRPEN1/IAR1/EOIR1 aliases
hypervisor/include/vm.h                      ← struct vcpu += vGIC fields
hypervisor/arch/arm64/vmexit/vmexit.c        ← handle_hvc: HC_INJECT_TEST → inject_sw
hypervisor/include/hypercall.h               ← HC_INJECT_TEST
hypervisor/common/vm/vm.c                    ← vgic_init in vm_init, vgic_restore in vm_run
                                               (+ a one-off ICC_SRE_EL2 enable)
tests/svm2/                                  ← EL1 payload (vectors + main + lds + runner)
```

### 4.2 vGIC API (`vgic.c`)

- `vgic_init(vcpu)` — `ICH_HCR_EL2.En=1`, `ICH_VMCR_EL2=0`, `ICH_LR0..3=0`.
- `vgic_inject_sw(vcpu, vintid, prio)` — `ICH_LR0 = Pending(bit62) | Group1(bit60) |
  prio<<48 | vintid`; store in `vcpu->ich_lr[0]`. No `isb` (the eret synchronises).
- `vgic_save(vcpu)` / `vgic_restore(vcpu)` — `ICH_HCR/VMCR/LR0..3` ↔ struct.

The `vgic_inject_sw` signature is chosen so M2.5 only *adds* `vgic_inject_hw` — no
existing signature changes.

### 4.3 `struct vcpu` ABI extension

M1 offsets (`VCPU_HCR_EL2=0x110`, `VCPU_VTTBR_EL2=0x118`) preserved; new fields
appended (`ich_hcr_el2` 0x120, `ich_vmcr_el2` 0x128, `ich_lr[4]` 0x130). No assembly
offset macro changes.

## 5. Design Decisions

### 5.1 Software injection (HW=0), triggered by HVC

**Decision:** prove virtual delivery with a software-injected vIRQ triggered by an
HVC, with no physical interrupt source. **Rejected:** start with the real timer
(couples in physical GIC init, timer config, and the level-triggered re-pend storm
that forces HW-forwarding — three hard things at once). **Why:** the novel mechanism
is *the vGIC presenting a virtual interrupt to the guest*; everything physical is
incidental. Triggering via HVC reuses the M1 sync path, so M2 adds **zero** new
exception/asm machinery. **Revisit:** never — software injection is permanently
needed for sources with no physical INTID (virtio, SGIs).

### 5.2 No EL2 IRQ vector / no EL2 DAIF change in M2

**Decision:** leave `vcpu_run` and the vector table exactly as M1 left them.
**Why:** a virtual interrupt is delivered to EL1, gated by the guest's `PSTATE.I` and
virtual interface — EL2's `DAIF.I` and the `+0x480` physical-IRQ vector are only
relevant once a *physical* interrupt must be taken at EL2, which first happens in
M2.5. Keeping them untouched shrinks M2's surface and isolates the asm changes to the
milestone that needs them.

## 6. Acceptance Criteria

1. **Build**: `make` succeeds with zero warnings; `build/obj/arch/arm64/irq/vgic.o`
   exists; entry point `0x40080000`.
2. **Functional**: the test guest produces, in order:

   ```
   [svm] EL1 init
   [svm] vGIC EL1 configured
   [svm] requesting injection (vINTID=N)
   [svm] vIRQ received (INTID=N)
   [svm] signalling HVC done
   [hv] SVM HVC: done
   ```

   The diagnostic line is `[svm] vIRQ received` — it proves the vGIC delivered a
   virtual interrupt to the guest's own EL1 handler.

## 7. Forward Compatibility (M2.5, M3)

- `vgic_inject_sw` is the permanent software path; M2.5 adds `vgic_inject_hw`.
- `vgic_*` already take `struct vcpu *`, ready for per-vCPU scheduling (M3).
- M2.5 adds the `+0x480` EL2 IRQ vector, `el1_irq_handler_asm`, EL2 `DAIF.I`
  unmasking, `gic_init`, and `vtimer_init` on top of this proven core.
