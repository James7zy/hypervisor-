# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

The architecture documentation should include sequence diagrams, class diagrams, and diagrams showing the 
relationships between modules, all represented using Mermaid. A picture is worth a thousand words.

## Project Overview

A learning/research Type-1 ARM64 hypervisor targeting QEMU `virt` (AArch64) first, then Rockchip RK3588. Inspired by ACRN, Xvisor. The directory layout
 mirrors ACRN's `hypervisor/` structure.

Completed: M0 (Hello EL2), M1 (bare-metal SVM guest: Stage-2 MMU, vCPU context switch, HVC dispatch), M1.5 (PSCI), M2 (vGIC SW injection), M2.5 (physical timer + GIC + HW-forwarding), **M3 — boot an unmodified single-core (UP) Linux guest to a busybox shell** (QEMU `virt`, GICv3, `-smp 1`).

M3 (done) was decomposed into five small, dependency-ordered sub-milestones, all complete: **M3.0** (Linux alive — load `Image`/`guest.dtb` via QEMU `-device loader`, set the arm64 boot protocol, earlycon via PL011 passthrough; stalls at the first GIC MMIO access), **M3.1** (Stage-2 data-abort decode + MMIO trap-and-emulate dispatch framework), **M3.2** (vGICv3 GICD/GICR(cpu0) emulation on the M3.1 bus + timer-PPI injection), **M3.3** (virtio-mmio transport + virtio-console with used-buffer IRQ), and **M3.4** (initramfs load + DTB initrd nodes → interactive busybox shell; see `docs/reference/guest-initramfs.md`). See `docs/superpowers/specs/2026-06-15-m3-linux-guest-decomposition-design.md`.

**M3 was first validated by a real QEMU boot on 2026-06-19** (until then every M3.x sub-milestone was verified statically only — build + `readelf` — and the live boot was deferred as an "operator handoff"). That first boot found and fixed three chained bugs (Stage-2 1 GB-block mis-alignment, guest RAM placed past the end of `-m 1G` DRAM, and an IRQ taken at EL2 in the `daifclr`→`eret` window) and wired up interactive PL011 input. The committed `scripts/run-qemu.sh` now boots Linux 6.12.93 to an interactive `~ #` shell. Full step-by-step diagnosis: `docs/debug/m3-boot-debug-walkthrough.md`; result summary: `docs/debug/m3-boot-verification.md`.

Next: **M3.5 (SMP)** then **M4 (RK3588 port)**.

## Build Commands

```sh
make defconfig          # copy configs/qemu_virt_defconfig → .config
make                    # build build/hypervisor.elf + build/hypervisor.bin
make run                # invoke scripts/run-qemu.sh (QEMU)
make clean              # remove build/
```

```sh
# M3.0+: requires a prebuilt arm64 Linux `Image`. Supply its path:
#   LINUX_IMAGE=/path/to/Image make run
# Linux is NOT built in-repo; the kernel is user-supplied.

# M3.4: to boot all the way to a busybox shell, also supply an initramfs:
#   LINUX_IMAGE=/path/to/Image LINUX_INITRD=/path/to/initramfs.cpio.gz make run
# The initramfs is user-supplied (see docs/reference/guest-initramfs.md); not built in-repo.
```

Boot constraints learned from the first real run (see `docs/debug/m3-boot-debug-walkthrough.md`):
- **`scripts/run-qemu.sh` uses `-m 2G` and this is required** — guest RAM is backed
  at the 1 GB-aligned PA `0x80000000` (so the Stage-2 L1 1 GB block can map it),
  which is only inside QEMU `virt` DRAM when more than 1 GB is present (ADR-0004).
- **The kernel `Image` must fit the address budget** (~31 MB: Image at PA `0x80080000`,
  DTB at `0x82000000`). A full arm64 `defconfig` Image is ~37 MB and overruns the
  DTB; trim unused subsystems (NET/PCI/USB/DRM/DEBUG_INFO, keep PL011 + virtio +
  devtmpfs + initramfs) to get under budget.
