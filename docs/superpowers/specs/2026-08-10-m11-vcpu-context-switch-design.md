# M11 — vCPU Context Switch & Scheduler Design

> **Status:** Specified, not started (2026-08-10). Produced by a grilling
> session that audited the code rather than the roadmap. The audit changed the
> shape of the milestone substantially — see "Why this is not one milestone"
> below.
> **Milestone:** M11 (renumbered scheme; formerly M6). Follows M10 (multi-VM
> foundation, gate-verified 2026-07-20).
> **Stated objective (user, 2026-08-10):** *learning depth* — the primary goal
> is understanding EL2 core mechanism, not reaching the ACRN end-state fastest.
> Several decisions below resolve differently than they would under an
> engineering-throughput objective; those are marked ⚑.
> **Companion documents:** `2026-08-10-m11-vcpu-context-switch-design-zh.md`
> (Chinese translation, equivalent content) and
> `2026-08-10-m11-decision-record.md` (the reasoning behind each decision,
> including ownership and reversal conditions).

## Problem Statement

The roadmap describes M11 as adding a vCPU scheduler and "lifting the
`-mgeneral-regs-only` no-save assumption". A code audit shows this understates
the gap by a wide margin.

**There is no vCPU context switch in this codebase at all.** Not an incomplete
one — an absent one:

- `struct vcpu` holds GPRs, `SP_EL1`, `ELR/SPSR_EL2`, `HCR_EL2`, `VTTBR_EL2`
  and four vGIC registers. **Zero EL1 system registers.** A tree-wide search
  finds no save, restore, read, or write of `SCTLR_EL1`, `TTBR0_EL1`,
  `TTBR1_EL1`, `TCR_EL1`, `MAIR_EL1`, `AMAIR_EL1`, `VBAR_EL1`, `CPACR_EL1`,
  `TPIDR_EL0`, `TPIDRRO_EL0`, `TPIDR_EL1`, `CONTEXTIDR_EL1`, `ESR_EL1`,
  `FAR_EL1`, `PAR_EL1`, `SP_EL0`, `CSSELR_EL1`, `AFSR0/1_EL1`, `ELR_EL1`,
  `SPSR_EL1`, the AArch32 banked SPSRs, `MDSCR_EL1`, or `ACTLR_EL1`.
- `vgic_save()` has **zero callers** in the entire tree. Its own comment, written
  at M2, says "M2.5's timer context switch is the first user." That never
  happened; the function has been dead code across nine milestones.
- `CNTVOFF_EL2` is written as the literal constant `0` at exactly two sites and
  is not a per-vCPU field.

This is *correct by construction today*: with static 1:1 vCPU↔pCPU pinning, a
pCPU only ever runs one vCPU, so EL1 state simply persists in hardware and never
needs swapping. The saved set is exactly what EL2 exception entry itself
clobbers. The moment two vCPUs share a pCPU, every unsaved register becomes
silent, non-deterministic guest corruption — the hardest class of bug to
diagnose in a hypervisor.

Two further problems are invisible under pinning and become correctness bugs the
instant preemption is introduced:

1. **`CNTVOFF_EL2 = 0`.** A descheduled vCPU still sees the physical counter
   advance. Guest Linux computes deadlines against `CNTVCT_EL0`; after losing a
   timeslice it wakes to a jumped virtual time and can spiral in timer-interrupt
   catch-up.
2. **Live cross-pCPU data races that already exist.** `spinlock.h` documents the
   design premise that "everything else stays lock-free by being
   build-once-read-only (Stage-2) or per-CPU (vcpu/vGIC/timer)". That premise
   **stopped being true at M10**, when cross-pCPU console routing and 2-vCPU VMs
   landed. The invariant comment is stale and the code never caught up:
   - The vuart RX ring is genuinely multi-core: `vuart_rx()` advances `rx_head`
     on pCPU0 while `vuart_read()` advances `rx_tail` on the owning pCPU (which
     is not pCPU0 when console focus is VM1). Fields are neither locked nor
     `volatile`. `ris` is a non-atomic `|=`/`&=` read-modify-write racing across
     cores.
   - `g_vgicd[vmid]` is shared by both vCPUs of a VM, which can trap MMIO
     concurrently on different pCPUs. Unlocked.

   These are rare at human typing speed, which is why they have not been
   observed. Preemption widens every one of these windows from nanoseconds to
   milliseconds, and a vCPU can now be descheduled mid-MMIO-emulation.

