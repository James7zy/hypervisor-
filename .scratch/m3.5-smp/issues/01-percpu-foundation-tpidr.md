# Per-CPU foundation via TPIDR_EL2 (UP, behavior unchanged)

Status: ready-for-agent

## Parent

Plan: `docs/superpowers/plans/2026-06-23-m3.5-smp.md` — Slice 1.
Design: `docs/superpowers/specs/2026-06-23-m3.5-smp-design.md`.

## What to build

A pure, behavior-preserving refactor that introduces a per-CPU data model and
makes `TPIDR_EL2` the single source of truth for "which vCPU is current on this
core" — before any second core exists.

Today the exception-entry asm reaches the guest frame by taking the address of
the single global `g_vm` (relying on `g_vm.vcpu` being the first field, see
ADR-0003). That coupling does not scale to a second vCPU. Replace it with a
per-CPU structure pointed to by `TPIDR_EL2`:

- A new per-CPU type holding at least `cur_vcpu`, `cpu_id`, and a `volatile`
  `online` flag, with `NR_CPUS == 2` and an array of them.
- A controlled, asserted offset constant for the `cur_vcpu` field (first field),
  guarded by a `_Static_assert`, mirroring the ADR-0003 asm/C offset-coupling
  pattern.
- An inline `current_vcpu()` that reads `TPIDR_EL2` and dereferences `cur_vcpu`.
- CPU0 initializes its per-CPU slot (`cur_vcpu`, `cpu_id = 0`) and sets
  `TPIDR_EL2` to point at it **before** the first guest entry.
- The four `adrp/add g_vm` sites in the vmexit and IRQ-handler entry asm switch
  to `mrs TPIDR_EL2` + load-at-offset, with comments updated to drop the
  "first field of first field" rationale.

This is a single-core refactor: the system must still be a UP `-smp 1` guest at
the end. Whether to keep `g_vm.vcpu` or already widen to a `vcpu[NR_CPUS]` array
is the implementer's call, as long as the per-CPU pointer is the asm's only path
to the current vCPU and UP boot does not regress. **Land this as its own commit.**

## Acceptance criteria

- [ ] Per-CPU struct, `NR_CPUS == 2`, per-CPU array, and `current_vcpu()` exist.
- [ ] `_Static_assert` pins the `cur_vcpu` field offset used by asm; build fails if it drifts.
- [ ] CPU0 sets `TPIDR_EL2` to its per-CPU slot before the first guest entry.
- [ ] All four asm entry sites (vmexit + IRQ handler) reach the current vCPU via `TPIDR_EL2`, not `g_vm`.
- [ ] `make` succeeds with zero warnings (`-Werror`); `readelf -h` entry is `0x40080000`.
- [ ] `make run` (still `-smp 1`) boots Linux to `~ #`; `ls`/`uname` work.
- [ ] Committed alone as the first M3.5 commit.

## Blocked by

None - can start immediately.
