# Guest-driven PSCI CPU_ON — secondary processor comes online

Status: ready-for-agent

## Parent

Plan: `docs/superpowers/plans/2026-06-23-m3.5-smp.md` — Slice 4 (headline).
Design: `docs/superpowers/specs/2026-06-23-m3.5-smp-design.md`.

## What to build

Make the second vCPU come online **because the guest asks for it**. This is the
headline M3.5 deliverable: an `-smp 2` Linux guest brings up its secondary CPU
through the emulated PSCI interface.

- DTS + QEMU: add a symmetric `cpu@1` node (device_type cpu, cortex-a72,
  `reg=<0x1>`, `enable-method="psci"`) to the guest device tree; confirm the
  `psci { method="hvc"; }` node; switch the QEMU launch from `-smp 1` to `-smp 2`
  and recompile the DTB.
- Replace the PSCI `CPU_ON` `NOT_SUPPORTED` stub with a real handler: read the
  target affinity / entry point / context id from the guest's call, map the
  target affinity to a vCPU index (reject anything outside {0,1} with
  `INVALID_PARAMETERS`), author that vCPU's register state from the guest request
  (`elr` = entry, `x0` = ctx, `x1..3` = 0, `spsr` = EL1h with masks per the design,
  `hcr_el2` mirroring vCPU0), issue the **physical** `CPU_ON` toward
  `secondary_entry` with the vCPU index as context, spin on that core's `online`
  flag with a timeout, and return `SUCCESS`.
- Remove the temporary slice-02 `CPU_ON` call from the CPU0 init path — bring-up
  is now triggered by the guest.
- Drop the slice-03 smoke stub: `secondary_main` ends by running the real vCPU
  whose registers were authored by the PSCI handler.

ADR work lands with this slice: add a superseding ADR capturing the per-CPU
`TPIDR_EL2` model and guest-driven PSCI bring-up, flip ADR-0002
(single-global-vm-single-vcpu) to `Superseded`, and update ADR-0003 to document
the new controlled `cur_vcpu` offset and the `TPIDR_EL2` dereference.

## Acceptance criteria

- [ ] Guest DTB has a symmetric `cpu@1` PSCI node; QEMU launches `-smp 2`; DTB recompiled.
- [ ] PSCI `CPU_ON` handler maps affinity → vCPU index, rejects out-of-range with `INVALID_PARAMETERS`, authors vCPU1 regs, fires physical `CPU_ON`, and handshakes on `online`.
- [ ] The temporary CPU0-init bring-up call and the smoke stub are both removed.
- [ ] `make run` shows `CPU1: Booted secondary processor …` and `smp: Brought up 1 node, 2 CPUs`.
- [ ] `nproc` reports **2** at the busybox prompt.
- [ ] New superseding ADR added; ADR-0002 marked Superseded; ADR-0003 updated for the `TPIDR_EL2` offset.
- [ ] `make` zero warnings; entry point unchanged.

## Blocked by

- `.scratch/m3.5-smp/issues/03-vcpu1-spins-guest-smoke.md`
