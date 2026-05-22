# Hypervisor — M0 (Hello EL2) Design

- **Date**: 2026-05-21
- **Project**: `hypervisor`
- **Milestone**: M0 — Hello EL2
- **Target platform (M0)**: QEMU `virt` (AArch64), GICv3, PL011 UART, Cortex-A72
- **Future port target**: Rockchip RK3588 (M4)
- **Language / build**: C + AArch64 GAS assembly + Makefile + Kconfig
- **License**: TBD (deferred — placeholder in repo)
- **Status**: Design approved, ready for implementation planning

---

## 1. Purpose & Positioning

This project is a **learning / research prototype** of an ARM hypervisor,
referencing ACRN, Xvisor and Xen. The directory layout closely follows ACRN;
the platform abstraction borrows a board-subdirectory idea from Xvisor to
cleanly host both QEMU `virt` and (later) RK3588 within `arch/arm64/`.

Goal of the whole project: a runnable Type-1 hypervisor that can boot a
Linux guest on ARM64. The path is broken into five milestones; **this
spec covers M0 only**.

### Project Roadmap (context for M0)

| Milestone | Goal | Indicative scope |
|---|---|---|
| **M0 — Hello EL2** *(this spec)* | Enter EL2 on QEMU virt, init UART, print banner | Boot asm, linker, vector stub, UART, build system, Kconfig skeleton |
| M1 — Bare-metal guest | Stage-2 MMU, minimal vCPU, run an EL1 bare-metal guest | Page tables, context switch, minimal trap handling |
| M2 — Multi-vCPU + interrupts | vGICv3, virtual timer, PSCI | Interrupt virtualization, SMP |
| M3 — Linux guest | Boot Linux to shell | virtio-console, DTB hand-off |
| M4 — RK3588 port | Run on real hardware | RK3588 board, UART, GIC adaptation |

Each subsequent milestone gets its own spec + plan + implementation cycle.

### Design Principle (project-wide)

Every file written in M0 must leave **room** for M1–M4 to add code, but
must not contain placeholder stubs for them. Directory skeleton is
complete; code is minimal-and-real.

---

## 2. Scope

### 2.1 In scope (M0)

- A bootable `hypervisor.elf` loadable by QEMU `-kernel` that:
  1. enters AArch64 EL2 from QEMU's reset state,
  2. asserts `CurrentEL == 0b1000`,
  3. parks non-boot CPUs in `wfi`,
  4. clears BSS,
  5. sets `SP_EL2`,
  6. loads `VBAR_EL2` with a panic-stub vector table,
  7. masks DAIF,
  8. transfers to C and prints `"[hv] Hello from EL2 on qemu_virt, CurrentEL=0x8\n"` over PL011,
  9. halts in `wfi` forever.
- Complete ACRN-style directory skeleton (with `.gitkeep` for empty subtrees).
- A board subdirectory `arch/arm64/board/qemu_virt/` providing platform
  constants and the linker script.
- A board subdirectory `arch/arm64/board/rk3588/` containing **only**
  `.gitkeep` — reserved for M4.
- A minimal Kconfig surface (3 options).
- A top-level `Makefile` with `all / run / clean / defconfig` targets.
- A `scripts/run-qemu.sh` invocation script.
- This design document.

### 2.2 Out of scope (M0)

- ❌ Any guest execution (Stage-2, vCPU, context switch).
- ❌ Real exception/interrupt handling (vector table = panic stubs only).
- ❌ MMU (M0 runs with identity mapping that QEMU provides; EL2 MMU is **not** enabled).
- ❌ SMP (non-boot CPUs `wfi` in `head.S`).
- ❌ Device tree parsing (DTB pointer in `x0` is preserved into `main` but not consumed).
- ❌ Full `kconfiglib` integration (M0 uses hand-written `.config` + Makefile `-D` injection).
- ❌ Full `printk` formatting (`%f`, width, precision, `%p` not supported).
- ❌ CI, unit tests, automated smoke scripts.
- ❌ Any `arch/x86/` content (directory holds `.gitkeep` only).
- ❌ Any `arch/arm64/board/rk3588/` content (directory holds `.gitkeep` only).

---

## 3. Directory Structure

