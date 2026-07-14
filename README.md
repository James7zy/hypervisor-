# hypervisor

A research Type-1 ARM64 hypervisor targeting QEMU `virt` (AArch64) first, then
Rockchip RK3588. Inspired by ACRN, Xvisor, and bao-hypervisor; the directory
layout mirrors ACRN's `hypervisor/` structure.

## Status

**M3.5 — SMP** is done (boot-verified 2026-06-27; reverified 2026-07-13). An
unmodified Linux 6.12 guest boots on QEMU `virt` with two vCPUs statically pinned
1:1 to two pCPUs, reaches an interactive busybox shell (`~ #`), and reports CPUs
`0-1` online. PSCI `CPU_ON`, per-CPU vGICv3/timer state, and cross-core SGI/IPI
delivery are working. M3.5 deliberately has no scheduler or vCPU overcommit.

Next: **M4 (RK3588 port)** — move from the QEMU `virt` board to real hardware.
See [the M3.5 SMP design](docs/superpowers/specs/2026-06-23-m3.5-smp-design.md)
for the completed QEMU architecture.

### Milestones

| Milestone | Status | Goal |
|---|---|---|
| M0 — Hello EL2 | done | Enter EL2, print banner |
| M1 — Bare-metal guest | done | Stage-2 MMU, minimal vCPU |
| M1.5 — PSCI | done | PSCI over HVC (no GIC) |
| M2 — vGIC software injection | done | HVC → `vgic_inject_sw` → guest EL1 IRQ handler |
| M2.5 — Physical timer + GIC + HW-forwarding | done | timer PPI → EL2 → `vgic_inject_hw` → guest |
| M3.0 — Linux alive | done | Load `Image` + DTB, arm64 boot protocol, PL011 earlycon |
| M3.1 — MMIO trap framework | done | Stage-2 data-abort decode + trap-and-emulate dispatch |
| M3.2 — vGICv3 emulation | done | GICD/GICR trap-and-emulate; timer-PPI injection |
| M3.4 — Boot to shell | **done** | initramfs → interactive busybox shell |
| M3.5 — SMP | **done** | 2-vCPU Linux, PSCI `CPU_ON`, per-pCPU vCPU, SGI virtualization |
| M4 — RK3588 port | next | Real hardware, DT/ACPI discovery, boot from storage |

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

`LINUX_INITRD` is optional. With an initramfs configured, boot continues through
Linux to a busybox shell prompt (`~ #`). Exit QEMU with `Ctrl-A x`.

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

1. **Automated suite**: `make test` builds a separate SVM-mode hypervisor under
   `build/test-svm/`, checks C/assembly struct offsets, and runs the M1, M2, and M2.5
   QEMU integration scenarios.
2. **Linux SMP boot**: run with `LINUX_IMAGE` (and optionally `LINUX_INITRD`) and
   verify `CPU1: Booted secondary processor`, `smp: Brought up 1 node, 2 CPUs`,
   and `0-1` in `/sys/devices/system/cpu/online`.
3. **IPI/timer check**: inspect `/proc/interrupts`; both CPU columns should show
   timer interrupts and increasing IPI counts.

## Documentation

- Design specs: `docs/superpowers/specs/`
- Architecture decisions: `docs/adr/`
- Reference knowledge base & debug walkthroughs: `docs/reference/`, `docs/debug/`
- Project guidance for contributors and agents: [`CLAUDE.md`](CLAUDE.md)

## License

See [`LICENSE`](LICENSE).
