# Architecture Decision Records (ADRs)

This directory records the **architecturally significant decisions** made while
building this hypervisor: choices that constrain the structure of the code, are
costly to reverse, or that a future reader (or a future milestone) would
otherwise have to reverse-engineer from the source.

An ADR captures *why* a decision was made and what was rejected — not *how* the
code currently works (that is the code's job) nor *what* a milestone set out to
build (that is the spec's job; see [ADRs vs specs](#adrs-vs-specs)).

## When to write an ADR

Write one when a decision:

- constrains the shape of multiple modules (e.g. the single-`g_vm` model), or
- is a deliberate trade-off with a real alternative that was rejected, or
- is a known shortcut / technical debt a later milestone will revisit, or
- encodes a non-obvious invariant that code alone does not make safe.

Do **not** write an ADR for routine implementation detail, for facts already
obvious from the code, or for something fully covered by a milestone spec.

## Numbering

- Files are named `NNNN-kebab-case-title.md`, zero-padded to four digits,
  allocated sequentially and never reused (`0001-…`, `0002-…`, …).
- `0000` is reserved for this README's conventions; the first real decision is
  `0001`.
- Numbers are immutable once merged. A decision that overturns an earlier one
  gets a **new** number and marks the old one `Superseded` (see below) — we do
  not edit history in place.

## Status vocabulary

Every ADR opens with a one-line status blockquote:

> **Status:** Accepted. **Milestone:** M2.5.

Allowed statuses:

| Status | Meaning |
| --- | --- |
| `Proposed` | Under discussion; not yet reflected in the code. |
| `Accepted` | Decided and in force; the code reflects it. |
| `Superseded by ADR-NNNN` | Replaced by a later decision. Keep the file; add the pointer. |
| `Deprecated` | No longer in force, but not directly replaced (e.g. the constraint was dropped). |

When a milestone is known to be on a collision course with a decision (e.g. SMP
will overturn the single-vCPU model), note it in the ADR's *Consequences* as a
forward reference — but leave the status `Accepted` until the superseding ADR
actually lands.

## Template

Copy [`template.md`](template.md) for a new record. The minimal shape is:

```markdown
# <Imperative one-line decision title>

> **Status:** Accepted. **Milestone:** M<x>.

<2–5 sentences: the forces / problem that made a decision necessary.>

**Decision:** <what we chose, stated plainly.>

## Considered Options

- **<Option>** — rejected/chosen because <reason>.

## Consequences

- <What this makes easier, harder, or constrains downstream.>
```

`0001-vtimer-hardware-forwarding.md` is a good worked example of this shape.

Every ADR should also carry **at least one Mermaid diagram** — a picture is worth
a thousand words. Choose the type that fits the decision: a `sequenceDiagram` for a
flow over time (e.g. 0001, 0006, 0011), a `flowchart`/`graph` for dispatch trees,
address maps or per-device routing (e.g. 0004, 0005, 0007, 0008, 0009), or a
`classDiagram` for struct/object relationships (e.g. 0002, 0003, 0010). The
template carries a placeholder block to fill in.

## ADRs vs specs

This repo also keeps per-milestone design documents under
`docs/superpowers/specs/` and execution plans under `docs/superpowers/plans/`.
They overlap with ADRs but serve different roles:

| Artifact | Scope | Lifetime | Answers |
| --- | --- | --- | --- |
| **Spec** (`specs/`) | One milestone | Snapshot at design time | *What* are we building this milestone, and how? |
| **Plan** (`plans/`) | One milestone | Consumed during execution | In what order do we build it? |
| **ADR** (`adr/`) | One decision, cross-cutting | Living; superseded, not deleted | *Why* is the system shaped this way? |

Rule of thumb: if a decision outlives the milestone that introduced it and
constrains code written in *later* milestones, it belongs in an ADR — even if it
was first described in a spec. ADRs are the durable, indexed source of truth for
"why is it like this"; specs are the source of truth for "what did M<x> do".

## Index

| ADR | Title | Status |
| --- | --- | --- |
| [0001](0001-vtimer-hardware-forwarding.md) | Forward the virtual-timer PPI with `ICH_LR.HW=1` | Accepted (M2.5) |
| [0002](0002-single-global-vm-single-vcpu.md) | Model the guest as a single global VM with a single vCPU | Superseded by [0013](0013-smp-per-cpu-tpidr-guest-driven-bringup.md) (M3.5) |
| [0003](0003-asm-c-vcpu-offset-coupling.md) | Couple assembly to the vCPU layout via hand-maintained offset macros | Accepted (M1) |
| [0004](0004-stage2-static-1gb-block-mapping.md) | Map guest Stage-2 with two static 1 GB block descriptors | Accepted (M1; refined M3.x) |
| [0005](0005-device-passthrough-vs-emulation.md) | Split devices into passthrough (PL011) vs trap-and-emulate (GIC, virtio) | Accepted (M3.0–M3.3) |
| [0006](0006-mmio-trap-and-emulate-bus.md) | Dispatch trapped MMIO through a fixed-size bus keyed on ISV-decoded accesses | Accepted (M3.1) |
| [0007](0007-exception-dispatch-fail-stop.md) | Dispatch EL2 exceptions on `ESR.EC` and fail-stop (park) on anything unhandled | Accepted (M3.1; HVC/PSCI from M1.5) |
| [0008](0008-static-board-config-vs-fdt.md) | Drive the platform from compile-time board constants, not a parsed FDT | Accepted (M3.0; revisited M4) |
| [0009](0009-no-ci-manual-verification.md) | Verify with a three-step manual gate, not an automated test suite | Accepted (M0) |
| [0010](0010-vgic-scope-cpu0-lr0-group1.md) | Scope the vGICv3 to one cpu0 redistributor, one list register, Group 1 | Accepted (M2/M3.2) |
| [0011](0011-virtio-mmio-v2-modern-only.md) | Implement virtio-mmio as modern (VERSION 2) only, no legacy | Accepted (M3.3) |
| [0012](0012-physical-gicv3-ownership.md) | Let the hypervisor own the physical GICv3; the guest sees only a virtual GIC | Accepted (M2.5; extended M3.0/M3.2) |
| [0013](0013-smp-per-cpu-tpidr-guest-driven-bringup.md) | Make EL2 per-CPU via `TPIDR_EL2`, bring secondaries up guest-driven, route IPIs through a kick-SGI | Accepted (M3.5) |
| [0014](0014-multi-vm-static-partition-el2-console.md) | Objectify the VM, statically partition 2 VMs across 4 pCPUs, move the console under EL2 | Accepted (M5) |
| [0015](0015-arch-boundary-core-vs-arch.md) | arch/ 以外的代码只能通过 `*_arch_*` 钩子访问架构层 | Accepted (M11 前置);修订 0003 的偏移宏位置 |
