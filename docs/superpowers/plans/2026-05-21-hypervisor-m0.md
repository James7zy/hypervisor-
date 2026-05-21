# Hypervisor M0 (Hello EL2) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Produce a bootable `hypervisor.elf` that QEMU virt enters in EL2 and that prints `[hv] Hello from EL2, CurrentEL=0x8` over PL011 UART, with the full ACRN-style directory skeleton in place.

**Architecture:** Bottom-up. First lay out the empty ACRN-style directory tree, then add headers, then libs (`memset`/`memcpy`, minimal `printk`), then the PL011 driver, then the C entry, then the AArch64 boot assembly + linker script, then the Makefile/Kconfig that ties everything into `make` and `make run`. Final task is the end-to-end manual acceptance against the spec checklist.

**Tech Stack:** C (freestanding), AArch64 GAS assembly, GNU `make`, hand-written `.config` (no Kconfig parser), `aarch64-linux-gnu-gcc` ≥ 10, `qemu-system-aarch64` ≥ 6.0.

**Spec reference:** `docs/superpowers/specs/2026-05-21-hypervisor-m0-design.md`

---

## Notes for the implementing engineer (read first)

You may be entirely new to ARM hypervisors and to this codebase. Here is what you must know before starting:

1. **There is no test framework.** This is a bare-metal hypervisor that runs in EL2 inside QEMU. The "test" for every task is one of:
   - **Build check** — `make` must succeed with no warnings (`-Werror` is on).
   - **Static inspection** — `objdump`/`readelf`/`grep` confirms the output has the right shape.
   - **Run + observe** — `make run` and read the serial output.

   Treat these as your red/green substitutes.

2. **Order matters.** Each task depends on the previous one. Do them in order. Do not skip ahead.

3. **Commit after every task.** Frequent commits are how we keep iteration cheap and bisectable. The commit command is provided in each task's final step.

4. **EL2 = the hypervisor's privilege level on ARMv8-A.** `CurrentEL` is a system register; its bits [3:2] hold the EL number, so `EL2` reads as `0b1000` = `0x8`. We assert this on boot — if QEMU drops us in any other EL, we panic.

5. **`-mgeneral-regs-only`** forbids GCC from emitting FP/SIMD instructions. This is mandatory because M0 does not save/restore FP state. If a build error mentions FP regs, do not "fix" the warning by enabling FP — fix the source instead.

6. **No magic numbers in the PL011 driver.** The driver receives the base address from `uart_init(base)`. Only `head.S`'s pre-C early-panic path is allowed to hard-code the PL011 address (it has no other option — C is not running yet).

7. **The Makefile uses a simple `.config` parser** — every line of the form `CONFIG_FOO=y` becomes `-DCONFIG_FOO=1`. No Kconfig tooling. The provided `configs/qemu_virt_defconfig` is just three lines.

8. **If something doesn't fit the spec, stop and ask.** Do not invent. Do not paper over. The spec is the contract.

---

## File map (locked-in decomposition)

| Path | Created in task | Responsibility |
|---|---|---|
| `.gitignore` | already exists | — |
| `LICENSE` | T1 | Placeholder `TBD` |
| `README.md` | T1 | Quickstart + spec link |
| `Makefile` | T11 | Top-level entry: `all/run/clean/defconfig` |
| `Kconfig` | T11 | Top-level Kconfig |
| `configs/qemu_virt_defconfig` | T11 | Three-line defconfig |
| `hypervisor/Makefile` | T11 | Sub-make orchestrator |
| `hypervisor/Kconfig` | T11 | `source` arch Kconfig |
| `hypervisor/arch/arm64/Makefile` | T11 | Arch sub-make: object list + include paths |
| `hypervisor/arch/arm64/Kconfig` | T11 | Arch/board choice |
| `hypervisor/include/types.h` | T2 | `u8`/`u16`/`u32`/`u64`/`bool`/`uintptr_t` |
| `hypervisor/include/uart.h` | T2 | `uart_init`/`uart_putc` prototypes |
| `hypervisor/include/printk.h` | T2 | `printk` prototype |
| `hypervisor/arch/arm64/include/board.h` | T2 | Per-arch shim — forwards to active board |
| `hypervisor/arch/arm64/board/qemu_virt/board.h` | T2 | `BOARD_UART_BASE` etc. |
| `hypervisor/arch/arm64/board/qemu_virt/board.c` | T2 | TU marker |
| `hypervisor/lib/string.c` | T3 | `memset`, `memcpy` |
| `hypervisor/lib/print.c` | T4 | Minimal `printk` |
| `hypervisor/debug/uart_pl011.c` | T5 | PL011 driver |
| `hypervisor/boot/main.c` | T6 | `hypervisor_main()` |
| `hypervisor/arch/arm64/cpu/cpu.c` | T7 | `current_el()`, `wfi()` wrappers |
| `hypervisor/arch/arm64/include/asm/sysreg.h` | T7 | Sysreg read/write macros |
| `hypervisor/arch/arm64/boot/vectors.S` | T8 | Panic-stub vector table |
| `hypervisor/arch/arm64/boot/head.S` | T9 | `_start`, EL2 entry, BSS clear, jump to C |
| `hypervisor/arch/arm64/board/qemu_virt/linker.lds` | T10 | Linker script |
| `scripts/run-qemu.sh` | T11 | QEMU invocation |

`.gitkeep` empty-dir placeholders are created in T1.

---

## Task 1: Create the ACRN-style directory skeleton

**Goal:** All directories in spec §3 exist, with `.gitkeep` in every empty one. README and LICENSE placeholders in place.

**Files:**
- Create: `LICENSE`
- Create: `README.md`
- Create: 14 `.gitkeep` files (see Step 2)

- [ ] **Step 1: Create the directory tree**

Run from the project root (`/home/ubuntu/Music/virtual/hypervisor-`):

```bash
mkdir -p hypervisor/arch/arm64/boot
mkdir -p hypervisor/arch/arm64/cpu
mkdir -p hypervisor/arch/arm64/board/qemu_virt
mkdir -p hypervisor/arch/arm64/board/rk3588
mkdir -p hypervisor/arch/arm64/include/asm
mkdir -p hypervisor/arch/x86
mkdir -p hypervisor/boot
mkdir -p hypervisor/common
mkdir -p hypervisor/dm
mkdir -p hypervisor/hwmgmt
mkdir -p hypervisor/lib
mkdir -p hypervisor/debug
mkdir -p hypervisor/include
mkdir -p misc
mkdir -p devicemodel
mkdir -p scripts/kconfig
mkdir -p configs
mkdir -p tools
```

- [ ] **Step 2: Place `.gitkeep` in every directory that ends M0 empty**

