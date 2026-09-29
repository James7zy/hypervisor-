# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

The architecture documentation should include sequence diagrams, class diagrams, and diagrams showing the
relationships between modules, all represented using Mermaid. A picture is worth a thousand words.

## Project Overview

A research Type-1 ARM64 hypervisor targeting QEMU `virt` (AArch64) first, then Rockchip RK3588. Inspired by ACRN, Xvisor, bao-hypervisor. The directory layout
mirrors ACRN's `hypervisor/` structure.

**M9 (SMP)** is complete: an unmodified Linux guest boots with two vCPUs
statically pinned 1:1 to two pCPUs. Next: **M10 (multi-VM foundation)** — the
long-term target form is the **full ACRN model** (Service VM + userspace Device
Model); see the roadmap below. The RK3588 port moved to M15, after the ACRN-model
core chain is proven on QEMU.

## Build Commands

```sh
make defconfig          # copy configs/qemu_virt_defconfig → .config
make                    # build build/hypervisor.elf + build/hypervisor.bin
make test               # offset checks + M1/M3/M4 + dual-VM + EL2-shell QEMU scenarios
LINUX_IMAGE=/path/to/Image LINUX_INITRD=/path/to/initramfs.cpio.gz make run
make clean              # remove build/
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

Exit QEMU with `Ctrl-A x`. GDB attach:
`LINUX_IMAGE=/path/to/Image QEMU_EXTRA_ARGS="-s -S" make run`, then
`aarch64-none-linux-gnu-gdb build/hypervisor.elf -ex 'target remote :1234'`.

## Verification (no CI; local automated integration tests)

1. **Automated suite**: `make test` builds a separate SVM-mode hypervisor under
   `build/test-svm/`, checks C/assembly struct offsets, and runs the `basic`
   (HVC/PSCI round trip) and `vtimer` (HW-forwarded PPI 27) QEMU scenarios,
   plus the dual-VM (`test-qemu-dual`), EL2-shell and vSPI scenarios on
   `NR_VMS=2` builds.
   - **All bare-metal SVM guests live in `tests/svm/`**: one shared runtime
     (`svm_lib.c`, `svm_vectors.S`, `svm.lds`) and one file per scenario in
     `cases/<case>.c`, built to `build/.../svm/svm-<case>.bin`. Expected output
     lines live in `expect/<scenario>.txt`, checked by the single runner
     `tests/run_svm_test.sh`. A new scenario is a new case file + expect file
     + one Makefile target — **do not add another `tests/svmN/` directory**.
     (`tests/vspi/` predates this and is still separate.)
   - `tests/run_shell_test.sh` is the only scenario that **writes** to the QEMU
     serial stdin (the rest run `</dev/null`). It drives `Ctrl-T`, `vm_list`,
     `help`, `vm_console`, the error paths, and backspace line editing. It
     needs `NR_VMS=2` but no guest OS, so it uses the bare-metal SVM guests and
     finishes in seconds instead of a ~25s Linux boot. Input timing is a fixed
     `sleep` before the first byte — bytes sent before EL2 enables PL011 RX are
     lost, so `BOOT_WAIT` in that script is deliberately generous.
2. **Build check**: `make` must succeed with zero warnings (`-Werror` is on).
3. **Linux SMP run**: boot with `LINUX_IMAGE` and optionally `LINUX_INITRD`, then
   verify CPU1 boots, `/sys/devices/system/cpu/online` reports `0-1`, and both CPU
   columns in `/proc/interrupts` have timer and IPI activity.

**"Done" means observed running.** A milestone is marked done only after its
DoD was actually run in QEMU and the result seen. Build-clean + `readelf`
checks alone make it **static-only** — say so explicitly. M3 was once marked
done on static checks and its first real run found three bugs
(`docs/debug/m3-boot-verification.md`).

## Where knowledge lives

Test: *would it still hold on another machine, for another person?*
- **Yes** → the repo: debugging lessons in `docs/debug/`, decisions in
  `docs/adr/`, standing rules in `CLAUDE.md`/`AGENTS.md`.
- **No** (local paths, personal workflow) → the agent's private memory under
  `~/.claude/`, never checked in. There is no in-repo `memory/` directory;
  do not recreate one.

## Known defects (found, not yet fixed)

- **`CPU_OFF` kills the whole VM** (`hypervisor/common/psci/psci.c`, found
  2026-07-25): `PSCI_CPU_OFF` shares `psci_power_down()` with `SYSTEM_OFF`/
  `SYSTEM_RESET`, so it sets the per-VM `vm->off` flag and parks *every* pCPU of
  the VM. Per the PSCI spec `CPU_OFF` must stop only the calling vCPU. A guest
  doing `echo 0 > /sys/devices/system/cpu/cpu1/online` therefore takes its whole
  VM down. Fixing it needs per-vCPU off state (only per-VM exists today) plus a
  decision on what "last vCPU off" means; deliberately deferred because **M11
  rewrites this state management anyway**. Visible as `halted` in the EL2
  shell's `vm_list`.
- **`vm_console <n>` accepts a halted VM**: attaching focus to a powered-down VM
  silently does nothing (M10 known gap). `vm_list`'s STATE column now at least
  makes the cause visible before you attach.

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
- **dtc does not concatenate adjacent string literals** (unlike C), so `guest/qemu_virt.dts` takes its whole `bootargs` value as one `-DHV_BOOTARGS='"..."'` string from the Makefile. The DTS is preprocessed with `$(CPP)` before `dtc`; `make guest` emits one DTB per VM, differing only in the `hv.vm=` token.
- **The guest initramfs `/init` lives at `guest/initramfs-init.sh`** (the `.cpio.gz` itself is outside the repo). It reads `hv.vm=` into `PS1` so the two VMs show `vm0:~ #` / `vm1:~ #` — without it both guests print an identical prompt on the shared console and `vm_console <n>` gives no visible feedback. The busybox it targets has only 11 applets and **no `hostname`**, hence `PS1` rather than a real hostname.

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
| M2 — PSCI | **done** | PSCI VERSION/FEATURES/CPU_OFF/SYSTEM_OFF over HVC (no GIC) |
| M3 — vGIC software injection | **done** | HVC → `vgic_inject_sw` → guest EL1 IRQ handler (no physical HW) |
| M4 — Physical timer + GIC + HW-forwarding | **done** | timer PPI → EL2 → `vgic_inject_hw` → guest (ADR-0001) |
| M5 — Linux alive (no interrupts) | **done** | Load `Image` + DTB, arm64 boot protocol, PL011 passthrough earlycon; stalls at first GIC MMIO |
| M6 — MMIO trap framework | **done** | Stage-2 data-abort decode + MMIO trap-and-emulate dispatch |
| M7 — vGICv3 emulation | **done** | GICD/GICR(cpu0) trap-and-emulate on the M6 bus; timer-PPI injection |
| M8 — Boot to shell | **done (boot-verified 2026-06-19)** | initramfs load + DTB initrd nodes → interactive busybox shell prompt (headline M5 goal: UP Linux boots to a busybox shell). Confirmed on a real QEMU run: Linux 6.12.93 reaches `~ #` and runs `ls`/`echo`/`uname` over the ttyAMA0 PL011 passthrough |
| M9 — SMP | **done (boot-verified 2026-06-27; reverified 2026-07-13)** | 2-vCPU Linux, PSCI `CPU_ON`, per-pCPU vCPU, SGI virtualization, static 1:1 pinning (no scheduler) |
| M10 — Multi-VM foundation | **done (gate-verified 2026-07-20)** | VM objectification (`g_vm` → `vm[NR_VMS]`): per-VM Stage-2/VMID, per-VM vGIC, static 2+2 CPU partitioning on 4 pCPUs. Memory: QEMU `-m 4G`, VM1 RAM backed at PA `0xC0000000`; both VMs see the identical guest address map (RAM IPA `0x40000000` — the IPA≠PA Stage-2 mechanism is live since M5), so one DTB template and one load-address scheme serve both VMs. Console: EL2 owns the physical PL011 exclusively (revoked the M5 passthrough), both VMs get trap-and-emulate vuarts (`hypervisor/dm/vuart.c`), Ctrl-T switches RX focus (TX is focus-independent). VM-scoped PSCI power-down: `SYSTEM_OFF` in one VM parks only that VM's pCPUs. Landed as three slices, each independently spec- and quality-reviewed, plus a final whole-milestone review; see [[docs/adr/0014-multi-vm-static-partition-el2-console]]. New automated regression: dual-SVM scenario (`svm4`) in `make test`. Known gaps carried into M11+: no automated test for PSCI-off isolation (verified manually only) — the Ctrl-T/`vm_console` focus gap was closed 2026-07-25 by `tests/run_shell_test.sh`; console focus left on a since-shut-down VM is a silent no-op, not fed back to the user (now at least visible as `halted` in `vm_list`); `docs/reference/arm/2026-06-21-architecture-zoom-out.md` still describes the pre-M10 single-VM model (flagged with a banner, full rewrite deferred) |
| M11 — vCPU scheduler | planned (moved up from M14, 2026-07-19) | Full context switch (incl. FP/SIMD state — lifts the `-mgeneral-regs-only` no-save assumption), time slicing, vCPU count > pCPU count. Deliverable: M10's 2 VMs × 2 vCPUs time-sliced on 2 pCPUs (the other 2 pCPUs left idle for later Service VM work); both guests reach shells and concurrent FP workloads in both VMs run uncorrupted |
| M12 — Hypercall ABI + VM lifecycle | planned | HVC hypercall namespace (distinct from PSCI), VM create/start/pause/destroy, Service VM privilege concept. Deliverable: Service VM controls User VM start/stop via hypercalls |
| M13 — HSM kernel driver + io_req ring | planned | Custom Linux kernel module in the Service VM (modeled on `acrn_hsm`): ioctl interface, io_req shared-memory ring, forwarding User VM MMIO exits to Service VM userspace. Deliverable: a userspace program receives one User VM MMIO access and completes it. Highest-risk milestone — kept minimal on purpose (no virtio) |
| M14 — Device Model + virtio backends | planned | Userspace `dm` program: VM load/start, virtio-mmio console and blk backends. Deliverable: User VM launched by DM, rootfs on a virtio-blk image |
| M15 — RK3588 port | planned | Runtime FDT parsing, real UART/GIC/storage, board bring-up; reproduce the full chain on hardware |
| Deferred | — | SMMU/DMA isolation and the device-passthrough framework: not needed while all User VM devices are DM-emulated on QEMU; schedule when RK3588 passthrough demands it |

