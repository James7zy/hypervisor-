# Verify with a three-step manual gate, not an automated test suite

> **Status:** Accepted. **Milestone:** M0.

A bare-metal EL2 hypervisor is awkward to unit-test: most of its behaviour is
only observable on real (or emulated) ARM hardware after a context switch into a
guest, and there is no host runtime to link a test framework against. Standing up
CI and a test harness for a single-developer learning project would be
significant scaffolding before there is much to assert on. But "no tests" must
not mean "no verification" — there still has to be a defined bar a change clears
before it is considered done.

**Decision:** Adopt an explicit, repeatable **three-step manual verification
gate** as the quality bar, documented in `CLAUDE.md`, with no automated suite or
CI:

1. **Build check** — `make` succeeds with **zero warnings** (`-Werror` is on, so
   any warning is a hard failure).
2. **Static inspection** — `aarch64-none-linux-gnu-readelf -h build/hypervisor.elf`:
   the entry point must be `0x40080000` and `.text` must start at `0x40080000`.
3. **Run + observe** — `make run` prints the expected EL2 banner
   (`[hv] Hello from EL2, …`) within a few seconds, and at M3.4 boots through to
   an interactive busybox shell.

## Considered Options

- **Three-step manual gate (chosen)** — matches what can actually be asserted
  without host-side mocking, is fast, and the `readelf` step cheaply pins the two
  invariants (entry point and load address) that a linker-script slip would
  otherwise silently break. Cost: everything past "it booted and printed" is
  unguarded.
- **Automated unit/integration suite + CI** — rejected for now: little of the
  codebase is exercisable off-target, and the harness would cost more than it
  returns at this stage. Worth revisiting once there is portable logic
  (virtqueue manipulation, MMIO decode, PSCI dispatch) that can run against a
  host-side test double.

## Consequences

- **This is a recorded decision, not merely an absence.** The gate is executed on
  every change, so it has the force of a policy; documenting it stops it from
  silently eroding into "I built it once."
- **Untested paths carry real regression risk.** The clearest example is the
  hand-maintained assembly/C offset coupling in
  [[0003-asm-c-vcpu-offset-coupling]]: reordering a `struct vcpu` field
  mis-aligns the context switch with no compile error and no test to catch it —
  only step 3 (run + observe) might surface it, and only if the corruption
  happens to be observable. Reviewers must compensate by inspection.
- An automated harness becomes worthwhile when (a) portable, off-target logic
  accumulates enough to assert on, or (b) the project gains contributors who need
  a mechanical signal rather than reviewer judgement. At that point this ADR
  should be superseded.
