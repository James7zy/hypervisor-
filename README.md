# hypervisor

A research Type-1 **multi-architecture** hypervisor. ARM64 (AArch64) comes first,
on QEMU `virt` and then Rockchip RK3588; AMD/Intel x86-64 (AMD SVM / Intel VT-x)
is the second target. Only ARM64 runs today: `arch/x86/` is a link-check stub.
Inspired by ACRN, Xvisor, and bao-hypervisor; the directory layout mirrors ACRN's
`hypervisor/` structure.

| Architecture | Status |
|---|---|
| ARM64 (QEMU `virt`) | **runs** two Linux VMs, `make test` covers it |
| x86-64 (AMD/Intel) | **link-check stub only**: the generic code compiles and links for x86, nothing boots |
| ARM64 (RK3588) | planned, M15 (bring-up) and M16 (passthrough) |

## Status

**M10 — Multi-VM foundation** is done (gate-verified 2026-07-20). Two unmodified
Linux guests run side by side on QEMU `virt`, each with its own Stage-2 page
table, VMID, vGICv3 and vuart, statically partitioned 2+2 across four pCPUs.
Each VM is built from the same guest address map (RAM at IPA `0x40000000`,
backed by different PAs), so one DTB template and one load-address scheme serve
both. There is still no scheduler: vCPUs are pinned 1:1 to pCPUs. EL2 owns the
physical PL011 exclusively; both VMs get a trap-and-emulate vuart, and `Ctrl-T`
opens an EL2 shell to switch console focus between them. The long-term target
is the full ACRN model (Service VM + userspace Device Model).

