# Map guest Stage-2 with two static 1 GB block descriptors

> **Status:** Accepted. **Milestone:** M1 (introduced); refined through M3.x.

The single Linux guest needs a Stage-2 (IPA→PA) translation that gives it RAM and
lets it reach the low-address peripheral window of QEMU `virt`. A full
demand-paged page-table walker (L1→L2→L3, per-page descriptors, fault-driven
population) is real hypervisor machinery, but for one guest with a fixed memory
map it is far more than the milestones up to M3.4 need, and it would bury the
EL2/Stage-2 mechanics under allocator and walker code.

**Decision:** Use a single statically-allocated L1 table (`l1_table[512]`,
4 KB-aligned) with the translation **starting at L1** (`VTCR_EL2.SL0=1`,
`T0SZ=25` → 39-bit IPA, 4 KB granule). Populate exactly **two 1 GB block
descriptors**, no L2/L3 tables:

- `l1_table[0]`: IPA `0x0000_0000–0x3FFF_FFFF` → **identity**, Device-nGnRE, XN.
  Covers the QEMU `virt` low-peripheral window (PL011 @ `0x0900_0000`,
  GIC @ `0x0800_0000`, virtio-mmio @ `0x0A00_0000`).
- `l1_table[1]`: IPA `0x4000_0000–0x7FFF_FFFF` → **non-identity** to `ram_pa`,
  Normal Write-Back, Inner-Shareable. Guest RAM. The 1 GB block output must be
  1 GB-aligned; `ram_pa` (`0x4800_0000`) is.

VMID goes in `VTTBR_EL2[63:48]`; the L1 table base in the low bits (the table is
4 KB-aligned so `VTTBR_EL2[11:0]` are zero as required).

## Considered Options

- **Two static 1 GB blocks, L1 only (chosen)** — two writes, no walker, no
  allocation; the whole guest address space is two lines of code and is trivial
  to reason about under GDB. Cost: coarse — a 1 GB block is the smallest unit, so
  per-page permissions or carving sub-regions is impossible without subdividing.
- **Full multi-level page table with demand paging** — rejected for now: the
  production answer (ACRN/Xvisor have it) but unjustified for one guest with a
  static map; revisit when there are multiple VMs or memory regions that need
  page-granular control.
- **Identity-map RAM as well** — rejected: the hypervisor image loads at PA
  `0x4008_0000`, inside the guest's IPA RAM window `0x4000_0000+`. RAM is
  therefore mapped **non-identity** to a dedicated `ram_pa` that does not overlap
  the hv image.

## Consequences

- **The whole low 1 GB is identity-mapped Device, including the GIC and virtio
  IPAs — yet those still trap and get emulated.** Trapping does *not* come from
  leaving those IPAs unmapped; it comes from there being **no physical device
  behind those PAs for the guest** (the hypervisor owns the physical GIC; there
  is no physical virtio-console). A guest access faults and is taken to EL2 as a
  Stage-2 data abort, which the MMIO bus then dispatches. PL011's PA *does* back
  a real device, so its identity mapping is genuine passthrough. This split is
  the subject of [[0005-device-passthrough-vs-emulation]].
- Granularity is 1 GB. If a future need requires page-granular Stage-2 (e.g.
  protecting a sub-region, or true MMIO "holes"), `l1_table[0]` must be
  subdivided into an L2/L3 subtree. That work is not yet present.
- **Doc drift to fix in a later task:** `docs/stage2.md` §5 predicted that M3.1
  would "carve a hole" by splitting the L1 block into L2/L3 so GIC accesses
  fault. The code did **not** take that path — it relies on the absent-PA-device
  fault described above and the two blocks are unchanged. `docs/stage2.md` should
  be reconciled with this ADR.
- Single L1 table ↔ single VMID ↔ single guest; this is one of the load-bearing
  consequences of [[0002-single-global-vm-single-vcpu]].