- **The Makefile does not track header dependencies** — after editing any header
  (e.g. `board.h`), run `make clean` or stale `.o`s relink with old values.

Override defaults with: `ARCH=arm64 BOARD=qemu_virt CROSS_COMPILE=aarch64-none-linux-gnu-`

**Toolchain required:**
- `aarch64-none-linux-gnu-gcc` ≥ 10
- `aarch64-none-linux-gnu-binutils`
- `qemu-system-aarch64` ≥ 6.0
- `dtc` (device-tree-compiler) — Debian/Ubuntu: `sudo apt-get install device-tree-compiler`

Exit QEMU with `Ctrl-A x`. GDB attach: `QEMU_EXTRA_ARGS="-s -S" make run`, then `aarch64-none-linux-gnu-gdb build/hypervisor.elf -ex 'target remote :1234'`.

## Verification (no CI, no test framework)

There is no automated test suite. Verification is:
1. **Build check**: `make` must succeed with zero warnings (`-Werror` is on).
2. **Static inspection**: `aarch64-none-linux-gnu-readelf -h build/hypervisor.elf` — entry point must be `0x40080000`; `.text` section must start at `0x40080000`.
3. **Run + observe**: `make run` must print `[hv] Hello from EL2, CurrentEL=0x8` within 3 seconds.

## Architecture
```
+--------------------------------------------------------------------------------+
|                         Applications / Workloads                               |
|                                                                                |
|   +----------------------+     +----------------------+     +----------------+ |
|   | Linux Apps           |     | Android / Linux Apps |     | RT Apps        | |
|   | Mgmt / Cloud / UI    |     | IVI / HMI / General  |     | Control Tasks  | |
|   +----------+-----------+     +----------+-----------+     +-------+--------+ |
|              |                            |                         |          |
+--------------|----------------------------|-------------------------|----------+
               |                            |                         |
               v                            v                         v
+-------------------------------+   +------------------------+   +--------------+
|          Service VM            |   |        User VM          |   |   RTOS VM    |
|       Linux / SOS VM           |   |  Linux / Android Guest  |   | RTOS Guest   |
|                                |   |                         |   |              |
| +----------------------------+ |   | +--------------------+  |   | +----------+ |
| |    Device Model, DM      | |   | | Guest OS           |  |   | | RTOS     | |
| |                            | |   | |                    |  |   | | Kernel   | |
| | - Create / start VM        | |   | | - VirtIO frontend  |  |   | |          | |
| | - Emulate virtual devices  | |<---->| - Virtual devices |  |   | | RT tasks | |
| | - Handle VM exits / MMIO   | |   | | - Guest drivers    |  |   | +----------+ |
| | - Provide VirtIO backend   | |   | | - Applications     |  |   |              |
| +-------------+--------------+ |   | +--------------------+  |   |              |
|               |                |   +------------+-----------+   +------+-------+
| +-------------v--------------+ |                |                      |
| | Manager / Tools       | |                |                      |
| | acrnctl / config / launch  | |                |                      |
| +-------------+--------------+ |                |                      |
|               |                                |                      |
+---------------|--------------------------------|----------------------|---------+
                |                                |                      |
                | Hypercall / ioctl             | VM Exit / Trap        |
                | VM lifecycle control          | MMIO / PIO / IRQ      |
                v                                v                      v
+--------------------------------------------------------------------------------+
|                              Hypervisor                                   |
|                                                                                |
| +--------------------+  +--------------------+  +----------------------------+ |
| | VM Management      |  | vCPU Scheduler     |  | Memory Manager            | |
| | Create / destroy   |  | vCPU dispatch      |  | Stage-2 memory isolation  | |
| +--------------------+  +--------------------+  +----------------------------+ |
|                                                                                |
| +--------------------+  +--------------------+  +----------------------------+ |
| | VM Exit Handler    |  | Interrupt Manager  |  | I/O Virtualization        | |
| | Forward exits to DM|  | GIC IRQ routing    |  | MMIO / device passthrough | |
| +--------------------+  +--------------------+  +----------------------------+ |
|                                                                                |
|        CPU / Memory / Interrupt / Device Isolation & Virtualization             |
+--------------------------------------------------------------------------------+
                |
                v
+--------------------------------------------------------------------------------+
|                              ARM Hardware / SoC                                |
|                                                                                |
| +----------------------+  +----------------------+  +------------------------+ |
| | ARM CPU Cores        |  | Memory               |  | SoC / Physical Devices | |
| | Cortex-A / Neoverse  |  | DDR / LPDDR RAM      |  | UART / I2C / SPI / CAN | |
| | EL1 Guest OS         |  |                      |  | GPU / NPU / USB / PCIe | |
| | EL2 Hypervisor       |  |                      |  | NIC / Storage / Display| |
| +----------------------+  +----------------------+  +------------------------+ |
|          |                         |                         |                 |
|          +-------------------------+-------------------------+                 |
|                                                                                |
|        ARM Virtualization Extension / EL2                                      |
|        Stage-2 Address Translation                                             |
|        GICv3 / GICv4 Interrupt Controller                                      |
|        SMMU / IOMMU for DMA Isolation                                          |
|        PSCI / Power Management                                                 |
+--------------------------------------------------------------------------------+
```