ACRN-faithful skeleton with a small extension: `arch/arm64/board/<board>/`.

```
hypervisor/                          # project root
├── Makefile                         # top-level: make / make run / make clean / make defconfig
├── Kconfig                          # top-level Kconfig
├── .config                          # generated (gitignored)
├── README.md
├── LICENSE                          # placeholder: "TBD - see docs/"
├── .gitignore
│
├── hypervisor/                      # ★ hypervisor proper (mirrors ACRN's hypervisor/)
│   ├── Makefile
│   ├── Kconfig
│   │
│   ├── arch/
│   │   ├── arm64/
│   │   │   ├── Makefile
│   │   │   ├── Kconfig
│   │   │   ├── boot/
│   │   │   │   ├── head.S           # _start, EL2 entry, BSS clear, SP, jump to C
│   │   │   │   └── vectors.S        # EL2 vector table (panic stubs only)
│   │   │   ├── cpu/
│   │   │   │   └── cpu.c            # current_el(), cpu_relax(), wfi() wrappers
│   │   │   ├── board/               # ★ extension over plain ACRN
│   │   │   │   ├── qemu_virt/       # M0 only board
│   │   │   │   │   ├── board.c
│   │   │   │   │   ├── board.h
│   │   │   │   │   └── linker.lds
│   │   │   │   └── rk3588/
│   │   │   │       └── .gitkeep
│   │   │   └── include/
│   │   │       └── asm/             # arch-private headers (sysreg.h, etc.)
│   │   └── x86/
│   │       └── .gitkeep
│   │
│   ├── boot/                        # arch-independent boot glue
│   │   └── main.c                   # hypervisor_main() — C entry
│   │
│   ├── common/                      # arch-independent common code
│   │   └── .gitkeep                 # M1+ adds vm/, sched/, ...
│   │
│   ├── dm/                          # device model — .gitkeep
│   ├── hwmgmt/                      # platform-independent HW mgmt — .gitkeep
│   │
│   ├── lib/                         # kernel libc subset
│   │   ├── string.c                 # memset only (BSS clear in head.S; memcpy deferred to M1)
│   │   └── print.c                  # printk → uart_putc
│   │
│   ├── debug/                       # debug facilities
│   │   └── uart_pl011.c             # PL011 minimal driver: uart_init / uart_putc
│   │
│   └── include/                     # arch-independent public headers
│       ├── types.h                  # u8/u16/u32/u64, bool
│       ├── printk.h
│       └── uart.h                   # abstract: uart_init(base), uart_putc(c)
│
├── misc/                            # ACRN-style misc (configs, launch helpers) — .gitkeep
├── devicemodel/                     # userspace DM (Xen-style) — .gitkeep
│
├── scripts/
│   ├── run-qemu.sh                  # qemu-system-aarch64 invocation
│   └── kconfig/                     # placeholder (M0 uses hand-written .config)
│       └── .gitkeep
│
├── docs/
│   └── superpowers/
│       └── specs/
│           └── 2026-05-21-hypervisor-m0-design.md   # this file
│
└── tools/
    └── .gitkeep
```

### 3.1 Files written in M0

Twelve files with real content (plus the top-level Kconfig/Makefile and
README/LICENSE/.gitignore):

| Path | Purpose |
|---|---|
| `Makefile` | Top-level entrypoint |
| `Kconfig` | Top-level Kconfig |
| `README.md` | Quickstart + link to this spec |
| `LICENSE` | Placeholder `TBD` |
| `.gitignore` | `build/`, `.config`, editor cruft |
| `hypervisor/Makefile` | Sub-make for hypervisor.elf |
| `hypervisor/Kconfig` | Source arch Kconfig |
| `hypervisor/arch/arm64/Makefile` | Arch sub-make |
| `hypervisor/arch/arm64/Kconfig` | Arch + board choices |
| `hypervisor/arch/arm64/boot/head.S` | EL2 entry |
| `hypervisor/arch/arm64/boot/vectors.S` | Panic-stub vector table |
| `hypervisor/arch/arm64/cpu/cpu.c` | `current_el()`, `wfi()` wrappers |
| `hypervisor/arch/arm64/include/board.h` | Per-arch shim that re-includes the active board header |
| `hypervisor/arch/arm64/board/qemu_virt/board.c` | TU marker (will hold `board_info` from M1) |
| `hypervisor/arch/arm64/board/qemu_virt/board.h` | Board constants |
| `hypervisor/arch/arm64/board/qemu_virt/linker.lds` | Linker script |
| `hypervisor/boot/main.c` | `hypervisor_main()` |
| `hypervisor/debug/uart_pl011.c` | PL011 driver |
| `hypervisor/lib/string.c` | `memset` (`memcpy` deferred to M1) |
| `hypervisor/lib/print.c` | Minimal `printk` |
| `hypervisor/include/types.h` | Primitive typedefs |
| `hypervisor/include/printk.h` | `printk` prototype |
| `hypervisor/include/uart.h` | Abstract UART interface |
| `scripts/run-qemu.sh` | QEMU invocation |