The milestone roadmap and its execution tracking live in
[Linear](https://linear.app/jamestam/project/hypervisor-3b151b91cb63):
阶段0 records M0–M10 as completed, 阶段1–9 = the M11 prerequisites → M16.
Ordering rationale: [ADR-0016](docs/adr/0016-acrn-model-roadmap-decisions.md)
and [ADR-0017](docs/adr/0017-el2-hardening-before-scheduler-and-passthrough-milestone.md).

The multi-architecture split is also in place: generic code reaches the
architecture only through `*_arch_*` hooks and `<arch/...>` headers, enforced by
`make test`; ARM exit decoding is separate from the common MMIO bus; and an x86
stub (`arch/x86/`) is compiled and linked on every `make test` to prove the
generic code builds for a second architecture. This does not yet add scheduling,
and it does **not** prove a working x86 port: the stub's hooks are empty and
never run.

Start with [the current architecture](docs/reference/arm/current-architecture.md)
for module, object and sequence diagrams. The
[multi-VM spec](docs/superpowers/specs/2026-07-19-m5-multi-vm-design.md),
[ADR-0014](docs/adr/0014-multi-vm-static-partition-el2-console.md) and
[ADR-0015](docs/adr/0015-arch-boundary-core-vs-arch.md) record the design decisions.

> **Note on milestone numbers:** the roadmap was renumbered to consecutive
> integers on 2026-08-10 (the old scheme had `M1.5`, `M2.5`, `M3.0`–`M3.5` and an
> unused `M4`). Historical specs, ADRs and debug notes keep their original
> numbering — e.g. this spec's filename says `m5` for what is now M10.

## Quickstart

```sh
export ARCH=arm64                               # default; ARCH=x86 selects the stub
export CROSS_COMPILE=aarch64-none-linux-gnu-    # the compiler must be on PATH
make qemu_virt_defconfig   # copy arch/arm64/configs/qemu_virt_defconfig → .config
make                    # build build/hypervisor.elf + build/hypervisor.bin
LINUX_IMAGE=/path/to/Image LINUX_INITRD=/path/to/initramfs.cpio.gz make run
```

The architecture and toolchain are chosen the way Linux does it: `ARCH` and
`CROSS_COMPILE`, then `make <board>_defconfig` (`make help` lists the boards of the
current `ARCH`). To link-check the x86 stub without touching your `.config`, run
`make test-x86-stub`. `scripts/build.sh -h` wraps all of this.

Earliest banner (within ~3 s):

```
[hv] Hello from EL2 on qemu_virt, CurrentEL=0x8
```

`LINUX_INITRD` is optional. With an initramfs configured, both VMs boot Linux to
a busybox shell prompt. Exit QEMU with `Ctrl-A x`.

Constraints that are easy to trip over:

- **`-m 4G` is required** (already set in `scripts/run-qemu.sh`). VM0's RAM is
  backed at PA `0x80000000` and VM1's at `0xC0000000`, so both 1 GB-aligned
  blocks only fall inside QEMU `virt` DRAM with 4 GB present.
- **The kernel `Image` must fit the address budget** (~31 MB). A full arm64
  `defconfig` Image overruns the DTB. A known-good trimmed Linux 6.12 **full
  `.config`** is checked in at `guest/linux-6.12-qemu_virt.config`; copy it into
  the kernel tree and run `olddefconfig` before building the Image.
- **Use `make run` / `make test`, not the QEMU script directly.** The board's
  `board.mk` supplies `board-nr-cpus` (4 on QEMU virt) to both `CONFIG_NR_CPUS`
  at build time and `HV_NR_CPUS` for QEMU's `-smp`.
- **Clean after header or build-configuration changes.** The Makefile does
  not track header dependencies or compiler-flag changes.

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
- `x86_64-linux-gnu-gcc` for the x86 link check (`make test-x86-stub`, part of `make test`)
- `qemu-system-aarch64` >= 6.0
- `dtc` (device-tree-compiler)
- Host C compiler (`cc`) for offset checks
- `perl` for the architecture-boundary check in `make test`

## Verification

There is no CI; verification is local and automated.

1. **Automated suite**: `make test` checks the arch boundary (source and
   transitive includes), compiles and links the generic code for x86-64 against
   the `arch/x86` stub, and checks host/cross-compiled C/assembly offsets, then runs:
   - `basic` and `vtimer`: single-VM SVM build (`build/test-svm/`).
   - Dual-VM and EL2 shell: `NR_VMS=2` build (`build/test-svm-dual/`).
   - vSPI RX/IPI/replay: separate dual-VM build (`build/test-vspi/`).
   Shell and vSPI both write QEMU serial input. The shell scenario checks
   `Ctrl-T`, `vm_list`, `help`, `vm_console`, errors and line editing without
   waiting for Linux. Shared SVM guests live in `tests/svm/`; the older vSPI
   guests remain in `tests/vspi/`.
2. **Build check**: `make` must succeed with zero warnings (`-Werror` is on).
3. **Linux SMP boot**: run with `LINUX_IMAGE` (and optionally `LINUX_INITRD`)
   and verify `CPU1: Booted secondary processor`, `smp: Brought up 1 node,
   2 CPUs`, and `0-1` in `/sys/devices/system/cpu/online`.
4. **IPI/timer check**: inspect `/proc/interrupts`; both CPU columns should show
   timer interrupts and increasing IPI counts.

## Documentation

- [Documentation index](docs/README.md) — current references vs historical records
- [Current architecture](docs/reference/arm/current-architecture.md) — the ARM64 implementation and the arch boundary
- [Target ACRN architecture](docs/reference/arm/target-architecture.md) — not yet implemented
- Design specs: `docs/superpowers/specs/`
- Architecture decisions: `docs/adr/`
- Reference knowledge base: `docs/reference/` — split by architecture into
  [`arm/`](docs/reference/arm/) (the implementation baseline: Stage-2, vGIC,
  world switch, exit dispatch) and [`x86/`](docs/reference/x86/) (an x86-64
  VT-x contrast document for mapping concepts when reading ACRN; the x86 port itself
  is not written yet)
- Debug walkthroughs: `docs/debug/`
- Project guidance for contributors and agents: [`CLAUDE.md`](CLAUDE.md)

## License

See [`LICENSE`](LICENSE).
