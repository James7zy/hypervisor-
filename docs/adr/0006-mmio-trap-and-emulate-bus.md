# Dispatch trapped MMIO through a fixed-size bus keyed on ISV-decoded accesses

> **Status:** Accepted. **Milestone:** M3.1.

Once devices are emulated rather than passed through (see
[[0005-device-passthrough-vs-emulation]]), a guest load/store to an emulated
device's IPA faults to EL2 as a Stage-2 data abort (ESR_EL2.EC = `0x24`). The
hypervisor must turn that raw abort into a structured `{which device, offset,
size, read/write, register}` access and route it to the right emulator — without
yet owning a guest instruction decoder. The two consumers (vGICv3 and the virtio
frame) are few and known at init time.

**Decision:** Provide a small MMIO bus in
`hypervisor/arch/arm64/vmexit/mmio.c`. Regions are held in a **fixed-size array**
(`MMIO_MAX_REGIONS = 8`) of `{base, len, handler, ctx}`; emulators self-register
with `mmio_bus_register(base, len, handler, ctx)` during init. On a Data Abort,
`mmio_handle_data_abort` decodes the access **entirely from ESR_EL2.ISS**, which
requires `ISS.ISV = 1` (Instruction Syndrome Valid). When `ISV = 0` the access is
reported unsupported (logged, returns `-1`) rather than fetched-and-decoded. A
linear scan matches the faulting IPA to a region; the handler is invoked with a
`struct mmio_access`, and on a read the result is written back to the guest's
destination GPR before ELR is advanced past the instruction.

## Considered Options

- **Fixed array + linear lookup (chosen)** — M3.x has at most a handful of
  regions (GICD, GICR, one or two virtio frames); a static `[8]` array with a
  linear scan needs no allocator, no locking, and no teardown path. `8` is ample
  headroom. Cost: a hard cap, and O(n) lookup — both irrelevant at this scale.
- **Dynamic / linked-list region registry** — rejected: real allocation
  machinery for a set that is fully known at init and never grows at runtime;
  pure overhead and a teardown burden with no current consumer.
- **Decode the faulting guest instruction (ISV=0 path)** — rejected for M3.x:
  Linux's GIC and virtio MMIO accesses are simple single-register loads/stores
  that the CPU reports with `ISV = 1`, handing us access size (`SAS`), transfer
  register (`SRT`), and direction (`WnR`) for free. Building a full AArch64
  load/store decoder to service `ISV = 0` would be substantial code for cases the
  guest does not generate here; we fail closed instead (see
  [[0007-exception-dispatch-fail-stop]]).

## Consequences

- **The bus is the substrate emulated devices plug into.** vGICv3
  (`vgic_v3_mmio.c`) registers GICD and the cpu0 GICR; the virtio-console frame
  registers itself — all via `mmio_bus_register`. Adding an emulated device is a
  registration plus a handler, nothing more (the device-side decision is governed
  by [[0005-device-passthrough-vs-emulation]]).
- **Faulting IPA is reconstructed from FAR_EL2 + HPFAR_EL2**, not from the GPR
  contents: `HPFAR_EL2[43:4]` supplies IPA[47:12] (the 4 KB-aligned page), and
  `FAR_EL2[11:0]` supplies the page offset — `((hpfar & 0xFFFFFFFFFFF0) << 8) |
  (far & 0xFFF)`. This is the architectural way to get the *intermediate*
  physical address of a Stage-2 fault; FAR alone gives only the virtual address.
- **`ISV = 1` is a hard dependency**, not just an optimisation. A guest (or a
  future, less cooperative one) issuing an access the CPU reports with `ISV = 0`
  — e.g. certain load/store variants — will not be emulated; it is logged and
  the caller fail-stops. If such accesses ever appear, the ISV=0 branch must grow
  a real instruction decoder.
- **Writes from / reads to the zero register are special-cased**: `SRT = 31`
  means XZR — a write sources `0`, and a read is discarded rather than clobbering
  a real GPR.
- The cap (`8`) is shared across all VMs because there is a single bus, matching
  [[0002-single-global-vm-single-vcpu]]; a multi-VM design would need a per-VM
  bus.