All other directories listed in §3 hold a single `.gitkeep`.

### 3.2 Key layout decisions

1. **`board/` lives under `arch/arm64/`**, not at the project top level.
   This keeps board-specific code arch-coupled and mirrors how ACRN
   handles per-platform configuration under `hypervisor/arch/x86/configs/`.
2. **Driver vs board separation**: `hypervisor/debug/uart_pl011.c` is the
   PL011 *protocol* driver. `arch/arm64/board/qemu_virt/board.c` provides
   the *address* and clock. Future boards using PL011 reuse the driver;
   future boards using a different UART add a new driver and reuse the
   board structure.
3. **Empty directories use `.gitkeep`** rather than being omitted, so the
   ACRN-like skeleton is visible from day one and future milestones can
   add files without restructuring.

---

## 4. Boot Flow & Code Contracts

### 4.1 Load model

QEMU is invoked with `-kernel hypervisor.elf` and
`-machine virt,virtualization=on,gic-version=3`. QEMU enters the kernel
at the ELF entry point in **EL2**, with `x0` = DTB physical address (we
preserve this register but do not consume the DTB in M0).

### 4.2 Boot sequence

```
QEMU                head.S                              C
 │                    │                                  │
 │── jump to _start ──▶ 1. read MPIDR_EL1                │
 │                    │    non-boot CPU → secondary_park (wfi loop)
 │                    │ 2. read CurrentEL                │
 │                    │    assert == 0b1000, else panic_early ("!EL")
 │                    │ 3. SP_EL2 = __stack_top          │
 │                    │ 4. clear BSS                     │
 │                    │ 5. VBAR_EL2 = vectors            │
 │                    │ 6. msr daifset, #0xF             │
 │                    │ 7. isb / dsb sy                  │
 │                    │ 8. bl hypervisor_main ───────────▶ 9. uart_init(BOARD_UART_BASE)
 │                                                       10. printk("[hv] Hello from EL2, "
 │                                                                  "CurrentEL=0x%lx\n", el)
 │                                                       11. for(;;) wfi()
```

### 4.3 Linker layout (`board/qemu_virt/linker.lds`)

```
ENTRY(_start)
SECTIONS {
    . = 0x40080000;            /* QEMU virt default kernel load address */
    .text   : { *(.text._start) *(.text*) }
    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }
    .bss    : ALIGN(16) {
        __bss_start = .;
        *(.bss*) *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }
    . = ALIGN(16);
    . += 0x4000;                /* 16 KiB boot stack */
    __stack_top = .;
}
```

### 4.4 Public interfaces (M0 has exactly three)

| Interface | Caller | Implementer | Signature & contract |
|---|---|---|---|
| `void uart_init(uintptr_t base)` | `main.c` | `uart_pl011.c` | Configure PL011 at `base` for 115200 8N1 polling-mode TX; no RX in M0. |
| `void uart_putc(char c)` | `print.c` | `uart_pl011.c` | Spin until `TXFF` clear, write `DR`. No `\r` translation. |
| `int printk(const char *fmt, ...)` | `main.c` | `print.c` | Supports `%s %c %d %u %x %lx %%`. No width/precision/floats/`%p`. Returns number of characters emitted. |

### 4.5 Board info contract (`board/qemu_virt/board.h`)