```bash
touch hypervisor/arch/arm64/board/rk3588/.gitkeep
touch hypervisor/arch/x86/.gitkeep
touch hypervisor/common/.gitkeep
touch hypervisor/dm/.gitkeep
touch hypervisor/hwmgmt/.gitkeep
touch misc/.gitkeep
touch devicemodel/.gitkeep
touch scripts/kconfig/.gitkeep
touch tools/.gitkeep
touch hypervisor/arch/arm64/include/asm/.gitkeep
```

Note: `hypervisor/arch/arm64/include/asm/.gitkeep` will be removed in T7 when `sysreg.h` is added. Same for any other directory that gains real files later — that's expected.

- [ ] **Step 3: Write `LICENSE`**

```
TBD - see docs/
```

(One line, no trailing license text. The spec defers license selection.)

- [ ] **Step 4: Write `README.md`**

````markdown
# hypervisor

A learning/research ARM hypervisor inspired by ACRN, Xvisor, and Xen.

## Status

**M0 — Hello EL2** (current milestone). The hypervisor enters EL2 on QEMU
virt and prints a banner. No guest support yet.

See [the M0 design](docs/superpowers/specs/2026-05-21-hypervisor-m0-design.md).

## Quickstart

```sh
make defconfig
make
make run
```

Expected output:

```
[hv] Hello from EL2, CurrentEL=0x8
```

Exit QEMU with `Ctrl-A x`.

## Requirements

- `aarch64-linux-gnu-gcc` ≥ 10
- `aarch64-linux-gnu-binutils`
- `qemu-system-aarch64` ≥ 6.0

## Acceptance checklist (M0)

- [ ] `make` builds cleanly with zero warnings.
- [ ] `aarch64-linux-gnu-objdump -h build/hypervisor.elf` shows `.text` at `0x40080000`.
- [ ] `aarch64-linux-gnu-readelf -h build/hypervisor.elf` shows entry == `0x40080000`.
- [ ] `make run` prints the banner within 3 seconds.
- [ ] Banner's `CurrentEL` reads `0x8`.
- [ ] Temporarily flipping the EL assertion in `head.S` to demand EL3
      triggers the `!EL` early-panic path. Revert after verifying.
- [ ] `Ctrl-A x` exits QEMU cleanly.

## License

TBD — see `LICENSE`.
````

- [ ] **Step 5: Verify directory shape**

Run:

```bash
find . -type d -not -path './.git*' -not -path './docs*' | sort
```

Expected (exact, 20 entries):

```
.
./configs
./devicemodel
./hypervisor
./hypervisor/arch
./hypervisor/arch/arm64
./hypervisor/arch/arm64/board
./hypervisor/arch/arm64/board/qemu_virt
./hypervisor/arch/arm64/board/rk3588
./hypervisor/arch/arm64/boot
./hypervisor/arch/arm64/cpu
./hypervisor/arch/arm64/include
./hypervisor/arch/arm64/include/asm
./hypervisor/arch/x86
./hypervisor/boot
./hypervisor/common
./hypervisor/debug
./hypervisor/dm
./hypervisor/hwmgmt
./hypervisor/include
./hypervisor/lib
./misc
./scripts
./scripts/kconfig
./tools
```

If counts differ, fix before proceeding.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "chore: scaffold ACRN-style directory skeleton + LICENSE/README"
```

---

## Task 2: Public headers (`types.h`, `uart.h`, `printk.h`, `board.h` chain)

**Goal:** Establish the M0 interface surface. Nothing builds yet — these are headers, but every later task references them.

**Files:**
- Create: `hypervisor/include/types.h`
- Create: `hypervisor/include/uart.h`
- Create: `hypervisor/include/printk.h`
- Create: `hypervisor/arch/arm64/include/board.h`
- Create: `hypervisor/arch/arm64/board/qemu_virt/board.h`
- Create: `hypervisor/arch/arm64/board/qemu_virt/board.c`
- Delete: `hypervisor/arch/arm64/include/asm/.gitkeep` is left alone for now (T7 removes).

- [ ] **Step 1: Write `hypervisor/include/types.h`**

```c
/* SPDX-License-Identifier: TBD */
#ifndef HV_TYPES_H
#define HV_TYPES_H

typedef unsigned char        u8;
typedef unsigned short       u16;
typedef unsigned int         u32;
typedef unsigned long        u64;

typedef signed char          s8;
typedef signed short         s16;
typedef signed int           s32;
typedef signed long          s64;

typedef unsigned long        uintptr_t;
typedef unsigned long        size_t;
typedef signed long          ssize_t;

typedef _Bool                bool;
#define true                 ((bool)1)
#define false                ((bool)0)

#define NULL                 ((void *)0)

#endif /* HV_TYPES_H */
```

- [ ] **Step 2: Write `hypervisor/include/uart.h`**

```c
/* SPDX-License-Identifier: TBD */
#ifndef HV_UART_H
#define HV_UART_H

#include <types.h>

/* Configure UART at `base` for 115200 8N1, TX polling. No RX in M0. */
void uart_init(uintptr_t base);

/* Spin until TX FIFO has room, then send `c`. No \r translation. */
void uart_putc(char c);

#endif /* HV_UART_H */
```

- [ ] **Step 3: Write `hypervisor/include/printk.h`**

```c
/* SPDX-License-Identifier: TBD */
#ifndef HV_PRINTK_H
#define HV_PRINTK_H

#include <types.h>

/*
 * Minimal printk. Supports: %s %c %d %u %x %lx %%
 * No width, precision, floats, or %p.
 * Returns number of characters emitted.
 */
int printk(const char *fmt, ...);

#endif /* HV_PRINTK_H */
```

- [ ] **Step 4: Write `hypervisor/arch/arm64/board/qemu_virt/board.h`**

```c
/* SPDX-License-Identifier: TBD */
#ifndef BOARD_QEMU_VIRT_H
#define BOARD_QEMU_VIRT_H

/* PL011 base on QEMU virt machine. See qemu/hw/arm/virt.c. */
#define BOARD_UART_BASE   0x09000000UL
#define BOARD_UART_CLK_HZ 24000000U
#define BOARD_DRAM_BASE   0x40000000UL

#endif /* BOARD_QEMU_VIRT_H */
```

This is the file that `main.c`'s `#include <board.h>` resolves to. The
arch sub-make's `-Ihypervisor/arch/arm64/board/$(BOARD)` (added in T11)
places this directory on the include path, so arch-independent code
gets the active board's constants without naming the board in source.

- [ ] **Step 5: Write `hypervisor/arch/arm64/include/board.h` (forward-compat placeholder)**