### Milestone renumbering (2026-08-10)

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

### ACRN-model strategy (decided 2026-07-14, revised 2026-07-19)

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

**Design principle:** My design philosophy is to move forward in small, fast iterations,
breaking requirements down into the smallest practical units and defining them clearly.
The goal is to achieve high cohesion and low coupling across the system.


## Reference Source Trees
The following production hypervisor source trees are available in the parent directory (`../`) for reference when making design or implementation decisions:

| Path | Project | Notes |
|------|---------|-------|
| `../acrn-hypervisor` | [ACRN](https://github.com/projectacrn/acrn-hypervisor) | Type-1, x86 ; primary structural inspiration for this repo's layout |
| `../xvisor` | [Xvisor](https://github.com/avpatel/xvisor-next) | Type-1, ARM-first; reference for Stage-2 MMU and vCPU design |
| `../bao-hypervisor` | local hypervisor | bao hypervisor  |

**When to consult these:** look up an existing implementation before designing any new subsystem (Stage-2 MMU, vGIC, PSCI, virtio, etc.).
Prefer reading the smallest relevant file rather than loading entire trees.

Import Reference
- [ARM Architecture Reference Manual (ARMv8-A)](https://developer.arm.com/documentation/ddi0487/latest)
- [ARM GIC Architecture Specification](https://developer.arm.com/documentation/ihi0069/latest)
- [pKVM (Protected KVM)](https://source.android.com/docs/core/virtualization)