### Layering
```
+--------------------------------------------------------------------------------+
|                              ARM Exception Levels                              |
+--------------------------------------------------------------------------------+
|                                                                                |
|  Guest VM / Service VM / RTOS VM                                                |
|  ----------------------------------------------------------------------------  |
|  EL0 : User Applications                                                       |
|  EL1 : Guest OS Kernel, Linux / Android / RTOS                                 |
|                                                                                |
|  Hypervisor                                                               |
|  ----------------------------------------------------------------------------  |
|  EL2 : Hypervisor Mode                                                         |
|       - vCPU scheduling                                                        |
|       - Stage-2 translation                                                    |
|       - trap and emulate                                                       |
|       - interrupt virtualization                                               |
|       - device passthrough control                                             |
|                                                                                |
|  Optional Secure World                                                         |
|  ----------------------------------------------------------------------------  |
|  EL3 : Secure Monitor / Trusted Firmware-A / PSCI                              |
|                                                                                |
+--------------------------------------------------------------------------------+
|                              ARM SoC Hardware                                  |
|                                                                                |
|  ARM Cores | DDR Memory | GICv3/GICv4 | SMMU | PCIe | MMIO Devices | DMA        |
+--------------------------------------------------------------------------------+
```

### Board vs Driver separation

### Key invariants

- **`-mgeneral-regs-only` is mandatory**: M0 does not save FP/SIMD state. Never add code that forces the compiler to emit FP/SIMD instructions.
- **No magic numbers in `uart_pl011.c`**: driver receives base from `uart_init`.
- **printk supports only**: `%s %c %d %u %x %lx %%`. No width, precision, floats, or `%p`.
- **Empty directories use `.gitkeep`** to preserve the ACRN-style skeleton shape for future milestones.
- **`.config` is required**: `make` fails with an error if `.config` is absent — always run `make defconfig` first. The Makefile converts `CONFIG_FOO=y` lines to `-DCONFIG_FOO=1`.

### Compiler flags (all translation units)

```
-ffreestanding -nostdlib -nostartfiles
-fno-pic -fno-stack-protector
-mgeneral-regs-only -mstrict-align
-Wall -Wextra -Werror -O2 -g
```

### Boot sequence