Finally, a related gap bounds how safely any of this can be built: **no
automated test boots a real Linux guest.** All five `make test` scenarios use
bare-metal SVM guests. Linux boot, virtio-console, and boot-to-shell have
roughly 4,000 lines of specs and plans between them and zero automated
coverage; M8 and M9 were verified by hand against a kernel image at a
hardcoded developer-local path.

## Solution

Treat M11 as **eight sequenced slices**, not one milestone. Build the *detector*
before the thing it detects, clear pre-existing races before adding
non-determinism, and change exactly one variable per slice.

The ordering is driven by the learning-depth objective: the measure of a good
slice is not "how fast does this reach a working scheduler" but "how quickly
does a failure in this slice tell me which register or which mechanism I
misunderstood."

| Slice | Deliverable |
|---|---|
| **S0** | Fix the pre-existing vuart / `g_vgicd` cross-pCPU races; correct the stale `spinlock.h` invariant |
| **S1** | `svm5` EL1-state detector guest + `run_svm5_test.sh` — expected to FAIL on arrival |
| **S2** | Per-vCPU run state machine; `CPU_OFF` stops only the calling vCPU |
| **S3** | Cooperative context switch, same VM, 2 vCPUs on 1 pCPU — `svm5` goes green |
| **S4** | Cross-VM switch: `VTTBR_EL2`/VMID switching + TLB discipline |
| **S5** | Per-vCPU `CNTVOFF_EL2` |
| **S6** | FP/SIMD eager save/restore |
| **S7** | Preemptive time-slicing |
| **S8** | Automated Linux boot gate (`make test-linux`), deferred from S0 ⚑ |

The headline observable: **M10's two 2-vCPU VMs time-sliced on 2 pCPUs**, both
guests reaching interactive shells, with concurrent FP workloads in both VMs
running uncorrupted.

### Why this is not one milestone

The roadmap treats M11 as a single unit of work. The audit shows it contains at
least three independent risk surfaces — pre-existing concurrency bugs, absent
architectural state, and absent detection capability — which fail in different
ways and are debugged with different tools. Sequencing them as slices, each
changing one variable, is what makes a failure attributable. The first two
slices are not scheduler work at all; they are the preconditions that make
scheduler work *learnable* rather than merely survivable.

## User Stories

1. As a hypervisor developer, I want the vuart RX ring protected against
   concurrent access, so that console input is not silently corrupted when
   focus is on a VM whose vCPU0 does not run on pCPU0.
2. As a hypervisor developer, I want `g_vgicd[]` protected, so that two vCPUs of
   the same VM trapping GICD MMIO concurrently cannot corrupt distributor state.
3. As a hypervisor developer, I want the `spinlock.h` lock-free premise comment
   to describe what is actually true after M10, so that the next person reading
   it does not inherit a false invariant.
4. As a hypervisor developer, I want a bare-metal guest that stamps every EL1
   system register with a distinctive value, so that I have ground truth about
   what a context switch must preserve.
5. As a hypervisor developer, I want that guest to verify each register after a
   yield and name the specific register that failed, so that a missing register
   costs me one line of test output rather than a night of bisection.
6. As a hypervisor developer, I want the detector to fail when first written, so
   that I know it is actually capable of detecting the bug it exists to catch.
7. As a hypervisor developer, I want to deliberately implement save/restore for
   only half the register list first, so that I observe each failure mode
   directly rather than reading a correct list off a reference implementation. ⚑
8. As a hypervisor developer, I want to record those observed failure modes in
   `docs/debug/`, so that the reasoning is preserved the way earlier milestones
   preserved theirs.
9. As a hypervisor developer, I want per-vCPU run state (`OFF`/`RUNNING`/
   `BLOCKED`), so that vCPU lifecycle is expressible at the granularity the
   architecture actually specifies.
10. As a guest OS, I want `PSCI_CPU_OFF` to stop only the calling vCPU, so that
    offlining one CPU does not take down the entire VM.
11. As a guest Linux user, I want `echo 0 > /sys/devices/system/cpu/cpu1/online`
    to leave CPU0 running, so that standard CPU hotplug behaves per the PSCI
    specification.
12. As a hypervisor developer, I want the `CPU_OFF` fix to arrive as part of the
    state-machine slice rather than as a standalone patch, so that the fix and
    the structure it requires are designed together.
13. As a hypervisor developer, I want `HCR_EL2.TWI` to trap guest `WFI`, so that
    an idle vCPU yields its pCPU at a deterministic, reproducible point.
