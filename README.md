# hypervisor

A research Type-1 ARM64 hypervisor targeting QEMU `virt` (AArch64) first, then
Rockchip RK3588. Inspired by ACRN, Xvisor, and bao-hypervisor; the directory
layout mirrors ACRN's `hypervisor/` structure.

## Status

**M10 — Multi-VM foundation** is done (gate-verified 2026-07-20). Two unmodified
Linux guests run side by side on QEMU `virt`, each with its own Stage-2 page
table, VMID, vGICv3 and vuart, statically partitioned 2+2 across four pCPUs.
EL2 owns the physical PL011 exclusively; both VMs get a trap-and-emulate vuart,
and `Ctrl-T` opens an EL2 shell to switch console focus between them.

Each VM is built from the same guest address map (RAM at IPA `0x40000000`,
backed by different PAs), so one DTB template and one load-address scheme serve
both. There is still no scheduler: vCPUs are pinned 1:1 to pCPUs.

Next: **M11 (vCPU scheduler)** — full context switch including FP/SIMD state,
time slicing, and vCPU count greater than pCPU count. The long-term target is
the full ACRN model (Service VM + userspace Device Model); the RK3588 port is
last, at M15.

See [the multi-VM design spec](docs/superpowers/specs/2026-07-19-m5-multi-vm-design.md)
and [ADR-0014](docs/adr/0014-multi-vm-static-partition-el2-console.md) for the
current architecture.

> **Note on milestone numbers:** the roadmap was renumbered to consecutive
> integers on 2026-08-10 (the old scheme had `M1.5`, `M2.5`, `M3.0`–`M3.5` and an
> unused `M4`). Historical specs, ADRs and debug notes keep their original
> numbering — e.g. this spec's filename says `m5` for what is now M10. CLAUDE.md
> has the full old→new mapping table.

### Milestones

| Milestone | Status | Goal |
|---|---|---|
| M0 — Hello EL2 | done | Enter EL2, print banner |
| M1 — Bare-metal guest | done | Stage-2 MMU, minimal vCPU |
| M2 — PSCI | done | PSCI over HVC (no GIC) |
| M3 — vGIC software injection | done | HVC → `vgic_inject_sw` → guest EL1 IRQ handler |
| M4 — Physical timer + GIC + HW-forwarding | done | timer PPI → EL2 → `vgic_inject_hw` → guest |
| M5 — Linux alive | done | Load `Image` + DTB, arm64 boot protocol, PL011 earlycon |
| M6 — MMIO trap framework | done | Stage-2 data-abort decode + trap-and-emulate dispatch |
| M7 — vGICv3 emulation | done | GICD/GICR trap-and-emulate; timer-PPI injection |
| M8 — Boot to shell | done | initramfs → interactive busybox shell |
| M9 — SMP | done | 2-vCPU Linux, PSCI `CPU_ON`, per-pCPU vCPU, SGI virtualization |
| M10 — Multi-VM foundation | **done** | 2 VMs × 2 vCPUs, per-VM Stage-2/VMID/vGIC/vuart, EL2-owned console |
| M11 — vCPU scheduler | next | Context switch incl. FP/SIMD, time slicing, vCPU overcommit |
| M12 — Hypercall ABI + VM lifecycle | planned | HVC hypercall namespace, VM create/start/pause/destroy |
| M13 — HSM kernel driver + io_req ring | planned | Service VM kernel module, MMIO exits forwarded to userspace |
| M14 — Device Model + virtio backends | planned | Userspace `dm`, virtio-mmio console and blk |
| M15 — RK3588 port | planned | Runtime FDT parsing, real UART/GIC/storage |

## Quickstart

```sh
make defconfig          # copy configs/qemu_virt_defconfig → .config
make                    # build build/hypervisor.elf + build/hypervisor.bin
LINUX_IMAGE=/path/to/Image LINUX_INITRD=/path/to/initramfs.cpio.gz make run
```

Earliest banner (within ~3 s):

```
[hv] Hello from EL2 on qemu_virt, CurrentEL=0x8
```

`LINUX_INITRD` is optional. With an initramfs configured, both VMs boot Linux to
a busybox shell prompt. Exit QEMU with `Ctrl-A x`.

Two constraints are easy to trip over:

- **`-m 4G` is required** (already set in `scripts/run-qemu.sh`). VM0's RAM is
  backed at PA `0x80000000` and VM1's at `0xC0000000`, so both 1 GB-aligned
  blocks only fall inside QEMU `virt` DRAM with 4 GB present.
- **The kernel `Image` must fit the address budget** (~31 MB). A full arm64
  `defconfig` Image overruns the DTB; trim unused subsystems, keeping PL011 +
  virtio + devtmpfs + initramfs.

### EL2 shell

Both guests share one physical UART, so EL2 mediates console input. Press
`Ctrl-T` at any time to drop into the hypervisor shell:

```
hv> help
hv> vm_list              # VMs, their pCPUs, state, and current console focus
hv> vm_console 1         # attach console input to VM 1
```

`Ctrl-T` again returns to the guest. Guest output is unaffected by focus — only
input is routed. Each VM's initramfs sets a distinct prompt (`vm0:~ #` /
`vm1:~ #`) so it is obvious which guest you are typing to.

GDB attach:

```sh
LINUX_IMAGE=/path/to/Image QEMU_EXTRA_ARGS="-s -S" make run
aarch64-none-linux-gnu-gdb build/hypervisor.elf -ex 'target remote :1234'
```

## Requirements

- `aarch64-none-linux-gnu-gcc` >= 10
- `aarch64-none-linux-gnu-binutils`
- `qemu-system-aarch64` >= 6.0
- `dtc` (device-tree-compiler)

## Verification

There is no CI; verification is local and automated.

1. **Automated suite**: `make test` checks C/assembly struct offsets, then runs
   QEMU integration scenarios — `basic` and `vtimer` on a single-VM SVM build
   (`build/test-svm/`), plus the dual-VM (`test-qemu-dual`) and EL2-shell scenarios on a
   `NR_VMS=2` build (`build/test-svm-dual/`). The shell scenario drives real
   keystrokes into QEMU's serial stdin (`Ctrl-T`, `vm_list`, `help`,
   `vm_console`, error paths, backspace editing) and uses bare-metal guests, so
   it finishes in seconds rather than waiting on a Linux boot.
2. **Build check**: `make` must succeed with zero warnings (`-Werror` is on).
3. **Linux SMP boot**: run with `LINUX_IMAGE` (and optionally `LINUX_INITRD`)
   and verify `CPU1: Booted secondary processor`, `smp: Brought up 1 node,
   2 CPUs`, and `0-1` in `/sys/devices/system/cpu/online`.
4. **IPI/timer check**: inspect `/proc/interrupts`; both CPU columns should show
   timer interrupts and increasing IPI counts.

## Documentation

- Design specs: `docs/superpowers/specs/`
- Architecture decisions: `docs/adr/`
- Reference knowledge base: `docs/reference/` — split by architecture into
  [`arm/`](docs/reference/arm/) (the implementation baseline: Stage-2, vGIC,
  world switch, exit dispatch) and [`x86/`](docs/reference/x86/) (an x86-64
  VT-x contrast document for mapping concepts when reading ACRN)
- Debug walkthroughs: `docs/debug/`
- Project guidance for contributors and agents: [`CLAUDE.md`](CLAUDE.md)

## License

See [`LICENSE`](LICENSE).
