# Wake pCPU1 to EL2 (no guest yet)

Status: ready-for-agent

## Parent

Plan: `docs/superpowers/plans/2026-06-23-m3.5-smp.md` — Slice 2.
Design: `docs/superpowers/specs/2026-06-23-m3.5-smp-design.md`.

## What to build

Bring the second physical core up to EL2 under hypervisor control and prove it
with a handshake, without running any guest on it yet.

- Add a minimal spinlock primitive (ticket or LDAXR/STXR test-and-set, with the
  required barriers). It is not exercised heavily yet but is introduced here so
  later slices can depend on it.
- Replace the `secondary_park` stub in the boot asm with a `secondary_entry`
  label reached at EL2 (QEMU PSCI passes the context id in `x0`). It sets a
  per-CPU stack pointer (indexed by context id), `VBAR_EL2`, `TPIDR_EL2` to this
  core's per-CPU slot, masks DAIF with the appropriate barriers, then calls a C
  `secondary_main(id)`.
- `secondary_main` performs per-CPU EL2 init in order: enable `ICC_SRE_EL2`;
  wake this core's GICR (clear ProcessorSleep, wait for ChildrenAsleep to clear)
  and enable the kick-SGI INTID + the vtimer PPI (INTID 27) on this redistributor;
  arm the vtimer; zero `CNTVOFF_EL2`. It then publishes `online = 1` (with a
  barrier after setting `cpu_id`) and parks in a `wfi` loop (temporary; replaced
  in a later slice).
- CPU0 issues a physical PSCI `CPU_ON` targeting `secondary_entry`'s physical
  address with context id 1, then spins on `percpu[1].online` with a timeout
  print. For this slice the call is made once from the CPU0 init path purely to
  prove the bring-up; it moves into the guest-driven PSCI handler in slice 04.
- Introduce the board-level constants this needs: a kick-SGI INTID
  (`BOARD_KICK_SGI`) and a way to resolve the `secondary_entry` physical address.

The CPU0 / UP guest path must stay untouched and still boot.

## Acceptance criteria

- [ ] Spinlock header exists with lock/unlock and correct barriers.
- [ ] `secondary_park` is gone; `secondary_entry` brings pCPU1 to EL2 with its own stack, `VBAR_EL2`, and `TPIDR_EL2`.
- [ ] `secondary_main` does per-CPU GICR wake + SRE + vtimer/PPI enable in the documented order, then publishes `online` and parks in `wfi`.
- [ ] CPU0 fires physical `CPU_ON(ctx=1)` and the handshake on `percpu[1].online` succeeds.
- [ ] `make run` prints `[hv] pCPU1 online` and CPU0's handshake message.
- [ ] The existing UP guest still boots to `~ #` (CPU0 path unchanged).
- [ ] `make` zero warnings; entry point unchanged.

## Blocked by

- `.scratch/m3.5-smp/issues/01-percpu-foundation-tpidr.md`
