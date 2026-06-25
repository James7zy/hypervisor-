# hypervisor

A research Type-1 ARM64 hypervisor targeting QEMU `virt` (AArch64) first, then
Rockchip RK3588. Inspired by ACRN, Xvisor, and bao-hypervisor; the directory
layout mirrors ACRN's `hypervisor/` structure.

## Status

**M3.4 — Boot to shell** is done (boot-verified 2026-06-19). An unmodified Linux
6.12 kernel boots under the hypervisor on QEMU `virt`, reaches an interactive
busybox shell (`~ #`), and runs `ls`/`echo`/`uname` over the ttyAMA0 PL011
passthrough.

Next: **M3.5 (SMP)** — PSCI `CPU_ON`, per-pCPU vCPUs, SGI virtualization, and a
scheduler. See [the M3.5 SMP design](docs/superpowers/specs/2026-06-23-m3.5-smp-design.md).

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
| M3.3 — virtio-console | done | virtio-mmio transport + console + virtqueue + used-buffer IRQ |
| M3.4 — Boot to shell | **done** | initramfs → interactive busybox shell |
| M3.5 — SMP | next | PSCI `CPU_ON`, per-pCPU vCPU, SGI virtualization, scheduler |
| M4 — RK3588 port | future | Real hardware, DT/ACPI discovery, boot from storage |

## Quickstart

```sh
make defconfig          # copy configs/qemu_virt_defconfig → .config
make                    # build build/hypervisor.elf + build/hypervisor.bin
make run                # boot under QEMU virt
```

Earliest banner (within ~3 s):

```
[hv] Hello from EL2 on qemu_virt, CurrentEL=0x8
```

With a kernel `Image` + initramfs configured, the boot continues through Linux
to a busybox shell prompt (`~ #`). Exit QEMU with `Ctrl-A x`.

GDB attach:

```sh
QEMU_EXTRA_ARGS="-s -S" make run
aarch64-none-linux-gnu-gdb build/hypervisor.elf -ex 'target remote :1234'
```

## Requirements

- `aarch64-none-linux-gnu-gcc` >= 10
- `aarch64-none-linux-gnu-binutils`
- `qemu-system-aarch64` >= 6.0
- `dtc` (device-tree-compiler)

## Verification

There is no automated test suite. Verification is:

1. **Build**: `make` succeeds with zero warnings (`-Werror` is on).
2. **Static inspection**: `aarch64-none-linux-gnu-readelf -h build/hypervisor.elf`
   — entry point and `.text` start at `0x40080000`.
3. **Run + observe**: `make run` prints the EL2 banner within 3 seconds.

## Documentation

- Design specs: `docs/superpowers/specs/`
- Architecture decisions: `docs/adr/`
- Reference knowledge base & debug walkthroughs: `docs/reference/`, `docs/debug/`
- Project guidance for contributors and agents: [`CLAUDE.md`](CLAUDE.md)

## License

See [`LICENSE`](LICENSE).
