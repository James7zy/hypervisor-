---
name: roadmap
description: Milestone roadmap (M0–M15) and the ACRN-model strategy decisions for this ARM64 hypervisor — milestone status, goals, the 2026-08-10 renumbering table, and the standing ordering decisions. Use when planning a milestone, checking what is done vs planned, or reading historical docs that cite old milestone numbers.
---

# Milestone Roadmap

| Milestone | Status | Goal |
|---|---|---|
| M0 — Hello EL2 | **done** | Enter EL2, print banner |
| M1 — Bare-metal guest | **done** | Stage-2 MMU, minimal vCPU |
| M2 — PSCI | **done** | PSCI VERSION/FEATURES/CPU_OFF/SYSTEM_OFF over HVC (no GIC) |
| M3 — vGIC software injection | **done** | HVC → `vgic_inject_sw` → guest EL1 IRQ handler (no physical HW) |
| M4 — Physical timer + GIC + HW-forwarding | **done** | timer PPI → EL2 → `vgic_inject_hw` → guest (ADR-0001) |
| M5 — Linux alive (no interrupts) | **done** | Load `Image` + DTB, arm64 boot protocol, PL011 passthrough earlycon; stalls at first GIC MMIO |
| M6 — MMIO trap framework | **done** | Stage-2 data-abort decode + MMIO trap-and-emulate dispatch |
| M7 — vGICv3 emulation | **done** | GICD/GICR(cpu0) trap-and-emulate on the M6 bus; timer-PPI injection |
| M8 — Boot to shell | **done (boot-verified 2026-06-19)** | initramfs load + DTB initrd nodes → interactive busybox shell prompt (headline M5 goal: UP Linux boots to a busybox shell). Confirmed on a real QEMU run: Linux 6.12.93 reaches `~ #` and runs `ls`/`echo`/`uname` over the ttyAMA0 PL011 passthrough |
| M9 — SMP | **done (boot-verified 2026-06-27; reverified 2026-07-13)** | 2-vCPU Linux, PSCI `CPU_ON`, per-pCPU vCPU, SGI virtualization, static 1:1 pinning (no scheduler) |
| M10 — Multi-VM foundation | **done (gate-verified 2026-07-20)** | VM objectification (`g_vm` → `vm[NR_VMS]`): per-VM Stage-2/VMID, per-VM vGIC, static 2+2 CPU partitioning on 4 pCPUs. Memory: QEMU `-m 4G`, VM1 RAM backed at PA `0xC0000000`; both VMs see the identical guest address map (RAM IPA `0x40000000` — the IPA≠PA Stage-2 mechanism is live since M5), so one DTB template and one load-address scheme serve both VMs. Console: EL2 owns the physical PL011 exclusively (revoked the M5 passthrough), both VMs get trap-and-emulate vuarts (`hypervisor/dm/vuart.c`), Ctrl-T switches RX focus (TX is focus-independent). VM-scoped PSCI power-down: `SYSTEM_OFF` in one VM parks only that VM's pCPUs. Landed as three slices, each independently spec- and quality-reviewed, plus a final whole-milestone review; see [[docs/adr/0014-multi-vm-static-partition-el2-console]]. New automated regression: dual-SVM scenario (`svm4`, now `make test-qemu-dual`) in `make test`. Known gaps carried into M11+: no automated test for PSCI-off isolation (verified manually only) — the Ctrl-T/`vm_console` focus gap was closed 2026-07-25 by `tests/run_shell_test.sh`; console focus left on a since-shut-down VM is a silent no-op, not fed back to the user (now at least visible as `halted` in `vm_list`); `docs/reference/arm/2026-06-21-architecture-zoom-out.md` still describes the pre-M10 single-VM model (flagged with a banner, full rewrite deferred) |
| M11 — vCPU scheduler | planned (moved up from M14, 2026-07-19) | Full context switch (incl. FP/SIMD state — lifts the `-mgeneral-regs-only` no-save assumption), time slicing, vCPU count > pCPU count. Deliverable: M10's 2 VMs × 2 vCPUs time-sliced on 2 pCPUs (the other 2 pCPUs left idle for later Service VM work); both guests reach shells and concurrent FP workloads in both VMs run uncorrupted |
| M12 — Hypercall ABI + VM lifecycle | planned | HVC hypercall namespace (distinct from PSCI), VM create/start/pause/destroy, Service VM privilege concept. Deliverable: Service VM controls User VM start/stop via hypercalls |
| M13 — HSM kernel driver + io_req ring | planned | Custom Linux kernel module in the Service VM (modeled on `acrn_hsm`): ioctl interface, io_req shared-memory ring, forwarding User VM MMIO exits to Service VM userspace. Deliverable: a userspace program receives one User VM MMIO access and completes it. Highest-risk milestone — kept minimal on purpose (no virtio) |
| M14 — Device Model + virtio backends | planned | Userspace `dm` program: VM load/start, virtio-mmio console and blk backends. Deliverable: User VM launched by DM, rootfs on a virtio-blk image |
| M15 — RK3588 port | planned | Runtime FDT parsing, real UART/GIC/storage, board bring-up; reproduce the full chain on hardware |
| Deferred | — | SMMU/DMA isolation and the device-passthrough framework: not needed while all User VM devices are DM-emulated on QEMU; schedule when RK3588 passthrough demands it |