14. As a hypervisor developer, I want the first context switch to be cooperative
    rather than preemptive, so that a state bug reproduces at a known
    instruction instead of at an arbitrary one.
15. As a hypervisor developer, I want EL1 system-register save/restore expressed
    as C functions rather than inline assembly, so that the register list is
    readable and reviewable.
16. As a hypervisor developer, I want the assembly entry path to save only what
    EL2 entry clobbers, so that the distinction between per-exception state and
    per-switch state is explicit in the code structure.
17. As a hypervisor developer, I want per-exception cost unchanged by this
    milestone, so that an MMIO trap does not start saving 25 system registers it
    does not need.
18. As a hypervisor developer, I want `vgic_save()` to finally acquire a caller,
    so that virtual interrupt state survives a vCPU switch.
19. As a hypervisor developer, I want a pending virtual interrupt to survive its
    vCPU being descheduled and redelivered on resume, so that no interrupt is
    lost across a switch.
20. As a hypervisor developer, I want the first switching slice confined to two
    vCPUs of the *same* VM, so that Stage-2 and VMID are held constant and any
    failure is necessarily EL1 or vGIC state.
21. As a hypervisor developer, I want cross-VM switching as its own slice, so
    that `VTTBR_EL2` switching and TLB invalidation are the only new variables
    when they are introduced.
22. As a security-minded developer, I want a cross-VM switch to leave no stale
    TLB entries, so that one VM cannot observe another's translations.
23. As a hypervisor developer, I want vCPUs to stay pinned to a pCPU through all
    of the above, so that migration is not silently folded into a slice that is
    already about something else.
24. As a guest OS, I want a per-vCPU `CNTVOFF_EL2`, so that virtual time does not
    jump forward by the amount of wall-clock time I was descheduled.
25. As a guest Linux kernel, I want virtual counter continuity across
    preemption, so that I do not enter timer-interrupt catch-up after losing a
    timeslice.
26. As a hypervisor developer, I want FP/SIMD explicitly out of scope until its
    own slice, so that an early failure has exactly one candidate cause.
27. As a guest workload, I want my FP/SIMD registers (V0–V31, `FPSR`, `FPCR`)
    preserved across preemption, so that floating-point computation is not
    silently corrupted.
28. As a hypervisor developer, I want FP saved eagerly before considering lazy
    trap-based saving, so that I am not debugging a correctness bug and an
    optimization simultaneously.
29. As a hypervisor developer, I want preemptive time-slicing only after
    cooperative switching is proven, so that non-determinism is added to a
    known-good state machine.
30. As a hypervisor developer, I want two VMs × two vCPUs running on two pCPUs,
    so that the milestone's headline claim is demonstrated rather than asserted.
31. As a hypervisor developer, I want concurrent FP workloads in both VMs to
    produce correct results, so that context-switch completeness is proven under
    real contention.
32. As a hypervisor developer, I want an automated Linux-boot-to-shell gate, so
    that "the guest still boots" stops being a claim I re-verify by hand.
33. As a hypervisor developer, I want that gate outside the default fast suite,
    so that a 25-second boot does not slow the loop I run every few minutes.
34. As a hypervisor developer, I want to derive the register list myself before
    consulting bao/xvisor, so that the derivation — the actual learning — is not
    short-circuited by reading the answer. ⚑
35. As a hypervisor developer, I want to diff my list against a reference
    implementation afterwards, so that the differences become the study
    material.
36. As a hypervisor developer, I want each slice to change one variable, so that
    the source of any regression is unambiguous.

## Implementation Decisions

### Decision summary (grilled 2026-08-10)

Ownership note: only decision 1 was answered by the user directly; the rest were
resolved by Claude at the user's instruction ("answer them as you see fit").
`2026-08-10-m11-decision-record.md` carries the full reasoning, ownership marks,
and reversal conditions for each.

