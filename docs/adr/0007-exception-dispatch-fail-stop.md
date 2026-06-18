# Dispatch EL2 exceptions on ESR.EC and fail-stop (park) on anything unhandled

> **Status:** Accepted. **Milestone:** M3.1 (dispatch framework). HVC/PSCI sub-dispatch predates it (M1.5).

When the guest traps to EL2, `el1_sync_handler` saves the guest frame and calls
`handle_exit(regs, esr)`. That function is the single decision point for every
synchronous exit, and it must answer two questions: which class of exit is this,
and what happens when the exit is one we do not (yet) handle? A learning
hypervisor that silently guesses at unrecognised exits hides bugs; one that
keeps running past a fault it could not service corrupts guest state invisibly.

**Decision:** Dispatch on `ESR_EL2.EC` (bits `[31:26]`):

- `EC = 0x16` (HVC from AArch64 EL1) → `handle_hvc`.
- `EC = 0x24` (Data Abort from a lower EL) → `mmio_handle_data_abort`
  ([[0006-mmio-trap-and-emulate-bus]]); on success the abort is consumed and we
  return to the guest.

Anything else — an unrecognised `EC`, **or** a Data Abort that the MMIO bus
declines (no matching region, or `ISV = 0`) — falls through to a **fail-stop**:
print a one-line diagnostic (`EC`, `ESR`, `ELR`) and enter an infinite `wfi`
loop. This is a deliberate park, not recovery. HVC is itself **two-level**:
`handle_hvc` first routes by SMCCC owner — function-ID byte `0x84` (32-bit) or
`0xC4` (64-bit) is **PSCI**, dispatched to `psci_handle` — and only otherwise
switches on the specific function ID, replying `SMCCC_NOT_SUPPORTED` for unknown
ones.

## Considered Options

- **Fail-stop / park on unhandled exits (chosen)** — the safest default while the
  trap surface is still being built out: an unserviceable exit halts loudly with
  a diagnostic instead of returning to a guest in an undefined state. Easy to
  spot under QEMU and GDB; impossible to mistake for forward progress.
- **Inject the fault back into the guest (EL1 fault path)** — rejected for now:
  correct long-term behaviour for some aborts, but it requires synthesising
  guest exception state (ESR_EL1/FAR_EL1/vector entry) and presumes the guest can
  cope, which masks hypervisor bugs during bring-up.
- **Skip the instruction and continue** — rejected outright: advancing past an
  exit we did not understand is the worst option — silent guest corruption with
  no signal.

## Consequences

- **The park is a safety property, not a stub to be optimised away.** It is
  tempting to "fix" the infinite `wfi` into a `return`/continue to make a hang go
  away; doing so converts a loud, debuggable halt into silent guest corruption.
  Any change here must replace the park with a *real* handler (emulate, or inject
  to the guest), never with a bare continue.
- Unknown **HVC** function IDs are non-fatal — they return `SMCCC_NOT_SUPPORTED`
  per the SMCCC contract — whereas unknown **EC** classes are fatal (park). The
  asymmetry is intentional: an unknown hypercall is a defined ABI outcome; an
  unknown exit class is an unmodelled gap in the hypervisor.
- The HVC/PSCI two-level dispatch predates the M3.1 framework (it arrived with
  PSCI in M1.5); M3.1 generalised the top-level `handle_exit` to add the Data
  Abort/MMIO arm alongside it.
- The `0x24` arm advances `ELR_EL2` past the faulting load/store only on
  successful emulation; the data-abort handler owns that advance (see
  [[0006-mmio-trap-and-emulate-bus]]), so a declined abort never resumes the
  guest mid-instruction — it parks.
- Because there is a single global guest ([[0002-single-global-vm-single-vcpu]]),
  a park halts the whole system; with multiple VMs this policy would need to
  scope the halt to the offending VM rather than the machine.