```
QEMU → _start (head.S)
  1. Park non-boot CPUs (MPIDR_EL1.Aff0 != 0 → secondary_park / wfi)
  2. Assert CurrentEL == 0b1000; else → panic_early (emits "!EL\n" directly to PL011 DR)
  3. Set SP_EL2 = __stack_top
  4. Clear BSS (memset)
  5. VBAR_EL2 = hv_vectors (panic stubs)
  6. DAIF mask, dsb/isb
  7. bl hypervisor_main(dtb_phys)
     → uart_init(BOARD_UART_BASE)
     → printk("[hv] Hello from EL2, CurrentEL=0x%lx\n", read_currentel())
     → for(;;) cpu_wfi()
```

### Load address

`0x40080000` — QEMU `virt` default kernel load address, set in `hypervisor/arch/arm64/board/qemu_virt/linker.lds`.

## Milestone Roadmap

| Milestone | Status | Goal |
|---|---|---|
| M0 — Hello EL2 | **done** | Enter EL2, print banner |
| M1 — Bare-metal guest | **done** | Stage-2 MMU, minimal vCPU |
| M1.5 — PSCI | **done** | PSCI VERSION/FEATURES/CPU_OFF/SYSTEM_OFF over HVC (no GIC) |
| M2 — vGIC software injection | **done** | HVC → `vgic_inject_sw` → guest EL1 IRQ handler (no physical HW) |
| M2.5 — Physical timer + GIC + HW-forwarding | **done** | timer PPI → EL2 → `vgic_inject_hw` → guest (ADR-0001) |
| M3.0 — Linux alive (no interrupts) | **done** | Load `Image` + DTB, arm64 boot protocol, PL011 passthrough earlycon; stalls at first GIC MMIO |
| M3.1 — MMIO trap framework | **done** | Stage-2 data-abort decode + MMIO trap-and-emulate dispatch |
| M3.2 — vGICv3 emulation | **done** | GICD/GICR(cpu0) trap-and-emulate on the M3.1 bus; timer-PPI injection |
| M3.3 — virtio-console | **done** | virtio-mmio transport + virtio-console + virtqueue + used-buffer IRQ |
| M3.4 — Boot to shell | **done (boot-verified 2026-06-19)** | initramfs load + DTB initrd nodes → interactive busybox shell prompt (headline M3 goal: UP Linux boots to a busybox shell). Confirmed on a real QEMU run: Linux 6.12.93 reaches `~ #` and runs `ls`/`echo`/`uname` over the ttyAMA0 PL011 passthrough |
| M3.5 — SMP | future | PSCI `CPU_ON`, per-pCPU vCPU, SGI virtualization, scheduler |
| M4 — RK3588 port | future | Real hardware, DT/ACPI discovery, boot from storage |

Each milestone gets its own spec in `docs/superpowers/specs/` and plan in `docs/superpowers/plans/`.

**Design principle:** My design philosophy is to move forward in small, fast iterations, 
breaking requirements down into the smallest practical units and defining them clearly. 
The goal is to achieve high cohesion and low coupling across the system.


## Reference Source Trees
The following production hypervisor source trees are available in the parent directory (`../`) for reference when making design or implementation decisions:

| Path | Project | Notes |
|------|---------|-------|
| `../acrn-hypervisor` | [ACRN](https://github.com/projectacrn/acrn-hypervisor) | Type-1, x86 + ARM64; primary structural inspiration for this repo's layout |
| `../xvisor` | [Xvisor](https://github.com/avpatel/xvisor-next) | Type-1, ARM-first; reference for Stage-2 MMU and vCPU design |
| `../hypervisor` | local Rust hypervisor | Rust-based hypervisor (NOT a copy of this project); reference for Rust idioms applied to bare-metal hypervisor design |

**When to consult these:** look up an existing implementation before designing any new subsystem (Stage-2 MMU, vGIC, PSCI, virtio, etc.). Prefer reading the smallest relevant file rather than loading entire trees.

Import Reference

- [ARM Architecture Reference Manual (ARMv8-A)](https://developer.arm.com/documentation/ddi0487/latest)
- [ARM GIC Architecture Specification](https://developer.arm.com/documentation/ihi0069/latest)
- [pKVM (Protected KVM)](https://source.android.com/docs/core/virtualization)
