# Make EL2 per-CPU via `TPIDR_EL2`, bring secondaries up guest-driven, and route IPIs through a physical kick-SGI

> **Status:** Accepted. **Milestone:** M3.5 (SMP). Supersedes [[0002-single-global-vm-single-vcpu]].

M3.5 turns the single-vCPU machine into a **2-vCPU SMP guest** (an unmodified
`-smp 2` Linux that reaches `nproc = 2`), with the two vCPUs **statically pinned
1:1** to two physical CPUs and **no scheduler**. The single-`g_vm`/single-vCPU
model ([[0002-single-global-vm-single-vcpu]]) breaks the instant a second core
runs EL2 in parallel: every "the guest is `g_vm.vcpu`" assumption — the
`adrp g_vm` in the exception-entry asm, the one-redistributor vGIC, the
unconditional inject target — is now wrong on the secondary. This ADR records the
cluster of decisions that replace that model. They are recorded together because
they are interlocking: none works without the others.

**Decision:**

1. **Per-CPU state via `TPIDR_EL2`.** A static `struct percpu percpu[NR_CPUS]`
   (`NR_CPUS = 2`) holds each core's current vCPU pointer, id, and `online` flag.
   Each pCPU writes `&percpu[id]` into its own `TPIDR_EL2` at entry; the
   exception-entry asm reaches the current vCPU through it, not through `g_vm`
   (see [[0003-asm-c-vcpu-offset-coupling]], upgraded for this).
2. **Guest-driven secondary bring-up.** The guest's PSCI `CPU_ON` (HVC) traps to
   EL2, which authors the target vCPU's boot state and issues a **physical**
   `smc PSCI_CPU_ON` to QEMU firmware to power the secondary pCPU on at our EL2
   `secondary_entry`. The hypervisor does not bring cores up on its own.
3. **Cross-core IPI via a physical kick-SGI.** Guest `ICC_SGI1R_EL1` writes trap
   to EL2 (`ICH_HCR_EL2.TC = 1`); the sender records the pending vSGI in the
   target vCPU's shared bitmap and fires a reserved physical "kick" SGI at the
   target pCPU, which drains the bitmap and injects into its own list register.
4. **Per-CPU vGIC/timer; lock only what is shared.** Each pCPU owns its
   redistributor, timer and `ICH_*`/LR state (no sharing → no lock). The only
   cross-core-written state — the SGI pending bitmap and the PL011 console —
   is guarded by a minimal `__atomic` ticket spinlock.

## Secondary bring-up and the IPI path

```mermaid
sequenceDiagram
    participant GL as Guest Linux (vCPU0 / pCPU0)
    participant PS as psci_handle (EL2, pCPU0)
    participant FW as QEMU PSCI firmware
    participant SE as secondary_entry → secondary_main (EL2, pCPU1)
    participant V1 as vCPU1 (guest EL1 / pCPU1)

    GL->>PS: HVC PSCI_CPU_ON(targetAff, entry, ctx)
    Note over PS: map Aff→vCPU idx; author vcpu[idx].regs<br/>(elr=entry, x0=ctx, spsr=EL1h); mirror hcr/vttbr
    PS->>FW: smc PSCI_CPU_ON(secondary_entry, ctx=idx)
    FW-->>SE: power on pCPU1 at EL2
    Note over SE: TPIDR_EL2=&percpu[1]; per-CPU GIC/timer init;<br/>stage2_activate → vgic_restore → publish online
    PS-->>GL: x0 = SUCCESS (after online handshake)
    SE->>V1: vcpu_run(&vcpu[1])

    Note over V1,GL: later — an IPI from vCPU1 to vCPU0
    V1->>SE: write ICC_SGI1R_EL1 (traps, ICH_HCR_EL2.TC=1)
    SE->>SE: set vcpu0 pending bit (SGI spinlock); physical kick-SGI → pCPU0
    Note over PS: pCPU0 EL2 IRQ handler drains bitmap,<br/>injects vINTID into its own LR, deactivates kick
```

## Considered Options

- **Per-CPU via `TPIDR_EL2` (chosen)** — one `mrs tpidr_el2 ; ldr [#offset]` on
  the entry path, no global lookup, and the asm is identical on every core. Cost:
  a new controlled struct offset the asm depends on (pinned by `_Static_assert`).
- **Keep `g_vm` and branch on MPIDR in the asm** — rejected: it puts a runtime
  affinity read and a branch on the hottest path, and still needs per-CPU storage
  somewhere; `TPIDR_EL2` is exactly the architectural slot for this.