## Milestone renumbering (2026-08-10)

The roadmap was flattened to consecutive integers `M0`–`M15`. The old scheme had
decimal milestones (`M1.5`, `M2.5`, `M3.0`–`M3.5`) and a gap where `M4` was never
used, which made ordering ambiguous. **Historical documents were deliberately not
rewritten** — `docs/superpowers/{specs,plans}/`, `docs/adr/` and `docs/debug/`
are records of what happened at the time, and their filenames are referenced from
git history and already-pushed branches. Use this table when reading them:

| Old | New | Milestone |
|---|---|---|
| M0 | M0 | Hello EL2 |
| M1 | M1 | Bare-metal guest |
| M1.5 | **M2** | PSCI |
| M2 | **M3** | vGIC software injection |
| M2.5 | **M4** | Physical timer + GIC + HW-forwarding |
| M3.0 | **M5** | Linux alive (no interrupts) |
| M3.1 | **M6** | MMIO trap framework |
| M3.2 | **M7** | vGICv3 emulation |
| M3.4 | **M8** | Boot to shell |
| M3.5 | **M9** | SMP |
| M5 | **M10** | Multi-VM foundation |
| M6 | **M11** | vCPU scheduler |
| M7 | **M12** | Hypercall ABI + VM lifecycle |
| M8 | **M13** | HSM kernel driver + io_req ring |
| M9 | **M14** | Device Model + virtio backends |
| M10 | **M15** | RK3588 port |

There was never an `M3.3`, and the old `M4` (once "RK3588 port") was retired when
the port moved to the end of the roadmap; neither has a new-scheme equivalent.
Filenames such as `2026-06-23-m3.5-smp-design.md` keep their old numbering.

## ACRN-model strategy (decided 2026-07-14, revised 2026-07-19)

The end state is the full ACRN architecture: a privileged Service VM running a
userspace Device Model that serves virtio backends to User VMs. Standing
decisions shaping the roadmap ordering:

1. **QEMU-first**: the entire ACRN-model core chain (multi-VM → hypercall →
   HSM → DM) is developed and verified on QEMU `virt`; the RK3588 port comes last.
2. **Scheduler right after multi-VM** (revised 2026-07-19; was "scheduler
   last"): the primary project goal is learning EL2 core technology, and the
   scheduler is the densest remaining piece — it forces per-vCPU state
   (FP/SIMD, system registers, vGIC LR save/restore) to be complete. Doing it
   at M11, before the hypercall→HSM→DM chain, means M12–M14 build on a finished
   state-switching foundation instead of accumulating "no-save" assumptions.
   M10 itself still keeps 1:1 vCPU:pCPU pinning (ACRN "partitioned" mode).
3. **No interim in-hypervisor virtio backends**: virtio backends are written
   once, in the userspace DM (M14). Until then User VMs use vuart console +
   initramfs (no disk). Avoids writing/maintaining the backend logic twice.
4. **FDT parsing deferred to M15**: QEMU milestones keep static board config
   (ADR-0008) and prebuilt guest DTB templates.
5. **EL2 owns the console from M10 on** (2026-07-19): the M5 PL011 passthrough
   is revoked; the physical UART belongs to EL2 and every VM sees a
   trap-and-emulate vuart, with an escape key switching input focus. This is
   the only design that gives every VM an interactive shell over one physical
   UART, and it pre-builds the Service VM console path.
6. **One guest address map for all VMs** (2026-07-19): every VM sees RAM at
   IPA `0x40000000` regardless of the backing PA. This is not new mechanism —
   `vm_config` already separates `mem_base` (IPA) from `ram_pa`, and VM0 has
   mapped IPA `0x40000000` → PA `0x80000000` since M5 — so VM1 is just a
   second config with `ram_pa = 0xC0000000`. One DTB template, one set of
   load-offset constants, and the guest-visible machine is identical across
   VMs. (An identity-mapped VM1 was considered and rejected: it would need a
   second DTB template and a second address-constant set for zero gain.)

Each milestone gets its own spec in `docs/superpowers/specs/` and plan in `docs/superpowers/plans/`.