| # | Question | Resolution |
|---|---|---|
| 1 | Primary objective | **Learning depth** over ACRN-end-state velocity (user-stated) |
| 2 | `CPU_OFF` known defect | Not a deferred bug — it is the first slice of the state machine wearing a disguise; per-VM `off` exists *because* per-vCPU state does not |
| 3 | Detector before scheduler | Yes — `svm5` lands before any switching code |
| 4 | First switching target | Same-VM 2 vCPUs on 1 pCPU, then cross-VM; never both at once. No migration in this milestone |
| 5 | FP/SIMD | Out of scope initially, then eager. Lazy (`CPTR_EL2.TFP`) is an optimization, deferred |
| 6 | Pre-existing races | Fixed first, standalone — not folded into the scheduler |
| 7 | Automated Linux gate | ⚑ Demoted to S8 under the learning-depth objective; it protects against regression (engineering value) rather than teaching mechanism |
| 8 | Failure methodology | ⚑ Deliberately implement a partial register list first and observe the failure modes |
| 9 | Save/restore location | Assembly keeps the minimal per-exception set; EL1 bulk save/restore in C, called only on actual vCPU switch |
| 10 | Switch trigger | Cooperative `WFI` trap first; preemptive timer second |
| 11 | Reference implementations | ⚑ Consult `../bao-hypervisor` and `../xvisor` *after* the first draft, not before. Escape hatch: if blocked more than a day, read them |

### Modules built or modified

- **`struct vcpu`** grows an EL1 system-register block, a run-state enum, a
  per-vCPU `CNTVOFF_EL2`, and (at S6) an FP/SIMD block. Every field appended
  after the assembly-visible prefix; `check_offsets` gains matching entries.
- **A new context-switch module** owning `vcpu_save_el1_state()` /
  `vcpu_restore_el1_state()` and the switch entry point. This is the module the
  milestone is really about; it should be deep — a narrow interface
  (`switch_to(vcpu)`) over the full breadth of architectural state.
- **`struct vuart`** gains a lock; ring fields get correct ordering. **vGIC MMIO
  emulation state** gains a lock. **`spinlock.h`**'s premise comment is
  corrected.
- **PSCI** gains per-vCPU power-down semantics; `psci_power_down()` splits so
  `CPU_OFF` and `SYSTEM_OFF`/`SYSTEM_RESET` no longer share a path.
- **vGIC** acquires the first real `vgic_save()` call site, paired with the
  existing `vgic_restore()`.
- **vtimer** moves `CNTVOFF_EL2` from a constant written at two sites to
  per-vCPU state; the duplicated inline init in the secondary path folds back
  into the shared init.
- **Exception entry assembly** is *not* extended with the EL1 register set.

### Architectural decisions warranting ADRs

Three decisions here are of the kind the project has historically recorded, and
each should get an ADR at the slice that implements it:

- **The two-tier state split** — which registers are saved per-exception versus
  per-switch. This is the central design decision of the milestone and directly
  determines trap cost.
- **Cooperative-before-preemptive switching**, and `WFI` trapping as the yield
  point.
- **Eager-before-lazy FP/SIMD**, explicitly deferring `CPTR_EL2.TFP`.

Note also that **ADR-0002** ("single global VM, single vCPU") and **ADR-0009**
("no CI, manual verification") are both partially superseded by this work;
ADR-0009 in particular should be revisited at S8 rather than silently
contradicted.

### Interfaces

The switch interface should be a single narrow entry point taking the target
vCPU, with save/restore of the outgoing vCPU internal to it. Callers (the `WFI`
trap handler at S3, the timer tick at S7) should not need to know the register
list exists. Resist exposing `save`/`restore` as a public pair — that invites
callers to sequence them wrongly.

## Testing Decisions

### What makes a good test here

A good test observes **only what a guest can observe**. The failure mode this
milestone must catch is "a register the hardware clobbers that nobody listed" —
which no test of the implementation can find, because the implementation's
register list *is* the bug. Only a guest that writes state, yields, and reads it
back can detect an omission. Correspondingly, a test asserting that
`vcpu_save_el1_state()` writes some specific struct field would be testing the
list against itself and would pass while the system is broken.

### Primary seam: `svm5`, a bare-metal EL1-state detector guest

One seam, matching existing prior art exactly. Five scenarios already follow
this shape (`run_svm_test.sh` through `run_shell_test.sh`): build a guest
`.bin`, boot it under QEMU, `grep -qF` fixed strings from serial output. `svm5`
adds a sixth `HV_GUEST` profile and one script; no new harness, no new machinery.

Behavior: stamp every EL1 system register with a distinctive per-register
value, yield (`WFI`), and on resume verify each one, printing a line naming any
register that failed. The test asserts a success string is present and that no
mismatch marker appears — the same present/absent assertion pattern
`run_shell_test.sh` already uses.

**This test is expected to fail when first written.** That failure is the
deliverable of S1: it proves the detector can see the bug. Under decision #8 it
should also be run against a deliberately half-complete register list, and the
observed failure modes recorded in `docs/debug/`.