```c
#define BOARD_UART_BASE  0x09000000UL   /* PL011 @ QEMU virt */
#define BOARD_DRAM_BASE  0x40000000UL
```

For M0 the macros above **are** the entire board contract. `board.c`
exists but is empty apart from a translation-unit marker comment; the
typed `struct board_info` is deferred to M1 when more fields make it
worthwhile.

To keep `hypervisor/boot/main.c` (arch-independent) free of arch/board
headers, the arch sub-Makefile passes include paths in this order:

```
-Ihypervisor/arch/arm64/board/$(BOARD)   ← searched first
-Ihypervisor/arch/arm64/include          ← searched second
```

So `main.c`'s `#include <board.h>` resolves **directly** to
`arch/arm64/board/qemu_virt/board.h` — the board directory wins because
it is listed first. `arch/arm64/include/board.h` is an intentionally
empty placeholder (include guard only); it is never reached by this
lookup and contains no `#include` forwarding.

**No magic numbers are allowed inside `uart_pl011.c`** — the driver
receives the base via `uart_init`.

### 4.6 Panic policy (M0)

- **Early panic (assembly, pre-C)**: if `CurrentEL != EL2`, jump to a
  minimal PL011 write loop that emits the fixed bytes `"!EL\n"` directly,
  then `wfi`. This is the only place where the PL011 base address is
  allowed to be hard-coded in assembly — explicitly chartered exception
  because C is not yet available.
- **Vector panic**: all 16 vector entries point to `panic_vector`, which
  reads `ESR_EL2`, calls `printk("!VEC ESR=0x%lx ELR=0x%lx\n", ...)`,
  then `wfi` loops.

### 4.7 Compiler options

```
-ffreestanding -nostdlib -nostartfiles
-fno-pic -fno-stack-protector
-mgeneral-regs-only -mstrict-align
-Wall -Wextra -Werror -O2 -g
```

`-mgeneral-regs-only` is mandatory: M0 does not save FP/SIMD state, so
the compiler must not emit instructions that touch those registers.

---

## 5. Build System & Kconfig

### 5.1 Top-level Makefile targets

| Target | Effect |
|---|---|
| `make` / `make all` | Build `build/hypervisor.elf` + `build/hypervisor.bin` |
| `make run` | Invoke `scripts/run-qemu.sh` |
| `make clean` | Remove `build/` |
| `make defconfig` | Copy `configs/qemu_virt_defconfig` to `.config` |
| `make menuconfig` | Reserved — not implemented in M0 |

### 5.2 Variables

```make
ARCH          ?= arm64
BOARD         ?= qemu_virt
CROSS_COMPILE ?= aarch64-none-linux-gnu-
```

`.config` is parsed by a tiny Makefile rule that converts `CONFIG_FOO=y`
into `-DCONFIG_FOO=1` for the compile line. No `kconfiglib` dependency
in M0.

### 5.3 Kconfig (M0)

```kconfig
# Top-level Kconfig
choice
    prompt "Target architecture"
    default ARCH_ARM64
config ARCH_ARM64
    bool "ARM64 (AArch64)"
endchoice

choice
    prompt "Target board"
    default BOARD_QEMU_VIRT
config BOARD_QEMU_VIRT
    bool "QEMU virt"
    depends on ARCH_ARM64
config BOARD_RK3588
    bool "Rockchip RK3588 (placeholder, M4)"
    depends on ARCH_ARM64
    # M0: selecting this will fail the build because
    # arch/arm64/board/rk3588/ contains only .gitkeep. The option is
    # exposed so the menu shape is stable across milestones.
endchoice

config DEBUG_UART
    bool "Early UART output"
    default y
```

### 5.4 Build artifacts

```
build/
├── obj/
│   ├── arch/arm64/boot/{head,vectors}.o
│   ├── arch/arm64/cpu/cpu.o
│   ├── arch/arm64/board/qemu_virt/board.o
│   ├── boot/main.o
│   ├── debug/uart_pl011.o
│   └── lib/{string,print}.o
├── hypervisor.elf
└── hypervisor.bin       # objcopy -O binary (kept for future use)
```

### 5.5 `scripts/run-qemu.sh`