```c
/* SPDX-License-Identifier: TBD */
#ifndef HV_ARCH_BOARD_H
#define HV_ARCH_BOARD_H

/*
 * Per-arch board indirection header (M0 placeholder).
 *
 * In M0 this file is NOT on the include resolution path for the
 * `<board.h>` token used by main.c — the arch sub-Makefile lists the
 * board directory first, so the board's own board.h wins.
 *
 * This file exists so that when M1+ introduces a typed `struct
 * board_info` we have an obvious place for arch-level board glue
 * (e.g. forward declarations, MMIO accessor wrappers) without
 * restructuring directories.
 *
 * Intentionally empty in M0 apart from the include guard.
 */

#endif /* HV_ARCH_BOARD_H */
```

(Plan note: spec §4.5 describes both `arch/arm64/include/board.h` and
`arch/arm64/board/qemu_virt/board.h`, with the arch one "shimming" the
board one. Because both files share the name `board.h`, the C
preprocessor cannot make the shim include the board version
unambiguously. In practice the `-I` ordering in T11 makes the board's
`board.h` win directly, and the arch file is unreferenced in M0. We
keep the arch file as a documented placeholder so the directory shape
remains exactly as the spec listed it.)

- [ ] **Step 6: Write `hypervisor/arch/arm64/board/qemu_virt/board.c`**

```c
/* SPDX-License-Identifier: TBD */

/*
 * Translation-unit marker. M0 holds nothing here; M1 introduces a
 * typed `struct board_info` constant.
 */
```

This file exists to keep `arch/arm64/Makefile`'s object list non-empty for the board directory and to give M1 a place to add `struct board_info` without restructuring.

- [ ] **Step 7: Compile-check the header chain**

This is a sanity pass — `<board.h>` must resolve to the board's header
when both arch and board directories are on the include path, with the
board directory listed first.

```bash
aarch64-linux-gnu-gcc -ffreestanding -nostdlib \
  -Ihypervisor/include \
  -Ihypervisor/arch/arm64/board/qemu_virt \
  -Ihypervisor/arch/arm64/include \
  -c -x c -o /tmp/hv_hdr_check.o - <<'EOF'
#include <types.h>
#include <uart.h>
#include <printk.h>
#include <board.h>

int test(void) {
    return (int)BOARD_UART_BASE;
}
EOF
```

Expected: silent success (exit 0), no warnings. `/tmp/hv_hdr_check.o` is created. Discard it.

If you see `error: 'BOARD_UART_BASE' undeclared`, the include-path
order is wrong — the board directory must come before the arch include
directory.

- [ ] **Step 8: Commit**

```bash
git add -A
git commit -m "feat(headers): add types/uart/printk + board.h chain

main.c (arch-independent) resolves <board.h> to
hypervisor/arch/arm64/board/qemu_virt/board.h via -I order in the arch
sub-Makefile. hypervisor/arch/arm64/include/board.h is reserved as a
documented placeholder for M1+ board glue."
```

---

## Task 3: lib/string.c — `memset` and `memcpy`

**Goal:** Smallest possible freestanding `memset`/`memcpy`. Used by `head.S`'s BSS clear fallback and by `print.c`.

**Files:**
- Create: `hypervisor/lib/string.c`

- [ ] **Step 1: Write `hypervisor/lib/string.c`**

```c
/* SPDX-License-Identifier: TBD */
#include <types.h>

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    while (n--) {
        *d++ = (unsigned char)c;
    }
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}
```

Note: byte-at-a-time on purpose. `-mstrict-align` (T11) forbids unaligned word access, and M0 does not have a working `memcpy` benchmark — correctness over speed.

- [ ] **Step 2: Compile-check**

```bash
aarch64-linux-gnu-gcc -ffreestanding -nostdlib -nostartfiles \
  -fno-pic -fno-stack-protector -mgeneral-regs-only -mstrict-align \
  -Wall -Wextra -Werror -O2 -g \
  -Ihypervisor/include \
  -c -o /tmp/string.o hypervisor/lib/string.c
```

Expected: silent success. Discard `/tmp/string.o`.

- [ ] **Step 3: Verify no FP/SIMD instructions were emitted**

```bash
aarch64-linux-gnu-gcc -ffreestanding -nostdlib -nostartfiles \
  -fno-pic -fno-stack-protector -mgeneral-regs-only -mstrict-align \
  -O2 -Ihypervisor/include \
  -S -o /tmp/string.s hypervisor/lib/string.c
grep -E 'v[0-9]+|q[0-9]+|fmov|fadd|fmul' /tmp/string.s && echo FAIL || echo OK
rm -f /tmp/string.s
```

Expected: `OK` (grep returns 1).

- [ ] **Step 4: Commit**

```bash
git add hypervisor/lib/string.c
git commit -m "feat(lib): add minimal memset/memcpy (byte-at-a-time)"
```

---

## Task 4: lib/print.c — minimal `printk`

**Goal:** A `printk` that supports `%s %c %d %u %x %lx %%` and calls `uart_putc` for every byte. Returns character count.

**Files:**
- Create: `hypervisor/lib/print.c`

- [ ] **Step 1: Write `hypervisor/lib/print.c`**

```c
/* SPDX-License-Identifier: TBD */
#include <types.h>
#include <printk.h>
#include <uart.h>

#include <stdarg.h>

static int emit_char(char c)
{
    uart_putc(c);
    return 1;
}

static int emit_string(const char *s)
{
    int n = 0;
    if (!s) {
        s = "(null)";
    }
    while (*s) {
        n += emit_char(*s++);
    }
    return n;
}

static int emit_signed(s64 v)
{
    char buf[24];
    int n = 0;
    int len = 0;
    u64 mag;

    if (v < 0) {
        n += emit_char('-');
        mag = (u64)(-(v + 1)) + 1; /* avoids UB on LLONG_MIN */
    } else {
        mag = (u64)v;
    }

    /* Decimal digits, low-to-high then reverse. */
    do {
        buf[len++] = (char)('0' + (mag % 10));
        mag /= 10;
    } while (mag);

    while (len--) {
        n += emit_char(buf[len]);
    }
    return n;
}

static int emit_unsigned(u64 v)
{
    char buf[24];
    int n = 0;
    int len = 0;

    do {
        buf[len++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v);

    while (len--) {
        n += emit_char(buf[len]);
    }
    return n;
}

static int emit_hex(u64 v)
{
    static const char digits[] = "0123456789abcdef";
    char buf[16];
    int n = 0;
    int len = 0;

    do {
        buf[len++] = digits[v & 0xF];
        v >>= 4;
    } while (v);

    while (len--) {
        n += emit_char(buf[len]);
    }
    return n;
}

int printk(const char *fmt, ...)
{
    va_list ap;
    int n = 0;

    va_start(ap, fmt);

    while (*fmt) {
        if (*fmt != '%') {
            n += emit_char(*fmt++);
            continue;
        }

        fmt++; /* skip '%' */
        switch (*fmt) {
        case '\0':
            goto done;
        case '%':
            n += emit_char('%');
            break;
        case 'c':
            n += emit_char((char)va_arg(ap, int));
            break;
        case 's':
            n += emit_string(va_arg(ap, const char *));
            break;
        case 'd':
            n += emit_signed((s64)va_arg(ap, int));
            break;
        case 'u':
            n += emit_unsigned((u64)va_arg(ap, unsigned int));
            break;
        case 'x':
            n += emit_hex((u64)va_arg(ap, unsigned int));
            break;
        case 'l':
            fmt++;
            if (*fmt == 'x') {
                n += emit_hex((u64)va_arg(ap, unsigned long));
            } else {
                /* Unsupported %l<other>; emit literally. */
                n += emit_char('%');
                n += emit_char('l');
                if (*fmt) {
                    n += emit_char(*fmt);
                } else {
                    goto done;
                }
            }
            break;
        default:
            /* Unknown specifier: emit %c literally. */
            n += emit_char('%');
            n += emit_char(*fmt);
            break;
        }
        fmt++;
    }

done:
    va_end(ap);
    return n;
}
```

