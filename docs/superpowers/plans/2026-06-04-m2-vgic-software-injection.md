# M2 — vGIC Software Injection Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prove the hypervisor can make a virtual interrupt appear in the guest's own EL1 IRQ handler — a guest HVC (`HC_INJECT_TEST`) triggers `vgic_inject_sw`, which writes a pending Group-1 vIRQ into `ICH_LR0_EL2`; on the eret back to EL1 the virtual CPU interface delivers it.

**Architecture:** A new `arch/arm64/irq/vgic.{h,c}` module enables the virtual CPU interface (`ICC_SRE_EL2.Enable=1`, `ICH_HCR_EL2.En=1`) and exposes `vgic_init` / `vgic_inject_sw` / `vgic_save` / `vgic_restore` over per-vCPU state appended to `struct vcpu`. The existing M1 HVC trap (`handle_hvc` in `vmexit.c`) gains one case that calls `vgic_inject_sw`; the existing `el1_sync_handler` eret-back path returns to the guest, where the pending vIRQ is taken to `VBAR_EL1 + 0x280`. **No physical GIC init, no timer, no EL2 IRQ vector, no EL2 DAIF change** — those are M2.5. A new self-contained EL1 test payload (`tests/svm2/`) installs its own vector table, configures its `ICC_*` interface, requests injection, and EOIs the vIRQ it receives.

**Tech Stack:** Freestanding C + AArch64 asm (`-ffreestanding -mgeneral-regs-only -Werror`), GNU make build, QEMU `virt` (`gic-version=3,virtualization=on`) integration test driven by `tests/run_svm2_test.sh`.

---

## Reference: spec

Design spec: `docs/superpowers/specs/2026-06-01-m2-vgic-software-injection-design.md`.

Key facts that shape this plan (verified against the M1 tree):

- **The guest can print directly.** `stage2_init` (`hypervisor/arch/arm64/mmu/stage2.c:37-39`) maps IPA `0x00000000–0x3FFFFFFF` as Device R/W, which covers the PL011 UART at `0x09000000`. So the `[svm] …` lines in the acceptance output are emitted by the guest itself, not relayed via HVC.
- **The M1 register ABI is preserved.** `VCPU_HCR_EL2=0x110`, `VCPU_VTTBR_EL2=0x118` (`hypervisor/include/vm.h:49-50`) stay put; new vGIC fields are *appended*. No assembly references the new fields, so no new `__ASSEMBLER__` offset macros are required — but we add static offset asserts to lock the spec's ABI (`0x120/0x128/0x130`) for M2.5.
- **The eret-back path already exists.** `el1_sync_handler` (`hypervisor/arch/arm64/vmexit/vmexit_asm.S:119-145`) restores guest regs and `eret`s after `handle_exit`. Writing the live `ICH_LR0_EL2` inside the HVC handler means the vIRQ is presented on that eret. No new vector, no asm changes.
- **GICv3 system-register access is virtualized in hardware.** With `ICC_SRE_EL2.Enable=1` and `ICH_HCR_EL2.En=1`, the guest's EL1 `ICC_PMR_EL1` / `ICC_IGRPEN1_EL1` / `ICC_IAR1_EL1` / `ICC_EOIR1_EL1` accesses are redirected to the *virtual* interface (`ICV_*`) without trapping to EL2. The injected interrupt comes from the list register, so **no distributor/redistributor is needed**.

### vGIC register encodings used

| Register | Field | Value/meaning |
|---|---|---|
| `ICC_SRE_EL2` | `SRE`(b0), `Enable`(b3) | `0x9` — EL2 sysreg interface on; EL1 may use `ICC_SRE_EL1` |
| `ICH_HCR_EL2` | `En`(b0) | `1` — virtual CPU interface enabled |
| `ICH_VMCR_EL2` | — | `0` at init (guest sets `VPMR`/`VENG1` itself) |
| `ICH_LR0_EL2` | `State`(b63:62)=`01`, `HW`(b61)=`0`, `Group`(b60)=`1`, `Priority`(b55:48), `vINTID`(b31:0) | Pending, software, Group-1 |

### Toolchain note

`ICH_HCR_EL2`, `ICH_VMCR_EL2`, `ICH_LR0..3_EL2`, `ICC_SRE_EL2`, `ICC_SRE_EL1`, `ICC_PMR_EL1`, `ICC_IGRPEN1_EL1`, `ICC_IAR1_EL1`, `ICC_EOIR1_EL1` are all standard binutils mnemonics (≥2.24). If any older assembler rejects a name, the raw form is `S3_<op0>_<Cn>_<Cm>_<op1>` — e.g. `ICH_LR0_EL2` is `S3_4_C12_C12_0`. The build step (`make`) is the verification gate; the canonical names are expected to work on the project's gcc-10+ toolchain.

## File Structure