```bash
#!/bin/sh
set -eu
qemu-system-aarch64 \
  -machine virt,virtualization=on,gic-version=3 \
  -cpu cortex-a72 -smp 1 -m 1G \
  -nographic -serial mon:stdio \
  -kernel build/hypervisor.elf \
  ${QEMU_EXTRA_ARGS:-}
```

Notes:
- `gic-version=3` is set now even though M0 does not touch GIC, so the
  script does not need editing when M2 adds vGICv3.
- Exit QEMU with `Ctrl-A x` (documented in `README.md`).
- `cortex-a72` is chosen over `max` to avoid SVE/other optional features
  affecting register state during early bring-up.

### 5.6 Toolchain requirements

- `aarch64-none-linux-gnu-gcc` ≥ 10 (pinned in `README.md`)
- `aarch64-none-linux-gnu-binutils`
- `qemu-system-aarch64` ≥ 6.0

---

## 6. Verification (Acceptance Criteria)

M0 verification is **manual**. No CI, no smoke script. Manual
checklist (also reproduced near the end of `README.md`):

- [ ] `make ARCH=arm64 BOARD=qemu_virt` builds cleanly with zero warnings (`-Werror`).
- [ ] `aarch64-none-linux-gnu-objdump -h build/hypervisor.elf` shows `.text` at `0x40080000`.
- [ ] `aarch64-none-linux-gnu-readelf -h build/hypervisor.elf` shows entry == `0x40080000`.
- [ ] `make run` produces the banner within 3 seconds:

  ```
  [hv] Hello from EL2 on qemu_virt, CurrentEL=0x8
  ```

- [ ] Banner's `CurrentEL` field reads `0x8` (i.e. `CurrentEL` register low 4 bits are `0b1000`).
- [ ] Temporarily editing `head.S` to assert `EL3` instead of `EL2`
  produces the `!EL` early-panic path. Revert after verifying.
- [ ] `Ctrl-A x` exits QEMU cleanly.

### 6.1 Debug aid (not part of acceptance)

`scripts/run-qemu.sh` accepts `QEMU_EXTRA_ARGS="-s -S"` to start a
gdbstub on port 1234. Connect with
`aarch64-none-linux-gnu-gdb build/hypervisor.elf -ex 'target remote :1234'`.
Useful but optional.

---

## 7. Risks & Mitigations

| Risk | Mitigation |
|---|---|
| QEMU does not enter EL2 unless `virtualization=on` | Hard-coded in `run-qemu.sh`; documented in `README.md`. |
| `-mgeneral-regs-only` conflicts with some GCC versions at `-O2` | Pin GCC ≥ 10 in `README.md`. |
| BSS not fully cleared → garbled banner | Loop-based clear in `head.S`; verified indirectly by the "intentionally break EL assertion" item in §6. |
| Future RK3588 UART is not PL011 | §3 already separates driver from board; M4 adds a new driver, board file stays small. |
| Makefile drops a header dependency | M0 file count is tiny; full rebuild is acceptable. M1+ introduces `-MMD -MP`. |

---

## 8. Forward-Compatibility Contracts

The following are reserved for M1 — directories exist in M0 but stay
empty:

| Path | Purpose (M1+) |
|---|---|
| `hypervisor/common/vm/` | VM descriptors, vCPU structures |
| `hypervisor/common/sched/` | Scheduler |
| `hypervisor/arch/arm64/mmu/` | Stage-1 / Stage-2 page tables |
| `hypervisor/arch/arm64/vmexit/` | Real trap handlers |
| `hypervisor/include/vm.h` | VM / vCPU types |

M0 interfaces (`uart.h`, `printk.h`, the `BOARD_*` macros) are
**source-stable**: M1+ may extend them additively but must not break
existing call sites.

---

## 9. Post-M0 Transition

When the acceptance checklist in §6 passes:

1. Commit the resulting tree.
2. Open a new spec at
   `docs/superpowers/specs/2026-XX-XX-hypervisor-m1-bare-metal-guest-design.md`
   covering Stage-2 MMU, the first vCPU, and a minimal EL1 guest.
3. Re-run the brainstorming → writing-plans → executing-plans loop for M1.

End of M0 design.