Alternatives considered and rejected:
- **C unit tests of the save/restore functions** — tests implementation, cannot
  catch an omitted register (see above).
- **Boot two Linux guests and watch for corruption** — 25+ seconds,
  non-deterministic, and a corrupted `TTBR0_EL1` surfaces as an opaque kernel
  panic rather than a register name. Valuable as an integration gate (S8),
  useless as a detector.

### Additional coverage per slice

- **S0** needs a race test, and this is the one genuinely hard testing problem
  in the milestone: the existing scenarios cannot reliably reproduce a
  nanosecond window. Options are a stress scenario driving console input while
  both VMs trap GICD MMIO, or accepting review-based verification for S0 and
  relying on the absence of regressions elsewhere. **This is the weakest point
  in the test plan and should be resolved at implementation time rather than
  papered over.**
- **S2** has a ready-made assertion: `echo 0 > /sys/devices/system/cpu/cpu1/online`
  must leave CPU0 running. Currently a manual repro; it should become automated
  — though note it requires a Linux guest, which is exactly what S8 provides.
  If S8 stays deferred, S2's automated coverage is limited to an SVM-level
  equivalent.
- **S4** needs its own assertion, because the `svm5` seam only sees what a
  single guest can see: two VMs each stamping a distinct value at the same IPA,
  verifying isolation after switching.
- **S6** extends `svm5` with an FP/SIMD stamp-and-verify block.
- **S7** should assert forward progress in both VMs concurrently, not merely
  absence of corruption.
- **`check_offsets`** continues to guard asm/C drift and gains entries as
  `struct vcpu` grows.

## Out of Scope

- **vCPU migration between pCPUs.** vCPUs stay pinned throughout. Migration is a
  separate concern and folding it in would violate the one-variable rule.
- **Lazy FP/SIMD via `CPTR_EL2.TFP`.** Explicitly deferred as an optimization.
- **vCPU count exceeding pCPU count beyond the 2×2-on-2 headline.** Overcommit
  beyond that ratio is not a goal here.
- **Any scheduling policy beyond simple round-robin time-slicing.** Priorities,
  fairness, and real-time guarantees are later concerns.
- **Hypercall ABI, VM lifecycle, HSM, Device Model, virtio** — M12–M14.
- **RK3588 / runtime FDT parsing** — M15.
- **SMMU/DMA isolation** — deferred per the standing roadmap decision.
- **Debug/trace register state** (`MDSCR_EL1` and the breakpoint/watchpoint
  banks) beyond what a guest observably requires.
- **AArch32 guest support.** The banked AArch32 registers are listed as unsaved
  for completeness, but guests are AArch64-only.

## Further Notes

**On the roadmap's framing.** The roadmap's M11 entry says the milestone "lifts
the `-mgeneral-regs-only` no-save assumption". That phrasing implies FP/SIMD is
the gap. It is not — FP/SIMD is slice six of eight, and the invariant itself
(EL2 never emits FP instructions) remains correct and should stay. The real gap
is that vCPU context switching does not exist.

**On the objective marks (⚑).** Three decisions resolve on learning-depth
grounds and would resolve differently under an engineering-throughput
objective: deferring the Linux gate (7), deliberately shipping a half-complete
register list first (8), and delaying reference-implementation reading (11). If
the objective changes, revisit those three first. Decision 7 in particular was
reversed mid-grilling once the objective was stated — it is the most likely to
warrant re-reversal.

**On S0 and the learning objective.** S0 is pure engineering hygiene and teaches
no EL2 mechanism; a defensible counter-argument is that it delays the actual
learning content. It is retained on the grounds that debugging a system with
both known races and unknown context-switch bugs spends more time excluding
interference than understanding ARM state — learning *efficiency* is the
justification, not code quality.

**On the blocked sub-problem in S8.** `LINUX_RUN.sh` hardcodes a
developer-local absolute path to a kernel image. Any automated Linux gate needs
a reproducible image first, and that is an unresolved decision: check in a
trimmed Image, script the kernel build, or document a fetch. It should be
settled before S8 is scheduled.

**On the memory constraint.** The kernel Image address budget (~31 MB, per
CLAUDE.md) and the `-m 4G` requirement are unchanged by this milestone but
constrain any S8 image choice.

**Publication note.** This spec follows the project convention of
`docs/superpowers/specs/`. It has not been filed as a GitHub Issue on
`James7zy/hypervisor-`; no triage label vocabulary was configured for this
session, and filing an issue is left to the maintainer.