- **Create** `hypervisor/arch/arm64/irq/vgic.h` — bit-field constants and the four `vgic_*` prototypes.
- **Create** `hypervisor/arch/arm64/irq/vgic.c` — `vgic_init`, `vgic_inject_sw`, `vgic_save`, `vgic_restore`.
- **Modify** `hypervisor/include/vm.h` — append `ich_hcr_el2` / `ich_vmcr_el2` / `ich_lr[4]` to `struct vcpu`.
- **Modify** `hypervisor/include/hypercall.h` — add `HC_INJECT_TEST`.
- **Modify** `hypervisor/arch/arm64/vmexit/vmexit.c` — `#include <vgic.h>`; add the `HC_INJECT_TEST` case to `handle_hvc`.
- **Modify** `hypervisor/common/vm/vm.c` — `#include <vgic.h>`; `vgic_init` in `vm_init`, `vgic_restore` in `vm_run`.
- **Modify** `hypervisor/arch/arm64/Makefile` — add `arch/arm64/irq/vgic.o` to `arch-objs`, `-Ihypervisor/arch/arm64/irq` to `arch-includes`.
- **Modify** `tests/check_offsets.c` and `tests/check_offsets_target.c` — mirror the new fields + assert offsets.
- **Create** `tests/svm2/svm2_main.c`, `tests/svm2/svm2_vectors.S`, `tests/svm2/svm2.lds` — the M2 EL1 payload.
- **Create** `tests/run_svm2_test.sh` — the M2 integration check.
- **Modify** `Makefile` — `svm2` / `test-qemu-svm2` targets; add `test-qemu-svm2` to `test`.

Verification model (per `CLAUDE.md`): no unit-test framework. `make` must build with **zero warnings**; `make test-qemu-svm2` greps QEMU serial output.

---

## Task 1: Failing end-to-end test — the M2 guest (RED)

Build the self-contained M2 payload and its integration check first. With the hypervisor unchanged, the virtual interface is never enabled and `HC_INJECT_TEST` is unknown, so the guest's `[svm] vIRQ received` line never appears — the test fails. The build stays green.

**Files:**
- Create: `tests/svm2/svm2.lds`
- Create: `tests/svm2/svm2_vectors.S`
- Create: `tests/svm2/svm2_main.c`
- Create: `tests/run_svm2_test.sh`
- Modify: `Makefile`

- [ ] **Step 1: Create the guest link script**

Create `tests/svm2/svm2.lds` (loads at the SVM entry IPA; `_start` forced first so it lands at `0x40200000`):

```
OUTPUT_FORMAT("elf64-littleaarch64")
ENTRY(_start)
SECTIONS {
    . = 0x40200000;
    .text : {
        KEEP(*(.text.start))
        *(.text*)
    }
    .data : { *(.data*) }
    .bss  : { *(.bss*)  }
}
```

- [ ] **Step 2: Create the guest EL1 vector table**

Create `tests/svm2/svm2_vectors.S`. Only the "Current EL with SP_ELx" IRQ slot (offset `0x280`) is live — the guest runs EL1h (SP_EL1). The IRQ entry saves caller-saved state + the return context, calls the C handler, and `eret`s so `_start` resumes:

```asm
/* SPDX-License-Identifier: TBD */
/* EL1 vector table for the M2 SVM guest. The guest runs at EL1h (SP_EL1),
 * so a virtual IRQ taken while running enters at offset 0x280 (Current EL
 * with SP_ELx, IRQ). Every other vector parks. */

    .macro VEC label
    .align 7                    /* 128-byte (0x80) vector stride */
    b   \label
    .endm

    .section .text.vectors, "ax"
    .align 11                   /* VBAR_EL1 requires 2048-byte alignment */
    .globl svm2_vectors
svm2_vectors:
    VEC svm2_park       /* 0x000 Cur EL SP0  Sync */
    VEC svm2_park       /* 0x080 Cur EL SP0  IRQ  */
    VEC svm2_park       /* 0x100 Cur EL SP0  FIQ  */
    VEC svm2_park       /* 0x180 Cur EL SP0  SErr */
    VEC svm2_park       /* 0x200 Cur EL SPx  Sync */
    VEC svm2_irq_entry  /* 0x280 Cur EL SPx  IRQ  */
    VEC svm2_park       /* 0x300 Cur EL SPx  FIQ  */
    VEC svm2_park       /* 0x380 Cur EL SPx  SErr */
    VEC svm2_park       /* 0x400 Lower A64   Sync */
    VEC svm2_park       /* 0x480 Lower A64   IRQ  */
    VEC svm2_park       /* 0x500 Lower A64   FIQ  */
    VEC svm2_park       /* 0x580 Lower A64   SErr */
    VEC svm2_park       /* 0x600 Lower A32   Sync */
    VEC svm2_park       /* 0x680 Lower A32   IRQ  */
    VEC svm2_park       /* 0x700 Lower A32   FIQ  */
    VEC svm2_park       /* 0x780 Lower A32   SErr */

    .section .text
svm2_park:
    b   svm2_park

    .globl svm2_irq_entry
svm2_irq_entry:
    /* Frame: x0..x18, x30, ELR_EL1, SPSR_EL1 — 11 pairs = 176 bytes (16-aligned).
     * x19..x28 are preserved by the AAPCS C handler, so we need not save them. */
    stp     x0,  x1,  [sp, #-176]!
    stp     x2,  x3,  [sp, #0x10]
    stp     x4,  x5,  [sp, #0x20]
    stp     x6,  x7,  [sp, #0x30]
    stp     x8,  x9,  [sp, #0x40]
    stp     x10, x11, [sp, #0x50]
    stp     x12, x13, [sp, #0x60]
    stp     x14, x15, [sp, #0x70]
    stp     x16, x17, [sp, #0x80]
    stp     x18, x30, [sp, #0x90]
    mrs     x0, elr_el1
    mrs     x1, spsr_el1
    stp     x0,  x1,  [sp, #0xA0]

    bl      svm2_irq_handler

    ldp     x0,  x1,  [sp, #0xA0]
    msr     elr_el1,  x0
    msr     spsr_el1, x1
    ldp     x2,  x3,  [sp, #0x10]
    ldp     x4,  x5,  [sp, #0x20]
    ldp     x6,  x7,  [sp, #0x30]
    ldp     x8,  x9,  [sp, #0x40]
    ldp     x10, x11, [sp, #0x50]
    ldp     x12, x13, [sp, #0x60]
    ldp     x14, x15, [sp, #0x70]
    ldp     x16, x17, [sp, #0x80]
    ldp     x18, x30, [sp, #0x90]
    ldp     x0,  x1,  [sp], #176
    eret
```

