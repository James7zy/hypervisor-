# vCPU1 spins a guest (smoke test)

Status: ready-for-agent

## Parent

Plan: `docs/superpowers/plans/2026-06-23-m3.5-smp.md` — Slice 3.
Design: `docs/superpowers/specs/2026-06-23-m3.5-smp-design.md`.

## What to build

Get pCPU1 to actually *enter* guest context (EL1) without crashing, sharing
vCPU0's Stage-2 translation and VMID. This is a smoke test of the per-CPU guest
entry path; the entry becomes guest-authored in slice 04.

- Widen vCPU storage to `vcpu[NR_CPUS]` (or per-CPU slots inside the VM struct) if
  not already done in slice 01. vCPU1 reuses vCPU0's Stage-2 table and VMID.
- In `secondary_main`, after EL2 init, perform per-CPU guest entry in the
  **required order**: activate Stage-2 for this vCPU, **then** restore the vGIC
  state, set `HCR_EL2`, `isb`, then run the vCPU. Stage-2 activation must precede
  the vGIC restore.
- Set a per-vCPU virtual MPIDR (`VMPIDR_EL2`): vCPU0 → Aff0=0, vCPU1 → Aff0=1, in
  each core's bring-up.
- For the smoke test only, seed vCPU1's registers to a known stub (or the same
  entry as vCPU0) just to confirm entry works. Remove the stub seeding once the
  test passes — slice 04 supplies the real vCPU1 register state from the guest's
  PSCI request.

## Acceptance criteria

- [ ] vCPU storage holds `NR_CPUS` vCPUs sharing one Stage-2 table / VMID.
- [ ] `secondary_main` enters guest via Stage-2 activate → vGIC restore → `HCR_EL2` → run, in that order.
- [ ] `VMPIDR_EL2` set per vCPU (Aff0 = vCPU index).
- [ ] `make run`: both cores enter the guest without crashing.
- [ ] Temporary stub register seeding is removed after the smoke test passes.
- [ ] `make` zero warnings; UP boot path remains green; entry point unchanged.

## Blocked by

- `.scratch/m3.5-smp/issues/02-wake-pcpu1.md`