- **Guest-driven PSCI `CPU_ON` (chosen, 2b)** vs **hypervisor-initiated bring-up
  (2a)** — 2a is simpler but bypasses the guest's PSCI contract (`maxcpus`,
  hotplug). 2b preserves real virtualization semantics and reuses the M1.5 PSCI
  HVC trap path; the QEMU `virt,virtualization=on` config has no EL3, so EL2 must
  itself `smc` QEMU firmware to power the secondary on.
- **IPI by polling the bitmap at the next exit** — rejected: an IPI would stall
  until the target's next timer tick. The physical kick-SGI forces the target
  into EL2 promptly, reusing the M2.5 "physical IRQ → EL2 → inject" path.
- **A full scheduler / overcommit** — out of scope: M3.5 is static 1:1 pinning,
  `NR_CPUS = 2`. Time-sharing vCPUs across pCPUs is a later milestone.

## Consequences

- **`NR_CPUS = 2` is a compile-time ceiling.** `percpu[]`, the vGIC redistributor
  array, the SGI bitmap and the secondary stacks are all sized by it. Going beyond
  2 (or to dynamic topology) revisits this ADR.
- **The exception-entry asm now depends on `percpu.cur_vcpu` being the first
  field** (offset `PERCPU_CUR_VCPU == 0`), pinned by a `_Static_assert` — this is
  the SMP successor to the `&g_vm == &g_vm.vcpu.regs` trick; see
  [[0003-asm-c-vcpu-offset-coupling]].
- **The QEMU PSCI target is the GICv3 *affinity* (`mp_affinity`, == cpu index for
  < 16 cores), not the raw `MPIDR_EL1` read** (which has bit 31 RES1, reading
  `0x80000000`). Passing the raw value returns `INVALID_PARAMETERS`. Encoded in
  `psci_cpu_on_guest`.
- **`scripts/run-qemu.sh` uses `-smp 2`** so QEMU actually creates pCPU1 for the
  physical `CPU_ON` to target; the guest still sees one CPU until its DTB gains a
  `cpu@1` node.
- **Trapping `ICC_SGI1R_EL1` via `ICH_HCR_EL2.TC` traps more than the SGI
  registers on QEMU.** QEMU's `gicv3_irqfiq_access` also traps `ICC_PMR_EL1`,
  `ICC_CTLR_EL1`, `ICC_RPR_EL1` under `TC=1`. The EL2 trap handler must decode the
  trapped register and **emulate PMR/CTLR/RPR against the guest's virtual
  interface in `ICH_VMCR_EL2`** (VPMR / VEOIM / VCBPR) — never the physical
  `ICC_*`, which is EL2's own interface. Dropping these writes silently
  mis-masks the secondary's interrupts and wedges bring-up; this was the
  load-bearing M3.5 debugging finding. (`handle_sysreg_trap` in `vmexit.c`.)
- **vtimer on the secondary is VENG1-gated:** a guest may arm its physical vtimer
  before enabling its virtual GIC interface; a HW-forwarded timer that fires then
  would wedge Active. When the guest's `VENG1 = 0` the handler software-injects
  and masks the physical PPI, re-enabling it when the guest enables its virtual
  PPI. Extends [[0001-vtimer-hardware-forwarding]] to SMP.
- **Concurrency is minimal by construction:** Stage-2 is build-once-read-only;
  vGIC/timer/vCPU state is per-CPU; only the SGI bitmap and printk take the
  spinlock. The repo's first synchronization primitive (`spinlock.h`) is a plain
  `__atomic` acquire/release ticket lock — an earlier `WFE`/`SEV` form had an
  event-register race and was replaced.
- The single-redistributor vGIC of [[0010-vgic-scope-cpu0-lr0-group1]] is
  generalized to one redistributor per vCPU (with a correct 64-bit `GICR_TYPER`
  so a secondary can match its redistributor by affinity).
- **M5 (multi-VM) re-scopes guest-driven bring-up per-VM** (see
  [[0014-multi-vm-static-partition-el2-console]]): `psci_cpu_on_guest` resolves
  the target vCPU and physical pCPU via `current_vcpu()->owner` and that VM's
  `config->pcpu_base`, so each VM's own PSCI `CPU_ON` only ever targets that
  VM's own pCPU slot. A VM's *first* vCPU is the one exception to "guest-driven
  bring-up": since no guest exists yet for a VM before its boot vCPU is
  running, that one boot vCPU is powered on by the hypervisor itself (a
  fire-and-forget physical `PSCI_CPU_ON` issued by CPU0 at hv-init time) —
  guest-driven `CPU_ON` remains how every VM's *second* vCPU comes up.