- [ ] **Step 3: Create the guest main**

Create `tests/svm2/svm2_main.c`. It prints via the PL011 directly (functions named `uart_*` to avoid clashing with gcc's `puts`/`putc` builtins under `-Werror`), configures its `ICC_*` interface, requests injection, then prints `done`:

```c
/* SPDX-License-Identifier: TBD */
/*
 * M2 SVM guest: proves vGIC software injection.
 *
 * Loaded by QEMU at IPA 0x40200000 via:
 *   -device loader,file=svm2.bin,addr=0x40200000
 *
 * Flow: install VBAR_EL1, enable the ICC_* interface, unmask PSTATE.I,
 * HVC HC_INJECT_TEST (x1=vINTID). The hypervisor writes a pending vIRQ into
 * ICH_LR0_EL2; on the eret back from the HVC the virtual CPU interface
 * delivers it to svm2_irq_handler, which EOIs it. Then HVC HC_GUEST_DONE.
 *
 * Build (see Makefile target `svm2`):
 *   ${CROSS}gcc -ffreestanding -nostdlib -nostartfiles -O2 -Werror \
 *       -T tests/svm2/svm2.lds -o build/svm2/svm2.elf \
 *       tests/svm2/svm2_main.c tests/svm2/svm2_vectors.S
 *   ${CROSS}objcopy -O binary build/svm2/svm2.elf build/svm2/svm2.bin
 */

#define UART_BASE     0x09000000UL
#define UART_DR       0x00U
#define UART_FR       0x18U
#define UART_FR_TXFF  (1U << 5)

#define HC_INJECT_TEST  0x80000002UL
#define HC_GUEST_DONE   0x80000001UL
#define TEST_VINTID     32U

static void uart_putc(char c)
{
    volatile unsigned int *fr = (volatile unsigned int *)(UART_BASE + UART_FR);
    volatile unsigned int *dr = (volatile unsigned int *)(UART_BASE + UART_DR);
    while (*fr & UART_FR_TXFF)
        ;
    *dr = (unsigned int)(unsigned char)c;
}

static void uart_puts(const char *s)
{
    while (*s)
        uart_putc(*s++);
}

/* Called from svm2_vectors.S (svm2_irq_entry) on a virtual IRQ. */
void svm2_irq_handler(void)
{
    unsigned long iar;
    unsigned long intid;

    __asm__ volatile("mrs %0, ICC_IAR1_EL1" : "=r"(iar));
    intid = iar & 0xFFFFFFUL;

    if (intid == TEST_VINTID)
        uart_puts("[svm] vIRQ received (INTID=32)\n");
    else
        uart_puts("[svm] vIRQ received (unexpected INTID)\n");

    __asm__ volatile("msr ICC_EOIR1_EL1, %0" :: "r"(iar));
    __asm__ volatile("isb");
}

void __attribute__((section(".text.start"))) _start(void)
{
    extern char svm2_vectors[];

    uart_puts("[svm] EL1 init\n");

    /* Point EL1 exceptions at our own vector table. */
    __asm__ volatile("msr vbar_el1, %0" :: "r"(svm2_vectors));
    __asm__ volatile("isb");

    /* Enable the EL1 ICC_* system-register interface and Group-1 IRQs.
     * These succeed only once the hypervisor sets ICC_SRE_EL2.Enable=1. */
    __asm__ volatile("msr ICC_SRE_EL1, %0"     :: "r"(7UL));   /* SRE|DFB|DIB */
    __asm__ volatile("isb");
    __asm__ volatile("msr ICC_PMR_EL1, %0"     :: "r"(0xFFUL)); /* allow all prios */
    __asm__ volatile("msr ICC_IGRPEN1_EL1, %0" :: "r"(1UL));
    __asm__ volatile("isb");
    uart_puts("[svm] vGIC EL1 configured\n");

    /* Unmask PSTATE.I so the pending virtual IRQ is taken on return. */
    __asm__ volatile("msr daifclr, #2");

    uart_puts("[svm] requesting injection (vINTID=32)\n");
    {
        register unsigned long r0 __asm__("x0") = HC_INJECT_TEST;
        register unsigned long r1 __asm__("x1") = TEST_VINTID;
        __asm__ volatile("hvc #0" : "+r"(r0) : "r"(r1) : "memory");
    }
    /* The injected vIRQ is delivered here, on return from the inject HVC,
     * before the next statement runs. */

    uart_puts("[svm] signalling HVC done\n");
    {
        register unsigned long r0 __asm__("x0") = HC_GUEST_DONE;
        __asm__ volatile("hvc #0" : "+r"(r0) :: "memory");
    }

    for (;;)
        __asm__ volatile("wfi");
}
```

- [ ] **Step 4: Create the integration check**

Create `tests/run_svm2_test.sh` (mirrors `tests/run_svm_test.sh`, with the M2 sequence):

```sh
#!/bin/sh
# SPDX-License-Identifier: TBD
# Integration test: run hypervisor + M2 SVM in QEMU, verify vGIC injection.
# Called by: make test-qemu-svm2 (after make all svm2)
# Requires: SVM_BIN set by Makefile, qemu-system-aarch64 in PATH.
set -eu

: "${SVM_BIN:?must be set by make test-qemu-svm2}"

TIMEOUT=10

OUTPUT=$(SVM_BIN="${SVM_BIN}" \
         timeout "${TIMEOUT}" ./scripts/run-qemu.sh </dev/null 2>&1 || true)

FAILURES=0
check() {
    if echo "${OUTPUT}" | grep -qF "$1"; then
        printf "PASS: '%s'\n" "$1"
    else
        printf "FAIL: '%s' not found in output\n" "$1"
        FAILURES=$((FAILURES + 1))
    fi
}

check "Hello from EL2"
check "SVM: launching VMID="
check "[svm] EL1 init"
check "[svm] vGIC EL1 configured"
check "[svm] requesting injection (vINTID=32)"
check "[svm] vIRQ received (INTID=32)"
check "[svm] signalling HVC done"
check "SVM HVC: done"

if [ "${FAILURES}" -gt 0 ]; then
    printf "\n--- QEMU output ---\n%s\n---\n" "${OUTPUT}"
    exit 1
fi

printf "ALL PASS\n"
```

- [ ] **Step 5: Add the `svm2` build and test targets to the top Makefile**

In `Makefile`, after the existing `SVM_BIN := $(BUILD_DIR)/svm/svm.bin` line (around line 52), add the svm2 artifacts:

```make
SVM2_ELF   := $(BUILD_DIR)/svm2/svm2.elf
SVM2_BIN   := $(BUILD_DIR)/svm2/svm2.bin
```

In the `.PHONY` line (around line 56), append `svm2 test-qemu-svm2`:

```make
.PHONY: all run clean defconfig menuconfig help svm svm2 check-offsets test-qemu test-qemu-svm2 test
```

After the existing `svm: $(SVM_BIN)` target, add the svm2 build rules and test target:

```make
$(SVM2_ELF): tests/svm2/svm2_main.c tests/svm2/svm2_vectors.S tests/svm2/svm2.lds
	@mkdir -p $(dir $@)
	$(CC) $(SVM_CFLAGS) -T tests/svm2/svm2.lds -o $@ \
	      tests/svm2/svm2_main.c tests/svm2/svm2_vectors.S

$(SVM2_BIN): $(SVM2_ELF)
	$(OBJCOPY) -O binary $< $@

svm2: $(SVM2_BIN)

test-qemu-svm2: all svm2
	SVM_BIN=$(SVM2_BIN) sh tests/run_svm2_test.sh
```

Finally extend the aggregate `test` target (currently `test: check-offsets check-offsets-target test-qemu`) to also run the M2 test:

```make
test: check-offsets check-offsets-target test-qemu test-qemu-svm2
```

- [ ] **Step 6: Make the runner executable**

Run: `chmod +x tests/run_svm2_test.sh`
Expected: no output (success).

- [ ] **Step 7: Run the M2 integration test to verify it FAILS**

Run: `make test-qemu-svm2`

Expected: the hypervisor and `svm2.bin` both **build** with zero warnings, but the run FAILS. You will see `FAIL: '[svm] vIRQ received (INTID=32)' not found in output`. In the dumped QEMU output the guest reaches `[svm] EL1 init`, then — because the hypervisor has not yet set `ICC_SRE_EL2.Enable=1` — the guest's `ICC_SRE_EL1` write traps to EL2 and hits the M1 default handler, printing `[hv] unexpected exit EC=0x18 …` and parking. The `Hello from EL2`, `SVM: launching VMID=`, and `[svm] EL1 init` checks PASS; the vGIC-config / injection / done checks FAIL. This is the expected red.

- [ ] **Step 8: Commit the failing test**

```bash
git add tests/svm2/svm2.lds tests/svm2/svm2_vectors.S tests/svm2/svm2_main.c \
        tests/run_svm2_test.sh Makefile
git commit -m "test(m2): M2 SVM guest requests vGIC injection, asserts vIRQ received (red)"
```

---

## Task 2: Extend `struct vcpu` with vGIC state (ABI)

Append the vGIC fields the spec specifies (§4.3), keeping the M1 offsets fixed, and lock the new offsets with the existing offset-regression guards. No behavior change.

**Files:**
- Modify: `hypervisor/include/vm.h:16-20`
- Modify: `tests/check_offsets.c:26-30,49-57`
- Modify: `tests/check_offsets_target.c`

- [ ] **Step 1: Append the vGIC fields to `struct vcpu`**

In `hypervisor/include/vm.h`, replace the `struct vcpu` definition (lines 16-20):

```c
struct vcpu {
    struct vcpu_regs regs;   /* MUST be first */
    u64 hcr_el2;             /* offset 0x110 */
    u64 vttbr_el2;           /* offset 0x118 */
};
```

with:

```c
struct vcpu {
    struct vcpu_regs regs;   /* MUST be first */
    u64 hcr_el2;             /* offset 0x110 */
    u64 vttbr_el2;           /* offset 0x118 */
    u64 ich_hcr_el2;         /* offset 0x120 */
    u64 ich_vmcr_el2;        /* offset 0x128 */
    u64 ich_lr[4];           /* offset 0x130 (LR0..LR3, 0x130..0x14F) */
};
```

(No `__ASSEMBLER__` macros are added: vmexit_asm.S never touches these fields. The offset asserts below pin the layout for M2.5, which *will* add asm save/restore.)

- [ ] **Step 2: Mirror the new fields in the host offset check**

In `tests/check_offsets.c`, replace the mirror `struct vcpu` (lines 26-30):

```c
struct vcpu {
    struct vcpu_regs regs;   /* MUST be first */
    u64 hcr_el2;
    u64 vttbr_el2;
};
```

with:

```c
struct vcpu {
    struct vcpu_regs regs;   /* MUST be first */
    u64 hcr_el2;
    u64 vttbr_el2;
    u64 ich_hcr_el2;
    u64 ich_vmcr_el2;
    u64 ich_lr[4];
};
```

Then, after the existing `VCPU_VTTBR_EL2` assert (line 54), add:

```c
_Static_assert(offsetof(struct vcpu, ich_hcr_el2)  == 0x120, "ich_hcr_el2 offset mismatch");
_Static_assert(offsetof(struct vcpu, ich_vmcr_el2) == 0x128, "ich_vmcr_el2 offset mismatch");
_Static_assert(offsetof(struct vcpu, ich_lr)       == 0x130, "ich_lr offset mismatch");
```

- [ ] **Step 3: Add the same offset asserts to the cross-compiled check**

`tests/check_offsets_target.c` includes the real `<vm.h>`, so its struct is already up to date — only add the asserts. After its existing `VCPU_VTTBR_EL2` assert, add:

```c
_Static_assert(offsetof(struct vcpu, ich_hcr_el2)  == 0x120, "ich_hcr_el2 offset mismatch");
_Static_assert(offsetof(struct vcpu, ich_vmcr_el2) == 0x128, "ich_vmcr_el2 offset mismatch");
_Static_assert(offsetof(struct vcpu, ich_lr)       == 0x130, "ich_lr offset mismatch");
```

- [ ] **Step 4: Verify both offset guards pass**

Run: `make check-offsets`
Expected: `PASS: all struct offsets match assembly macros`

Run: `make check-offsets-target`
Expected: `PASS: cross-compiled struct offsets match assembly macros`

(Both compile only if every `_Static_assert` holds, including the three new ones at `0x120/0x128/0x130`.)

- [ ] **Step 5: Verify the hypervisor still builds clean**

Run: `make`
Expected: links `build/hypervisor.elf` with zero warnings (the appended fields are unused so far; that's fine).

- [ ] **Step 6: Commit**

```bash
git add hypervisor/include/vm.h tests/check_offsets.c tests/check_offsets_target.c
git commit -m "feat(m2): extend struct vcpu with vGIC state (ich_hcr/vmcr/lr), lock offsets"
```

---

## Task 3: The vGIC module

Add the module that enables the virtual CPU interface and injects/saves/restores virtual interrupts. It is not yet wired into any caller — this task only adds the code and the build entry, so `make` links cleanly and `build/obj/arch/arm64/irq/vgic.o` exists (acceptance §6.1).

**Files:**
- Create: `hypervisor/arch/arm64/irq/vgic.h`
- Create: `hypervisor/arch/arm64/irq/vgic.c`
- Modify: `hypervisor/arch/arm64/Makefile`

- [ ] **Step 1: Create the vGIC header**

Create `hypervisor/arch/arm64/irq/vgic.h`:

```c
/* SPDX-License-Identifier: TBD */
#ifndef HV_VGIC_H
#define HV_VGIC_H

#include <vm.h>   /* struct vcpu */

/* ICC_SRE_EL2: enable the EL2 system-register interface and permit EL1 to
 * use ICC_SRE_EL1. */
#define ICC_SRE_EL2_SRE     (1ULL << 0)
#define ICC_SRE_EL2_ENABLE  (1ULL << 3)

/* ICH_HCR_EL2: virtual CPU interface enable. */
#define ICH_HCR_EL2_EN      (1ULL << 0)

/* ICH_LR<n>_EL2 fields (GICv3, 64-bit list register). */
#define ICH_LR_STATE_PENDING (1ULL << 62)   /* State[63:62] = 0b01 (Pending) */
#define ICH_LR_HW            (1ULL << 61)    /* 0 = software injection      */
#define ICH_LR_GROUP1        (1ULL << 60)    /* Group 1                     */
#define ICH_LR_PRIO_SHIFT    48              /* Priority[55:48]             */
#define ICH_LR_VINTID_MASK   0xFFFFFFFFULL   /* vINTID[31:0]                */

/* Enable the virtual CPU interface and blank per-vCPU vGIC state. */
void vgic_init(struct vcpu *vcpu);

/* Inject a pending, Group-1, software (HW=0) virtual interrupt via ICH_LR0.
 * Writes the live register so the vIRQ is presented on the next eret to EL1. */
void vgic_inject_sw(struct vcpu *vcpu, u32 vintid, u8 prio);

/* Save/restore the virtual interface state to/from struct vcpu. */
void vgic_save(struct vcpu *vcpu);
void vgic_restore(struct vcpu *vcpu);

#endif /* HV_VGIC_H */
```

- [ ] **Step 2: Create the vGIC implementation**

Create `hypervisor/arch/arm64/irq/vgic.c`:

```c
/* SPDX-License-Identifier: TBD */
#include <types.h>
#include <vm.h>
#include <vgic.h>
#include <asm/sysreg.h>

void vgic_init(struct vcpu *vcpu)
{
    /* One-off (per PE): enable the EL2 sysreg interface and let EL1 use the
     * ICC_* interface. This is the only physical GIC register M2 touches. */
    SYSREG_WRITE(ICC_SRE_EL2, ICC_SRE_EL2_SRE | ICC_SRE_EL2_ENABLE);
    asm volatile("isb");

    /* Per-vCPU virtual interface: enabled, blank VMCR and list registers.
     * The guest programs its own VPMR/VENG1 via ICC_PMR_EL1/ICC_IGRPEN1_EL1. */
    vcpu->ich_hcr_el2  = ICH_HCR_EL2_EN;
    vcpu->ich_vmcr_el2 = 0;
    vcpu->ich_lr[0] = 0;
    vcpu->ich_lr[1] = 0;
    vcpu->ich_lr[2] = 0;
    vcpu->ich_lr[3] = 0;
}

void vgic_inject_sw(struct vcpu *vcpu, u32 vintid, u8 prio)
{
    u64 lr = ICH_LR_STATE_PENDING | ICH_LR_GROUP1 |
             ((u64)prio << ICH_LR_PRIO_SHIFT) |
             ((u64)vintid & ICH_LR_VINTID_MASK);

    vcpu->ich_lr[0] = lr;
    /* Write the live register; the eret back to EL1 synchronises (no isb). */
    SYSREG_WRITE(ICH_LR0_EL2, lr);
}

void vgic_restore(struct vcpu *vcpu)
{
    SYSREG_WRITE(ICH_HCR_EL2,  vcpu->ich_hcr_el2);
    SYSREG_WRITE(ICH_VMCR_EL2, vcpu->ich_vmcr_el2);
    SYSREG_WRITE(ICH_LR0_EL2,  vcpu->ich_lr[0]);
    SYSREG_WRITE(ICH_LR1_EL2,  vcpu->ich_lr[1]);
    SYSREG_WRITE(ICH_LR2_EL2,  vcpu->ich_lr[2]);
    SYSREG_WRITE(ICH_LR3_EL2,  vcpu->ich_lr[3]);
    asm volatile("isb");
}

/* Symmetric save half. M2's single-vCPU flow never reschedules, so this has
 * no caller yet; M2.5's timer context switch is the first user. It is real
 * (non-stub) code kept paired with vgic_restore per spec §4.2. */
void vgic_save(struct vcpu *vcpu)
{
    vcpu->ich_hcr_el2  = SYSREG_READ(ICH_HCR_EL2);
    vcpu->ich_vmcr_el2 = SYSREG_READ(ICH_VMCR_EL2);
    vcpu->ich_lr[0] = SYSREG_READ(ICH_LR0_EL2);
    vcpu->ich_lr[1] = SYSREG_READ(ICH_LR1_EL2);
    vcpu->ich_lr[2] = SYSREG_READ(ICH_LR2_EL2);
    vcpu->ich_lr[3] = SYSREG_READ(ICH_LR3_EL2);
}
```

(`vgic_save` is non-static and declared in the header, so `-Werror`'s `-Wunused-function` does not fire. It is the only deliberate deviation from strict YAGNI in this milestone — see Self-Review note. If you prefer, it may be deferred to M2.5; nothing in M2 calls it.)

- [ ] **Step 3: Wire the module into the arch build**

In `hypervisor/arch/arm64/Makefile`, add the object to `arch-objs` and the include dir to `arch-includes`:

```make
# SPDX-License-Identifier: TBD
arch-objs := \
    arch/arm64/boot/head.o \
    arch/arm64/boot/vectors.o \
    arch/arm64/cpu/cpu.o \
    arch/arm64/board/$(BOARD)/board.o \
    arch/arm64/mmu/stage2.o \
    arch/arm64/vmexit/vmexit.o \
    arch/arm64/vmexit/vmexit_asm.o \
    arch/arm64/irq/vgic.o

arch-includes := \
    -Ihypervisor/arch/arm64/board/$(BOARD) \
    -Ihypervisor/arch/arm64/include \
    -Ihypervisor/arch/arm64/mmu \
    -Ihypervisor/arch/arm64/irq

arch-ldscript := hypervisor/arch/arm64/board/$(BOARD)/linker.lds
```

- [ ] **Step 4: Build and confirm the object exists (acceptance §6.1)**

Run: `make`
Expected: zero warnings; `build/hypervisor.elf` links. (`vgic_*` are defined but uncalled — `-Wunused-function` only flags *static* functions, so the build is clean.)

Run: `ls build/obj/arch/arm64/irq/vgic.o`
Expected: the file exists.

- [ ] **Step 5: Confirm the entry point is unchanged (acceptance §6.1)**

Run: `aarch64-none-linux-gnu-readelf -h build/hypervisor.elf | grep Entry`
Expected: `Entry point address: 0x40080000`

- [ ] **Step 6: Commit**

```bash
git add hypervisor/arch/arm64/irq/vgic.h hypervisor/arch/arm64/irq/vgic.c \
        hypervisor/arch/arm64/Makefile
git commit -m "feat(m2): vGIC module — init/inject_sw/save/restore over ICH_* regs"
```

---

## Task 4: Wire injection end-to-end (GREEN)

Add the `HC_INJECT_TEST` hypercall, route it in `handle_hvc`, and call `vgic_init` / `vgic_restore` from the VM lifecycle. After this task the M2 integration test passes.

**Files:**
- Modify: `hypervisor/include/hypercall.h:7`
- Modify: `hypervisor/arch/arm64/vmexit/vmexit.c:1-32`
- Modify: `hypervisor/common/vm/vm.c:1-43`

- [ ] **Step 1: Define the injection hypercall**

In `hypervisor/include/hypercall.h`, add `HC_INJECT_TEST` after the existing `HC_GUEST_DONE` (line 7):

```c
#define HC_GUEST_DONE        (HVC_VENDOR_BASE | 0x0001U)
#define HC_INJECT_TEST       (HVC_VENDOR_BASE | 0x0002U)
```

- [ ] **Step 2: Route the hypercall to `vgic_inject_sw`**

In `hypervisor/arch/arm64/vmexit/vmexit.c`, add the vGIC include after the existing `#include <psci.h>` (line 6):

```c
#include <vgic.h>
```

Then add an `HC_INJECT_TEST` case to the `switch` in `handle_hvc`, immediately before the existing `case HC_GUEST_DONE:`. The new case injects vINTID `x1` at priority `0xA0` and falls through to the existing eret-back path (`break`, not `hv_restore`):

```c
    case HC_INJECT_TEST:
        vgic_inject_sw(&g_vm.vcpu, (u32)regs->x[1], 0xA0);
        printk("[hv] SVM HVC: inject vINTID=%u\n", (unsigned)regs->x[1]);
        break;
```

(`g_vm` is already visible via `extern struct vm g_vm;` in `<vm.h>`, which `vmexit.c` includes.)

- [ ] **Step 3: Initialise and restore the vGIC in the VM lifecycle**

In `hypervisor/common/vm/vm.c`, add the include after the existing `#include "stage2.h"` (line 6):

```c
#include <vgic.h>
```

In `vm_init`, add the vGIC init right after the `stage2_init(...)` call (line 31), before the launch `printk`:

```c
    stage2_init(&g_vm.vcpu, (u32)svm_config.vmid);

    vgic_init(&g_vm.vcpu);
```

In `vm_run`, add the restore between `stage2_activate` and `vcpu_run` (lines 39-40):

```c
void vm_run(void)
{
    stage2_activate(&g_vm.vcpu);
    vgic_restore(&g_vm.vcpu);
    vcpu_run(&g_vm.vcpu);
    /* Returns here after hv_restore() is called from HVC handler */
}
```

- [ ] **Step 4: Build clean (acceptance §6.1)**

Run: `make`
Expected: zero warnings; `build/hypervisor.elf` links.

- [ ] **Step 5: Run the M2 integration test — verify it PASSES (acceptance §6.2)**

Run: `make test-qemu-svm2`

Expected: `ALL PASS`, with the QEMU output containing, in order:

```
[svm] EL1 init
[svm] vGIC EL1 configured
[svm] requesting injection (vINTID=32)
[svm] vIRQ received (INTID=32)
[svm] signalling HVC done
[hv] SVM HVC: done (x1=0x0)
```

The `[svm] vIRQ received (INTID=32)` line is the diagnostic: the vGIC delivered a virtual interrupt, carrying the injected INTID, to the guest's own EL1 handler.

- [ ] **Step 6: Confirm the M1.5 PSCI test still passes (no regression)**

Run: `make test-qemu`
Expected: `ALL PASS` (the PSCI guest still observes `PSCI_VERSION == 0x10001`; vGIC init does not disturb the PSCI path).

- [ ] **Step 7: Commit**

```bash
git add hypervisor/include/hypercall.h hypervisor/arch/arm64/vmexit/vmexit.c \
        hypervisor/common/vm/vm.c
git commit -m "feat(m2): inject vIRQ on HC_INJECT_TEST; enable vGIC in vm_init/vm_run"
```

---

## Done criteria

- `make` builds with zero warnings; `build/obj/arch/arm64/irq/vgic.o` exists; entry point `0x40080000`. *(spec §6.1)*
- `make test-qemu-svm2` reports `ALL PASS`; the guest's EL1 handler prints `[svm] vIRQ received (INTID=32)`, proving virtual delivery. *(spec §6.2)*
- `make check-offsets` and `make check-offsets-target` pass with the new `0x120/0x128/0x130` asserts; the M1 offsets (`0x110/0x118`) are unchanged. *(spec §4.3)*
- `make test-qemu` (M1.5 PSCI) still passes — no regression.

## Out of scope (do not implement)

- Physical GICv3 init (`GICD`/`GICR`, PPI config), the EL2 physical-IRQ vector (`+0x480`), EL2 `DAIF.I` unmasking, the virtual timer, and `HW=1` hardware-forwarded injection — all **M2.5**. *(spec §2.2)*
- Multiple INTIDs / LRs beyond LR0, distributor MMIO emulation, SMP — **M3**. *(spec §2.2)*
- `vcpu_run` / vector-table changes: M2 deliberately leaves the M1 asm untouched. *(spec §5.2)*

---

## Self-Review

**1. Spec coverage.**
- §2.1 vGIC interface enable (`ICH_HCR_EL2.En=1`, `ICH_VMCR_EL2=0`) → Task 3 `vgic_init`. ✅
- §2.1 `ICC_SRE_EL2.Enable=1`, only GIC register touched → Task 3 `vgic_init` (single `ICC_SRE_EL2` write; no GICD/GICR). ✅
- §2.1 `vgic_inject_sw(vcpu, vintid, prio)` writing Pending/Group-1/HW=0 into `ICH_LR0` → Task 3. ✅
- §2.1 per-vCPU save/restore of `ICH_HCR/VMCR/LR0..3`; `struct vcpu` extended, M1 offsets preserved → Task 2 + Task 3. ✅
- §2.1 `HC_INJECT_TEST` in `handle_hvc` → Task 4. ✅
- §2.1 EL1 payload: installs `VBAR_EL1`, enables `ICC_*`, unmasks `PSTATE.I`, issues `HC_INJECT_TEST`, runs its IRQ handler, EOIs, HVCs done → Task 1 (`svm2_main.c` + `svm2_vectors.S`). ✅
- §4.1 file list (vgic.{h,c}, vm.h, vmexit.c, hypercall.h, vm.c, tests payload) → all covered. **Deviation:** the spec slots the GIC register encodings into `arch/arm64/include/asm/sysreg.h`; this plan keeps them in `vgic.h` instead, because `sysreg.h` holds only the generic `SYSREG_READ/WRITE` accessor macros (which already work with the register *names* via stringization) and the encodings are vGIC-specific. Documented and intentional.
- §6 acceptance output lines → `run_svm2_test.sh` greps each. ✅

**2. Placeholder scan.** No `TBD`/`TODO`/"handle edge cases"/"similar to". Every code step shows complete code. The pre-existing `/* TODO M2: clear HCR_EL2.VM … */` in `vm_run` concerns multi-vCPU scheduling and is out of this milestone's scope; left untouched. ✅

**3. Type/name consistency.** `vgic_init`/`vgic_inject_sw`/`vgic_save`/`vgic_restore`; fields `ich_hcr_el2`/`ich_vmcr_el2`/`ich_lr[4]`; `HC_INJECT_TEST`; `svm2_vectors`/`svm2_irq_entry`/`svm2_irq_handler`; `TEST_VINTID == 32` used consistently in the guest string, the HVC arg, and the handler comparison. The injected priority `0xA0` (Task 4) passes the guest's `ICC_PMR_EL1 = 0xFF` threshold, and Group-1 matches `ICC_IGRPEN1_EL1 = 1` / `ICC_IAR1_EL1` / `ICC_EOIR1_EL1`. ✅

**Intentional YAGNI note.** `vgic_save` is implemented but uncalled in M2 (its first caller is M2.5's context switch). It is included to honor the spec's save/restore pair (§4.2/§2.1) and is real, non-stub code that triggers no `-Werror` warning. If strict minimalism is preferred, deleting `vgic_save` (and its prototype) is a safe, isolated trim with no effect on M2's behavior.