- [ ] **Step 2: Compile-check**

```bash
aarch64-linux-gnu-gcc -ffreestanding -nostdlib -nostartfiles \
  -fno-pic -fno-stack-protector -mgeneral-regs-only -mstrict-align \
  -Wall -Wextra -Werror -O2 -g \
  -Ihypervisor/include \
  -c -o /tmp/print.o hypervisor/lib/print.c
```

Expected: silent success. Discard `/tmp/print.o`.

If GCC complains about `stdarg.h` (it shouldn't — `stdarg.h` is one of the headers freestanding C is required to provide), check that you have the cross-toolchain's `gcc` not just `cpp`.

- [ ] **Step 3: Verify no FP/SIMD emission**

```bash
aarch64-linux-gnu-gcc -ffreestanding -nostdlib -nostartfiles \
  -fno-pic -fno-stack-protector -mgeneral-regs-only -mstrict-align \
  -O2 -Ihypervisor/include \
  -S -o /tmp/print.s hypervisor/lib/print.c
grep -E '\bv[0-9]+\.|q[0-9]+|fmov|fadd|fmul' /tmp/print.s && echo FAIL || echo OK
rm -f /tmp/print.s
```

Expected: `OK`.

- [ ] **Step 4: Commit**

```bash
git add hypervisor/lib/print.c
git commit -m "feat(lib): add minimal printk (%s %c %d %u %x %lx %%)"
```

---

## Task 5: debug/uart_pl011.c — PL011 driver

**Goal:** Polling-mode PL011 driver. `uart_init(base)` configures 115200-8N1 TX; `uart_putc` busy-waits on `TXFF` then writes `DR`.

**Files:**
- Create: `hypervisor/debug/uart_pl011.c`

PL011 register layout (relative to base, all 32-bit MMIO):

| Offset | Name | Purpose |
|---|---|---|
| 0x000 | DR | Data |
| 0x018 | FR | Flag register; bit 5 = TXFF (TX FIFO full) |
| 0x024 | IBRD | Integer baud rate divisor |
| 0x028 | FBRD | Fractional baud rate divisor |
| 0x02C | LCR_H | Line control: word length, FIFO |
| 0x030 | CR | Control: enable bits |
| 0x038 | IMSC | Interrupt mask set/clear |

Baud divisor for 115200 @ 24 MHz: divisor = `clk / (16 * baud)` = `24_000_000 / 1_843_200` = 13.0208…
- IBRD = 13
- FBRD = round(0.0208 * 64) = 1

- [ ] **Step 1: Write `hypervisor/debug/uart_pl011.c`**

```c
/* SPDX-License-Identifier: TBD */
#include <types.h>
#include <uart.h>

/* PL011 register offsets. */
#define UART_DR        0x000
#define UART_FR        0x018
#define UART_IBRD      0x024
#define UART_FBRD      0x028
#define UART_LCR_H     0x02C
#define UART_CR        0x030
#define UART_IMSC      0x038

/* FR bits. */
#define FR_TXFF        (1U << 5)

/* LCR_H bits. */
#define LCR_H_WLEN_8   (3U << 5)
#define LCR_H_FEN      (1U << 4)

/* CR bits. */
#define CR_UARTEN      (1U << 0)
#define CR_TXE         (1U << 8)
#define CR_RXE         (1U << 9)

/*
 * Baud divisor for 115200 baud assuming the spec's BOARD_UART_CLK_HZ
 * (24 MHz on QEMU virt). On QEMU the divisor is effectively ignored,
 * but real PL011 hardware honors it. We set it so the driver stays
 * correct when re-targeted to a real board with a 24 MHz clock.
 */
#define BAUD_IBRD      13U
#define BAUD_FBRD      1U

static volatile u32 *uart_base;

static inline void mmio_write32(uintptr_t addr, u32 val)
{
    *(volatile u32 *)addr = val;
}

static inline u32 mmio_read32(uintptr_t addr)
{
    return *(volatile u32 *)addr;
}

void uart_init(uintptr_t base)
{
    uart_base = (volatile u32 *)base;

    /* Disable UART before reconfiguring. */
    mmio_write32(base + UART_CR, 0);

    /* Mask all interrupts (M0 is polling only). */
    mmio_write32(base + UART_IMSC, 0);

    /* Set baud rate. */
    mmio_write32(base + UART_IBRD, BAUD_IBRD);
    mmio_write32(base + UART_FBRD, BAUD_FBRD);

    /* 8N1, FIFO enabled. */
    mmio_write32(base + UART_LCR_H, LCR_H_WLEN_8 | LCR_H_FEN);

    /* Enable UART, TX, RX (RX kept enabled so the user can paste input
     * for future debugging; M0 ignores received bytes). */
    mmio_write32(base + UART_CR, CR_UARTEN | CR_TXE | CR_RXE);
}

void uart_putc(char c)
{
    uintptr_t base = (uintptr_t)uart_base;

    while (mmio_read32(base + UART_FR) & FR_TXFF) {
        /* spin */
    }
    mmio_write32(base + UART_DR, (u32)(unsigned char)c);
}
```

- [ ] **Step 2: Compile-check**

```bash
aarch64-linux-gnu-gcc -ffreestanding -nostdlib -nostartfiles \
  -fno-pic -fno-stack-protector -mgeneral-regs-only -mstrict-align \
  -Wall -Wextra -Werror -O2 -g \
  -Ihypervisor/include \
  -c -o /tmp/uart_pl011.o hypervisor/debug/uart_pl011.c
```

Expected: silent success. Discard the object.

- [ ] **Step 3: Confirm no magic-number leakage**

The spec forbids hard-coded UART addresses in this file. Check:

```bash
grep -nE '0x09000000|0x0900_0000' hypervisor/debug/uart_pl011.c && echo FAIL || echo OK
```

Expected: `OK`.

- [ ] **Step 4: Commit**

```bash
git add hypervisor/debug/uart_pl011.c
git commit -m "feat(debug): add PL011 polling-mode UART driver"
```

---

## Task 6: boot/main.c — C entry point

**Goal:** `hypervisor_main()` — receives DTB pointer (currently unused), calls `uart_init`, prints the banner, halts.

**Files:**
- Create: `hypervisor/boot/main.c`

- [ ] **Step 1: Write `hypervisor/boot/main.c`**

```c
/* SPDX-License-Identifier: TBD */
#include <types.h>
#include <uart.h>
#include <printk.h>
#include <board.h>

extern u64 read_currentel(void);  /* defined in arch/arm64/cpu/cpu.c */
extern void cpu_wfi(void);        /* defined in arch/arm64/cpu/cpu.c */

void hypervisor_main(uintptr_t dtb_phys)
{
    (void)dtb_phys; /* M0: DTB is preserved but unused. M3 will parse it. */

    uart_init(BOARD_UART_BASE);

    u64 el = read_currentel();
    printk("[hv] Hello from EL2, CurrentEL=0x%lx\n", el);

    for (;;) {
        cpu_wfi();
    }
}
```

Note: `read_currentel` reads the raw `CurrentEL` system register (returns `0x8` when in EL2 — bits [3:2] = `0b10`, the low two bits are RES0). The banner prints the raw value, matching spec §4.2 step 10 / §6 acceptance criterion.

- [ ] **Step 2: Compile-check**

```bash
aarch64-linux-gnu-gcc -ffreestanding -nostdlib -nostartfiles \
  -fno-pic -fno-stack-protector -mgeneral-regs-only -mstrict-align \
  -Wall -Wextra -Werror -O2 -g \
  -Ihypervisor/include \
  -Ihypervisor/arch/arm64/board/qemu_virt \
  -Ihypervisor/arch/arm64/include \
  -c -o /tmp/main.o hypervisor/boot/main.c
```

Expected: silent success. Discard the object.

- [ ] **Step 3: Commit**

```bash
git add hypervisor/boot/main.c
git commit -m "feat(boot): add hypervisor_main C entry"
```

---

## Task 7: arch/arm64/cpu/cpu.c + sysreg.h

**Goal:** Provide `read_currentel()` and `cpu_wfi()` used by `main.c`. Define a sysreg-access helper macro for reuse later.

**Files:**
- Create: `hypervisor/arch/arm64/include/asm/sysreg.h`
- Create: `hypervisor/arch/arm64/cpu/cpu.c`
- Delete: `hypervisor/arch/arm64/include/asm/.gitkeep` (replaced by real header)

- [ ] **Step 1: Remove the placeholder in `asm/`**

```bash
rm hypervisor/arch/arm64/include/asm/.gitkeep
```

- [ ] **Step 2: Write `hypervisor/arch/arm64/include/asm/sysreg.h`**

```c
/* SPDX-License-Identifier: TBD */
#ifndef HV_ASM_SYSREG_H
#define HV_ASM_SYSREG_H

#include <types.h>

#define SYSREG_READ(reg) ({                                \
    u64 __v;                                               \
    __asm__ volatile("mrs %0, " #reg : "=r"(__v));         \
    __v;                                                   \
})

#define SYSREG_WRITE(reg, val) do {                        \
    u64 __v = (val);                                       \
    __asm__ volatile("msr " #reg ", %0" :: "r"(__v));      \
} while (0)

#endif /* HV_ASM_SYSREG_H */
```

- [ ] **Step 3: Write `hypervisor/arch/arm64/cpu/cpu.c`**

```c
/* SPDX-License-Identifier: TBD */
#include <types.h>
#include <asm/sysreg.h>

u64 read_currentel(void)
{
    return SYSREG_READ(CurrentEL);
}

void cpu_wfi(void)
{
    __asm__ volatile("wfi");
}

void cpu_relax(void)
{
    __asm__ volatile("yield");
}
```

- [ ] **Step 4: Compile-check**

```bash
aarch64-linux-gnu-gcc -ffreestanding -nostdlib -nostartfiles \
  -fno-pic -fno-stack-protector -mgeneral-regs-only -mstrict-align \
  -Wall -Wextra -Werror -O2 -g \
  -Ihypervisor/include \
  -Ihypervisor/arch/arm64/include \
  -c -o /tmp/cpu.o hypervisor/arch/arm64/cpu/cpu.c
```

Expected: silent success.

- [ ] **Step 5: Verify the assembly reads `CurrentEL` (not something else)**

```bash
aarch64-linux-gnu-gcc -ffreestanding -nostdlib -nostartfiles \
  -O2 -Ihypervisor/include -Ihypervisor/arch/arm64/include \
  -S -o /tmp/cpu.s hypervisor/arch/arm64/cpu/cpu.c
grep -i 'mrs.*currentel' /tmp/cpu.s
rm -f /tmp/cpu.s
```

Expected: a line like `mrs x0, currentel` (case may vary).

- [ ] **Step 6: Commit**

```bash
git add hypervisor/arch/arm64/include/asm/sysreg.h \
        hypervisor/arch/arm64/cpu/cpu.c
git rm hypervisor/arch/arm64/include/asm/.gitkeep
git commit -m "feat(arch/arm64): add sysreg helpers + cpu.c (read_currentel/wfi)"
```

---

## Task 8: arch/arm64/boot/vectors.S — panic-stub vector table

**Goal:** 16 vector entries, all forwarding to a single `panic_vector` label that reads ESR/ELR and dies. Real handlers come in M1.

**Files:**
- Create: `hypervisor/arch/arm64/boot/vectors.S`

ARMv8-A vector table layout: 16 entries × 128 bytes (`0x80`), table aligned to 2048 bytes (`0x800`):

| Offset | Source EL → Target EL | Synchronous / IRQ / FIQ / SError |
|---|---|---|
| 0x000 | Current EL, SP_EL0 | Sync |
| 0x080 | Current EL, SP_EL0 | IRQ |
| 0x100 | Current EL, SP_EL0 | FIQ |
| 0x180 | Current EL, SP_EL0 | SError |
| 0x200 | Current EL, SP_ELx | Sync |
| 0x280 | Current EL, SP_ELx | IRQ |
| 0x300 | Current EL, SP_ELx | FIQ |
| 0x380 | Current EL, SP_ELx | SError |
| 0x400 | Lower EL, AArch64 | Sync |
| 0x480 | Lower EL, AArch64 | IRQ |
| 0x500 | Lower EL, AArch64 | FIQ |
| 0x580 | Lower EL, AArch64 | SError |
| 0x600 | Lower EL, AArch32 | Sync |
| 0x680 | Lower EL, AArch32 | IRQ |
| 0x700 | Lower EL, AArch32 | FIQ |
| 0x780 | Lower EL, AArch32 | SError |

- [ ] **Step 1: Write `hypervisor/arch/arm64/boot/vectors.S`**

```asm
/* SPDX-License-Identifier: TBD */
/*
 * EL2 vector table. M0: every entry funnels into panic_vector.
 * Real handlers arrive in M1 (sync/IRQ split out).
 */

    .extern printk
    .extern cpu_wfi

    .macro VECTOR_ENTRY label
    .align 7                    /* each entry is 128 bytes (1 << 7) */
    b   \label
    .endm

    .section .text.vectors, "ax"
    .align 11                    /* table must be 2048-byte aligned */
    .globl hv_vectors
hv_vectors:
    /* Current EL with SP_EL0 */
    VECTOR_ENTRY panic_vector    /* 0x000 sync */
    VECTOR_ENTRY panic_vector    /* 0x080 irq  */
    VECTOR_ENTRY panic_vector    /* 0x100 fiq  */
    VECTOR_ENTRY panic_vector    /* 0x180 serr */
    /* Current EL with SP_ELx */
    VECTOR_ENTRY panic_vector    /* 0x200 sync */
    VECTOR_ENTRY panic_vector    /* 0x280 irq  */
    VECTOR_ENTRY panic_vector    /* 0x300 fiq  */
    VECTOR_ENTRY panic_vector    /* 0x380 serr */
    /* Lower EL, AArch64 */
    VECTOR_ENTRY panic_vector    /* 0x400 sync */
    VECTOR_ENTRY panic_vector    /* 0x480 irq  */
    VECTOR_ENTRY panic_vector    /* 0x500 fiq  */
    VECTOR_ENTRY panic_vector    /* 0x580 serr */
    /* Lower EL, AArch32 */
    VECTOR_ENTRY panic_vector    /* 0x600 sync */
    VECTOR_ENTRY panic_vector    /* 0x680 irq  */
    VECTOR_ENTRY panic_vector    /* 0x700 fiq  */
    VECTOR_ENTRY panic_vector    /* 0x780 serr */

    .section .text
    .globl panic_vector
panic_vector:
    mrs     x0, esr_el2
    mrs     x1, elr_el2
    /*
     * printk format string lives in .rodata; we load by adrp/add.
     * Format: "!VEC ESR=0x%lx ELR=0x%lx\n"
     */
    adrp    x2, .Lpanic_fmt
    add     x2, x2, :lo12:.Lpanic_fmt
    mov     x3, x0           /* save esr for arg slot */
    mov     x4, x1           /* save elr */
    /* printk(fmt, esr, elr) -- arg order: x0=fmt, x1=esr, x2=elr */
    mov     x0, x2
    mov     x1, x3
    mov     x2, x4
    bl      printk
1:  wfi
    b       1b

    .section .rodata
.Lpanic_fmt:
    .asciz "!VEC ESR=0x%lx ELR=0x%lx\n"
```

- [ ] **Step 2: Assemble-check**

```bash
aarch64-linux-gnu-gcc -ffreestanding -nostdlib -nostartfiles \
  -c -o /tmp/vectors.o hypervisor/arch/arm64/boot/vectors.S
aarch64-linux-gnu-objdump -d /tmp/vectors.o | head -40
rm /tmp/vectors.o
```

Expected: a disassembly showing the `hv_vectors` symbol followed by 16 `b` instructions spaced 0x80 apart. If they are not 0x80 apart, `.align 7` is wrong.

- [ ] **Step 3: Verify alignment**

```bash
aarch64-linux-gnu-gcc -ffreestanding -nostdlib -nostartfiles \
  -c -o /tmp/vectors.o hypervisor/arch/arm64/boot/vectors.S
aarch64-linux-gnu-readelf -s /tmp/vectors.o | grep hv_vectors
aarch64-linux-gnu-objdump -h /tmp/vectors.o | grep '.text.vectors'
rm /tmp/vectors.o
```

Expected: the `.text.vectors` section has alignment `2**11` (= 2048). If it shows `2**4` (16) or similar, the `.align 11` in the section directive failed.

- [ ] **Step 4: Commit**

```bash
git add hypervisor/arch/arm64/boot/vectors.S
git commit -m "feat(arch/arm64): add EL2 panic-stub vector table"
```

---

## Task 9: arch/arm64/boot/head.S — `_start`

**Goal:** AArch64 entry point. Park secondary CPUs, assert EL2, set SP, clear BSS, install vector table, mask DAIF, call `hypervisor_main`.

**Files:**
- Create: `hypervisor/arch/arm64/boot/head.S`

- [ ] **Step 1: Write `hypervisor/arch/arm64/boot/head.S`**

```asm
/* SPDX-License-Identifier: TBD */
/*
 * AArch64 EL2 entry point. Called by QEMU with:
 *   x0 = DTB physical address
 *   PC = 0x40080000 (per linker.lds)
 *   CurrentEL = 0b1000 (EL2) when QEMU is invoked with virtualization=on
 *
 * x19 is callee-saved by AAPCS64, so we use it to preserve the DTB
 * pointer across the memset call.
 */

    .extern hypervisor_main
    .extern hv_vectors
    .extern memset
    .extern __bss_start
    .extern __bss_end
    .extern __stack_top

    /* Early-panic UART base — only place in M0 where a board address
     * is hard-coded in source. Justified by spec §4.6: C is not yet
     * available, so we cannot resolve the board.h macro at this point. */
    .equ EARLY_UART_DR, 0x09000000

    .section .text._start, "ax"
    .globl _start
_start:
    /* Park secondary CPUs. MPIDR_EL1.Aff0 == 0 wins. */
    mrs     x9, mpidr_el1
    and     x9, x9, #0xFF
    cbnz    x9, secondary_park

    /*
     * Assert CurrentEL == 0b1000 (EL2). CurrentEL bits[3:2] hold the EL;
     * the raw register value is EL << 2.
     */
    mrs     x9, CurrentEL
    cmp     x9, #(2 << 2)
    b.ne    panic_early

    /* Save DTB pointer (x0 on entry) in a callee-saved register. */
    mov     x19, x0

    /* Set boot stack. */
    ldr     x9, =__stack_top
    mov     sp, x9

    /* Clear BSS: memset(__bss_start, 0, __bss_end - __bss_start). */
    ldr     x0, =__bss_start
    ldr     x2, =__bss_end
    sub     x2, x2, x0
    mov     x1, #0
    bl      memset

    /* Install EL2 vector table. */
    ldr     x9, =hv_vectors
    msr     vbar_el2, x9
    isb

    /* Mask debug, SError, IRQ, FIQ at EL2. */
    msr     daifset, #0xF
    dsb     sy
    isb

    /* Restore DTB pointer and enter C. */
    mov     x0, x19
    bl      hypervisor_main

    /* hypervisor_main never returns; halt if it does. */
1:  wfi
    b       1b

panic_early:
    /*
     * Emit "!EL\n" directly to PL011 DR. No FIFO check (boot-time, we
     * accept the risk of dropping bytes in exchange for not needing
     * any C or data).
     */
    ldr     x9, =EARLY_UART_DR
    mov     w10, #'!'
    str     w10, [x9]
    mov     w10, #'E'
    str     w10, [x9]
    mov     w10, #'L'
    str     w10, [x9]
    mov     w10, #'\n'
    str     w10, [x9]
2:  wfi
    b       2b

secondary_park:
3:  wfi
    b       3b
```

- [ ] **Step 2: Assemble-check**

```bash
aarch64-linux-gnu-gcc -ffreestanding -nostdlib -nostartfiles \
  -c -o /tmp/head.o hypervisor/arch/arm64/boot/head.S
aarch64-linux-gnu-objdump -d /tmp/head.o | head -60
rm /tmp/head.o
```

Expected: a disassembly with `_start`, `panic_early`, `secondary_park` labels and no assembler errors.

- [ ] **Step 3: Commit**

```bash
git add hypervisor/arch/arm64/boot/head.S
git commit -m "feat(arch/arm64): add _start EL2 entry with early panic path"
```

---

## Task 10: arch/arm64/board/qemu_virt/linker.lds — linker script

**Goal:** Place `.text._start` first at `0x40080000`, lay out other sections, define `__bss_start`/`__bss_end`/`__stack_top`.

**Files:**
- Create: `hypervisor/arch/arm64/board/qemu_virt/linker.lds`

- [ ] **Step 1: Write `hypervisor/arch/arm64/board/qemu_virt/linker.lds`**

```ld
/* SPDX-License-Identifier: TBD */
OUTPUT_FORMAT(elf64-littleaarch64)
OUTPUT_ARCH(aarch64)
ENTRY(_start)

SECTIONS
{
    . = 0x40080000;                /* QEMU virt default kernel load addr */

    .text : ALIGN(4) {
        KEEP(*(.text._start))      /* _start must be first */
        *(.text.vectors)
        *(.text*)
    }

    .rodata : ALIGN(8) {
        *(.rodata*)
    }

    .data : ALIGN(8) {
        *(.data*)
    }

    .bss : ALIGN(16) {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }

    . = ALIGN(16);
    . += 0x4000;                   /* 16 KiB boot stack */
    __stack_top = .;

    /DISCARD/ : {
        *(.note.*)
        *(.comment*)
        *(.eh_frame*)
    }
}
```

- [ ] **Step 2: Sanity-check the script with a dry link**

This step actually requires every object from earlier tasks. Just verify the script parses standalone:

```bash
aarch64-linux-gnu-ld --verbose -T hypervisor/arch/arm64/board/qemu_virt/linker.lds \
    2>&1 | head -5
```

Expected: the linker prints its default script then your script. No parse errors. (You will see "cannot find entry symbol _start" — that is OK at this stage; we have no objects yet. The next task builds them.)

- [ ] **Step 3: Commit**

```bash
git add hypervisor/arch/arm64/board/qemu_virt/linker.lds
git commit -m "feat(arch/arm64): add QEMU virt linker script (load @ 0x40080000)"
```

---

## Task 11: Build system — Makefiles, Kconfig, defconfig, `run-qemu.sh`

**Goal:** `make defconfig && make && make run` works end-to-end.

**Files:**
- Create: `Makefile` (top-level)
- Create: `Kconfig` (top-level)
- Create: `configs/qemu_virt_defconfig`
- Create: `hypervisor/Makefile`
- Create: `hypervisor/Kconfig`
- Create: `hypervisor/arch/arm64/Makefile`
- Create: `hypervisor/arch/arm64/Kconfig`
- Create: `scripts/run-qemu.sh`
- Modify: `.gitignore` (already exists from earlier session) — verify it lists `build/`, `.config`.

- [ ] **Step 1: Verify `.gitignore` is sufficient**

```bash
grep -E '^(build/|\.config)$' .gitignore
```

Expected output:

```
build/
.config
```

If missing, append them and `git add .gitignore` for the final commit.

- [ ] **Step 2: Write `configs/qemu_virt_defconfig`**

```
CONFIG_ARCH_ARM64=y
CONFIG_BOARD_QEMU_VIRT=y
CONFIG_DEBUG_UART=y
```

- [ ] **Step 3: Write top-level `Kconfig`**

```kconfig
mainmenu "Hypervisor configuration"

source "hypervisor/Kconfig"
```

- [ ] **Step 4: Write `hypervisor/Kconfig`**

```kconfig
source "hypervisor/arch/arm64/Kconfig"

config DEBUG_UART
    bool "Early UART output"
    default y
```

- [ ] **Step 5: Write `hypervisor/arch/arm64/Kconfig`**

```kconfig
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
endchoice
```

(These Kconfig files are not parsed by M0's build. They document the menu shape for the future kconfiglib integration. M0 builds purely from `.config` text lines.)

- [ ] **Step 6: Write `scripts/run-qemu.sh`**

```bash
#!/bin/sh
set -eu
exec qemu-system-aarch64 \
  -machine virt,virtualization=on,gic-version=3 \
  -cpu cortex-a72 -smp 1 -m 1G \
  -nographic -serial mon:stdio \
  -kernel build/hypervisor.elf \
  ${QEMU_EXTRA_ARGS:-}
```

Make it executable:

```bash
chmod +x scripts/run-qemu.sh
```

- [ ] **Step 7: Write `hypervisor/arch/arm64/Makefile`**

```make
# SPDX-License-Identifier: TBD
# Arch sub-make: declares per-arch sources, include paths, linker script.

arch-objs := \
    arch/arm64/boot/head.o \
    arch/arm64/boot/vectors.o \
    arch/arm64/cpu/cpu.o \
    arch/arm64/board/$(BOARD)/board.o

arch-includes := \
    -Ihypervisor/arch/arm64/board/$(BOARD) \
    -Ihypervisor/arch/arm64/include

arch-ldscript := hypervisor/arch/arm64/board/$(BOARD)/linker.lds
```

- [ ] **Step 8: Write `hypervisor/Makefile`**

```make
# SPDX-License-Identifier: TBD
# Hypervisor sub-make: declares arch-independent sources.

hv-objs := \
    boot/main.o \
    debug/uart_pl011.o \
    lib/string.o \
    lib/print.o

hv-includes := \
    -Ihypervisor/include
```

- [ ] **Step 9: Write top-level `Makefile`**

```make
# SPDX-License-Identifier: TBD

ARCH          ?= arm64
BOARD         ?= qemu_virt
CROSS_COMPILE ?= aarch64-linux-gnu-

CC      := $(CROSS_COMPILE)gcc
LD      := $(CROSS_COMPILE)ld
OBJCOPY := $(CROSS_COMPILE)objcopy

BUILD_DIR := build
OBJ_DIR   := $(BUILD_DIR)/obj
ELF       := $(BUILD_DIR)/hypervisor.elf
BIN       := $(BUILD_DIR)/hypervisor.bin

CFLAGS := \
    -ffreestanding -nostdlib -nostartfiles \
    -fno-pic -fno-stack-protector \
    -mgeneral-regs-only -mstrict-align \
    -Wall -Wextra -Werror -O2 -g

ASFLAGS := -g

# ----- Pull in sub-makefiles for variable definitions only -----
include hypervisor/Makefile
include hypervisor/arch/$(ARCH)/Makefile

INCLUDES := $(hv-includes) $(arch-includes)

# ----- .config -> -DCONFIG_* flags -----
ifeq ($(wildcard .config),.config)
CONFIG_DEFS := $(shell \
    sed -n 's/^CONFIG_\([A-Za-z0-9_]*\)=y$$/-DCONFIG_\1=1/p' .config)
else
CONFIG_DEFS :=
endif

CFLAGS  += $(CONFIG_DEFS) $(INCLUDES)
ASFLAGS += $(CONFIG_DEFS) $(INCLUDES)

# ----- Object list under $(OBJ_DIR) -----
ALL_OBJS := $(addprefix $(OBJ_DIR)/, $(hv-objs) $(arch-objs))

LD_SCRIPT := $(arch-ldscript)

.PHONY: all run clean defconfig menuconfig help

all: $(ELF) $(BIN)

$(BIN): $(ELF)
	$(OBJCOPY) -O binary $< $@

$(ELF): $(ALL_OBJS) $(LD_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD) -T $(LD_SCRIPT) -o $@ $(ALL_OBJS)

# ----- Pattern rules: hypervisor/<path>.{c,S} -> build/obj/<path>.o -----
$(OBJ_DIR)/%.o: hypervisor/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/%.o: hypervisor/%.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c -o $@ $<

run: $(ELF)
	./scripts/run-qemu.sh

clean:
	rm -rf $(BUILD_DIR)

defconfig:
	cp configs/$(BOARD)_defconfig .config
	@echo "Wrote .config from configs/$(BOARD)_defconfig"

menuconfig:
	@echo "menuconfig is reserved for a later milestone; edit .config by hand for now."
	@false

help:
	@echo "Targets: all run clean defconfig"
	@echo "Vars:    ARCH=$(ARCH) BOARD=$(BOARD) CROSS_COMPILE=$(CROSS_COMPILE)"
```

- [ ] **Step 10: Run defconfig and build**

```bash
make defconfig
make
```

Expected:
- `Wrote .config from configs/qemu_virt_defconfig`
- Compilation lines for each object, no warnings, no errors.
- Final lines link `build/hypervisor.elf` and objcopy to `build/hypervisor.bin`.

If the build fails on missing `__bss_start`/`__bss_end`/`__stack_top`, recheck the linker script (T10) — those must be defined inside `SECTIONS { ... }`.

- [ ] **Step 11: Verify ELF entry and `.text` placement**

```bash
aarch64-linux-gnu-readelf -h build/hypervisor.elf | grep -E 'Entry point|Type'
aarch64-linux-gnu-objdump -h build/hypervisor.elf | grep '\.text'
```

Expected:
- `Entry point address: 0x40080000`
- `.text` section starts at `0000000040080000` (or with leading zeros).

If either is wrong, your linker script's `ENTRY` directive or load address is off — fix T10.

- [ ] **Step 12: Commit**

```bash
git add -A
git commit -m "build: add top-level Makefile/Kconfig + arch/hypervisor sub-makes

Implements 'make defconfig', 'make', 'make run', 'make clean'.
.config is parsed by a one-line sed rule that converts CONFIG_FOO=y
into -DCONFIG_FOO=1. No Kconfig parser dependency."
```

---

## Task 12: End-to-end manual acceptance

**Goal:** Run the spec §6 checklist top to bottom. This is the milestone gate.

**Files:** None modified in this task — only inspection and `make run`.

- [ ] **Step 1: Clean rebuild**

```bash
make clean && make defconfig && make
```

Expected: zero warnings, builds cleanly.

- [ ] **Step 2: Inspect ELF**

```bash
aarch64-linux-gnu-objdump -h build/hypervisor.elf | grep '\.text'
aarch64-linux-gnu-readelf -h build/hypervisor.elf | grep 'Entry point'
```

Expected:
- `.text` at `40080000`
- Entry point `0x40080000`

- [ ] **Step 3: Run and observe banner**

```bash
make run
```

Expected (within ~3 seconds):

```
[hv] Hello from EL2, CurrentEL=0x8
```

Then the hypervisor halts in `wfi` — terminal stays attached, no further output.

Exit QEMU: `Ctrl-A` then `x`.

- [ ] **Step 4: Verify the early-panic path**

Edit `hypervisor/arch/arm64/boot/head.S` and change the EL assertion from EL2 to EL3:

```asm
    cmp     x9, #(3 << 2)        /* was: #(2 << 2) */
```

Rebuild and run:

```bash
make && make run
```

Expected output:

```
!EL
```

Then QEMU hangs (no further output — secondary loop). Exit with `Ctrl-A x`.

- [ ] **Step 5: Revert the EL3 check**

```bash
git checkout hypervisor/arch/arm64/boot/head.S
make
```

Re-run `make run` and confirm the normal banner returns.

- [ ] **Step 6: Tick the acceptance checklist in `README.md`**

Edit `README.md` and replace the `- [ ]` boxes under "Acceptance checklist (M0)" with `- [x]` for the ones you have verified.

- [ ] **Step 7: Final commit**

```bash
git add README.md
git commit -m "docs(readme): tick M0 acceptance checklist after manual verification"
```

- [ ] **Step 8: Tag the milestone**

```bash
git tag -a m0 -m "M0: Hello EL2 milestone complete"
```

---

## Done

After T12 the spec §6 acceptance checklist is satisfied. Next step per spec §9: open a new spec for M1 (Stage-2 + bare-metal guest).
