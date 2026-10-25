# hypervisor

A research Type-1 hypervisor inspired by ACRN (directory layout mirrors ACRN's
`hypervisor/`), Xvisor and bao. ARM64 (AArch64) is the working architecture;
x86-64 (AMD SVM / Intel VT-x) is planned.

| Architecture | Status |
|---|---|
| ARM64 (QEMU `virt`) | **runs** two Linux VMs; covered by `make test` |
| ARM64 (RK3588) | planned (M15 bring-up, M16 passthrough) |
| x86-64 (AMD/Intel) | link-check stub only — compiles, nothing boots |

## Status

Two unmodified Linux guests run side by side on QEMU `virt`: per-VM Stage-2
page table, VMID, vGICv3 and vuart, vCPUs statically pinned 2+2 across four
pCPUs (no scheduler yet). EL2 owns the physical PL011; `Ctrl-T` opens an EL2
shell to switch console focus between the two vuarts.

The generic code reaches the architecture only through `*_arch_*` hooks and
`<arch/...>` headers, enforced by `make test`; the x86 stub is linked on every
`make test` to prove the generic code builds for a second architecture.

M0–M10 are done; the milestone roadmap and execution tracking live in
[Linear](https://linear.app/jamestam/project/hypervisor-3b151b91cb63)
(阶段0 = M0–M10 completed; 阶段1–9 = M11 prerequisites → M16). Ordering
rationale: [ADR-0016](docs/adr/0016-acrn-model-roadmap-decisions.md),
[ADR-0017](docs/adr/0017-el2-hardening-before-scheduler-and-passthrough-milestone.md).

> Historical specs and ADRs use old milestone numbers (renumbered 2026-08-10) —
> e.g. a spec filename says `m5` for what is now M10.

## Quickstart

```sh
export ARCH=arm64                               # default; ARCH=x86 selects the stub
export CROSS_COMPILE=aarch64-none-linux-gnu-    # the compiler must be on PATH
make qemu_virt_defconfig   # copy arch/arm64/configs/qemu_virt_defconfig → .config
make                    # build build/hypervisor.elf + build/hypervisor.bin
LINUX_IMAGE=/path/to/Image LINUX_INITRD=/path/to/initramfs.cpio.gz make run
```

You should see `[hv] Hello from EL2 on qemu_virt, CurrentEL=0x8` within ~3 s.
With an initramfs configured, both VMs boot to a busybox shell (`vm0:~ #` /
`vm1:~ #`). Exit QEMU with `Ctrl-A x`.

`make help` lists the boards of the current `ARCH`; `scripts/build.sh -h` wraps
all of this; `make test-x86-stub` link-checks x86 without touching `.config`.

Constraints that are easy to trip over:

- **`-m 4G` is required** (already set in `scripts/run-qemu.sh`): VM0's RAM is
  backed at PA `0x80000000`, VM1's at `0xC0000000`.
- **The kernel `Image` must fit ~31 MB** — a full `defconfig` Image overruns
  the DTB. Use the known-good trimmed config
  `guest/linux-6.12-qemu_virt.config` (`olddefconfig`) before building.
- **Use `make run` / `make test`, not the QEMU script directly** — `board.mk`
  feeds both `CONFIG_NR_CPUS` and QEMU's `-smp`.
- **Clean after header or flag changes** — the Makefile tracks neither.

### EL2 shell

Both guests share one physical UART, so EL2 mediates console input:

```
Ctrl-T            # drop into the hypervisor shell (again to return)
hv> vm_list       # VMs, their pCPUs, state, and current console focus
hv> vm_console 1  # attach console input to VM 1
```

Guest output is unaffected by focus — only input is routed.

GDB attach:

```sh
LINUX_IMAGE=/path/to/Image QEMU_EXTRA_ARGS="-s -S" make run
aarch64-none-linux-gnu-gdb build/hypervisor.elf -ex 'target remote :1234'
```

## Requirements

- `aarch64-none-linux-gnu-gcc` >= 10 and binutils
- `x86_64-linux-gnu-gcc` (x86 link check), `qemu-system-aarch64` >= 6.0, `dtc`
- Host `cc` (offset checks) and `perl` (arch-boundary check in `make test`)

## Verification

No CI; verification is local.

1. `make test` — arch-boundary check, x86 stub link check, offset checks, then
   QEMU scenarios: `basic`/`vtimer` (single VM), dual-VM + EL2 shell, vSPI
   RX/IPI/replay. Guests live in `tests/svm/` and `tests/vspi/`.
2. `make` must build with zero warnings (`-Werror` is on).
3. Linux SMP boot (`LINUX_IMAGE` [+ `LINUX_INITRD`]): expect `CPU1: Booted
   secondary processor`, `smp: Brought up 1 node, 2 CPUs`, and `0-1` in
   `/sys/devices/system/cpu/online`.
4. `/proc/interrupts`: timer interrupts and increasing IPI counts on both CPUs.

## Documentation

- [Docs index](docs/README.md)
- [Current architecture](docs/reference/arm/current-architecture.md) — modules, objects, sequences
- [Target ACRN architecture](docs/reference/arm/target-architecture.md) — not yet implemented
- Design specs `docs/superpowers/specs/` · decisions `docs/adr/` ·
  references `docs/reference/` (arm = implementation baseline, x86 = VT-x
  contrast) · debug walkthroughs `docs/debug/`
- Project guidance for contributors and agents: [`CLAUDE.md`](CLAUDE.md)

## License

See [`LICENSE`](LICENSE).
