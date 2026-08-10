# Stage-2 punch-hole for GICD/GICR — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make guest GICD/GICR memory-mapped accesses fault into EL2 so the `vgic_v3_mmio.c` shadow emulation actually fires, instead of passing through to the physical GIC.

**Architecture:** Split `l1_table[0]` (currently a 1 GB Device block covering IPA `0x00000000–0x3FFFFFFF`) into a table descriptor pointing at a new static `l2_dev[512]` table. `l2_dev` is filled with 2 MB Device identity blocks, except the single entry covering GICD+GICR (`0x08000000–0x081FFFFF`, index 64) which is left invalid → guest access faults → MMIO trap-and-emulate → vgic.

**Tech Stack:** C (freestanding, `-mgeneral-regs-only -mstrict-align -Werror`), ARM64 Stage-2 page tables, QEMU `virt` gic-version=3. No unit-test framework — verification is `make` (zero warnings) + a real QEMU boot (CLAUDE.md "Verification" section).

**Spec:** `docs/superpowers/specs/2026-06-21-stage2-gic-punch-hole-design.md`
**Background evidence:** `docs/reference/arm/2026-06-21-gicd-gicr-trap-investigation.md`

---

## File Structure

- **Modify** `hypervisor/arch/arm64/mmu/stage2.c` — the entire change lives here: new `S2_TABLE` macro, new `l2_dev[512]` static table, rewrite of the `l1_table[0]` assignment inside `stage2_init`.
- **Modify** `docs/adr/0012-physical-gicv3-ownership.md` — one sentence pointing at where the punch-hole is implemented (user-confirmed).
- **Verification only (reverted after)** `hypervisor/arch/arm64/irq/vgic_v3_mmio.c` — temporary `[VERIFY]` printk in the two handlers.

`stage2.c` includes only `<types.h> <vm.h> "stage2.h"` today. `BOARD_GIC_DIST_BASE` / `BOARD_GIC_RDIST_BASE` live in `board.h`. Task 1 Step 1 resolves how that symbol is reached (direct include vs. already-transitive) before using it.

---

## Task 1: Add S2_TABLE macro and the l2_dev table, punch entry 64

**Files:**
- Modify: `hypervisor/arch/arm64/mmu/stage2.c` (macros block ~line 23-30; static tables ~line 33; `stage2_init` body ~line 37-39)

- [ ] **Step 1: Confirm how `BOARD_GIC_DIST_BASE` reaches stage2.c**

Run:
```bash
cd /home/corsair/Music/virtual/hyp-/hypervisor-/
grep -rn "BOARD_GIC_DIST_BASE\|BOARD_GIC_RDIST_BASE" hypervisor/arch/arm64/board/qemu_virt/board.h
grep -n "include" hypervisor/arch/arm64/mmu/stage2.c
grep -rn "board.h" hypervisor/include hypervisor/common/vm/vm.h 2>/dev/null
```
Expected: the two macros are defined in `board.h`. `stage2.c` does NOT currently include `board.h`. Decision: add `#include <board.h>` to stage2.c (the `qemu_virt` board dir is on the `-I` path per the Makefile compile line). If grep shows `vm.h` already pulls in `board.h` transitively, skip the explicit include.

- [ ] **Step 2: Add the `#include <board.h>` (only if Step 1 said it's needed)**

In `hypervisor/arch/arm64/mmu/stage2.c`, change the include block:
```c
/* SPDX-License-Identifier: TBD */
#include <types.h>
#include <vm.h>
#include <board.h>
#include "stage2.h"
```

- [ ] **Step 3: Add the `S2_TABLE` macro**

After the `S2_XN` line (currently line 30), add:
```c
#define S2_XN           (1ULL   << 54)  /* Execute-never */
#define S2_TABLE        0x3ULL          /* Table descriptor: bits[1:0]=0b11 (vs block 0b01) */
```

- [ ] **Step 4: Add the static `l2_dev` table**

After the `l1_table` declaration (currently line 33), add:
```c
/* Must be 4 KB-aligned: VTTBR_EL2[11:0] are reserved and must be zero. */
static u64 l1_table[512] __attribute__((aligned(4096)));

/*
 * L2 table backing l1_table[0] (IPA 0x00000000–0x3FFFFFFF). 512 × 2 MB blocks,
 * identity Device-nGnRE, EXCEPT the one entry covering GICD (0x08000000) +
 * GICR (0x080A0000): left invalid so guest accesses fault → vgic_v3_mmio shadow.
 */
static u64 l2_dev[512] __attribute__((aligned(4096)));
```

- [ ] **Step 5: Rewrite the `l1_table[0]` assignment in `stage2_init`**

Replace the current `l1_table[0]` block (lines 37-39):
```c
    /* IPA 0x00000000–0x3FFFFFFF → PA identity: Device (covers PL011 @ 0x09000000) */
    l1_table[0] = 0x00000000UL |
                  S2_BLOCK | S2_MEMATTR_DEV | S2_S2AP_RW | S2_SH_OSH | S2_AF | S2_XN;
```
with:
```c
    /*
     * IPA 0x00000000–0x3FFFFFFF: split the old 1 GB Device block into an L2
     * table so GICD/GICR can be punched out. Each L2 entry is 2 MB; fill all
     * as identity Device-nGnRE (covers PL011 @ 0x09000000), then invalidate the
     * single 2 MB entry holding GICD+GICR → guest access faults → MMIO trap →
     * vgic_v3_mmio shadow emulation (ADR-0012).
     */
    for (u32 i = 0; i < 512U; i++)
        l2_dev[i] = ((u64)i << 21) |
                    S2_BLOCK | S2_MEMATTR_DEV | S2_S2AP_RW | S2_SH_OSH | S2_AF | S2_XN;

    u32 gic_l2_idx = (u32)(BOARD_GIC_DIST_BASE >> 21);
    if ((u32)(BOARD_GIC_RDIST_BASE >> 21) != gic_l2_idx)
        printk("[hv] stage2: WARN GICD/GICR span >1 L2 entry "
               "(D=%u R=%u); punch-hole only covers D's entry\n",
               gic_l2_idx, (u32)(BOARD_GIC_RDIST_BASE >> 21));
    l2_dev[gic_l2_idx] = 0;   /* invalid → fault */

    l1_table[0] = (u64)(uintptr_t)l2_dev | S2_TABLE;
```

Note: `printk` — verify it's already visible in stage2.c (`<vm.h>` or `<types.h>`). If not, add `#include <printk.h>` alongside the other includes (vgic_v3_mmio.c uses `<printk.h>`).

- [ ] **Step 6: Build, zero warnings**

Run:
```bash
cd /home/corsair/Music/virtual/hyp-/hypervisor-/
export PATH="/home/corsair/Downloads/toolchain/arm-gnu-toolchain-14.2.rel1-x86_64-aarch64-none-linux-gnu/bin:$PATH"
make clean >/dev/null 2>&1 && make 2>&1 | tail -5
```
Expected: ends with `objcopy -O binary build/hypervisor.elf build/hypervisor.bin`, no `warning:`/`error:` lines. (`-Werror` is on, so any warning fails the build.)

- [ ] **Step 7: Static check — entry point + symbols intact**

Run:
```bash
export PATH="/home/corsair/Downloads/toolchain/arm-gnu-toolchain-14.2.rel1-x86_64-aarch64-none-linux-gnu/bin:$PATH"
aarch64-none-linux-gnu-readelf -h build/hypervisor.elf | grep Entry
aarch64-none-linux-gnu-nm build/hypervisor.elf | grep -E "l2_dev|stage2_init"
```
Expected: `Entry point address: 0x40080000`; `stage2_init` present; `l2_dev` present as a symbol.

- [ ] **Step 8: Commit**

```bash
cd /home/corsair/Music/virtual/hyp-/hypervisor-/
git add hypervisor/arch/arm64/mmu/stage2.c
git commit -m "feat(stage2): punch-hole GICD/GICR so vGICv3 emulation fires

Split l1_table[0] (1GB Device block) into an L2 table and mark the one
2MB entry covering GICD(0x08000000)+GICR(0x080A0000) invalid. Guest
accesses now fault into EL2 → mmio_bus_lookup → vgic_v3_mmio shadow,
instead of passing through to the physical GIC. PL011 (0x09000000, a
different 2MB entry) stays identity-mapped Device passthrough.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 2: Verify with a real boot — confirm vgic handlers now fire

**Files:**
- Temporarily modify (revert after): `hypervisor/arch/arm64/irq/vgic_v3_mmio.c`

- [ ] **Step 1: Add one-shot `[VERIFY]` printk to `vgicd_mmio_handler`**

In `hypervisor/arch/arm64/irq/vgic_v3_mmio.c`, at the top of `vgicd_mmio_handler` (right after `(void)ctx;`), add:
```c
    {
        static int once;
        if (!once) { once = 1;
            printk("[VERIFY] vgicd_mmio_handler HIT off=0x%lx wr=%d\n",
                   (unsigned long)acc->offset, (int)acc->is_write); }
    }
```

- [ ] **Step 2: Add the same to `vgicr_mmio_handler`**

At the top of `vgicr_mmio_handler` (right after `(void)ctx;`), add:
```c
    {
        static int once;
        if (!once) { once = 1;
            printk("[VERIFY] vgicr_mmio_handler HIT off=0x%lx wr=%d\n",
                   (unsigned long)acc->offset, (int)acc->is_write); }
    }
```

- [ ] **Step 3: Rebuild**

Run:
```bash
cd /home/corsair/Music/virtual/hyp-/hypervisor-/
export PATH="/home/corsair/Downloads/toolchain/arm-gnu-toolchain-14.2.rel1-x86_64-aarch64-none-linux-gnu/bin:$PATH"
make 2>&1 | tail -3
```
Expected: clean build, ends at objcopy.

- [ ] **Step 4: Boot and capture**

Run:
```bash
cd /home/corsair/Music/virtual/hyp-/hypervisor-/
export PATH="/home/corsair/Downloads/toolchain/arm-gnu-toolchain-14.2.rel1-x86_64-aarch64-none-linux-gnu/bin:$PATH"
export LINUX_IMAGE="/home/corsair/Music/virtual/linux-6.12.93/arch/arm64/boot/Image"
WORK=$(mktemp -d)
timeout 12 ./scripts/run-qemu.sh > "$WORK/boot.log" 2>&1 < /dev/null; echo "exit: $?"
echo "=== [VERIFY] hits ==="; grep -nE "VERIFY" "$WORK/boot.log" || echo "NONE"
echo "=== GIC context ==="; grep -nE "found redistributor|256 SPIs|ttyAMA0.*enabled|vGICv3.*registered" "$WORK/boot.log"
echo "$WORK" > /tmp/punchhole_work
```
Expected (PASS criteria): at least one `[VERIFY] vgicd_mmio_handler HIT` and/or `vgicr_mmio_handler HIT` line appears (previously 0). `found redistributor` still appears — but now answered by the shadow model. Note in the run output: if the guest also progresses further than the pre-change baseline, capture that.

- [ ] **Step 5: Confirm the verdict**

If `[VERIFY]` hit count is 0 → STOP, the punch-hole did not take effect; re-examine `gic_l2_idx` and the `S2_TABLE` descriptor (DFSC in the abort, alignment of `l2_dev`). Do not proceed to revert.
If `[VERIFY]` hits ≥ 1 → punch-hole works; proceed to revert instrumentation.

- [ ] **Step 6: Revert instrumentation, rebuild clean**

Run:
```bash
cd /home/corsair/Music/virtual/hyp-/hypervisor-/
export PATH="/home/corsair/Downloads/toolchain/arm-gnu-toolchain-14.2.rel1-x86_64-aarch64-none-linux-gnu/bin:$PATH"
git checkout hypervisor/arch/arm64/irq/vgic_v3_mmio.c
grep -c VERIFY hypervisor/arch/arm64/irq/vgic_v3_mmio.c   # expect 0
make >/dev/null 2>&1 && echo "rebuilt clean"
```
Expected: `0` VERIFY lines, clean rebuild. (No commit — instrumentation was never committed.)

---

## Task 3: Update ADR-0012 to point at the implementation

**Files:**
- Modify: `docs/adr/0012-physical-gicv3-ownership.md`

- [ ] **Step 1: Locate the GICD/GICR shadow sentence**

Run:
```bash
cd /home/corsair/Music/virtual/hyp-/hypervisor-/
grep -n "Stage-2 data\|影子模型\|vgic_v3_mmio" docs/adr/0012-physical-gicv3-ownership.md
```
Expected: the Decision paragraph (around line 16-17) mentions GICD/GICR MMIO → Stage-2 data abort → `vgic_v3_mmio.c`.

- [ ] **Step 2: Add the pointer sentence**

In the Decision paragraph of `docs/adr/0012-physical-gicv3-ownership.md`, immediately after the sentence ending `…被影子模型 vgic_v3_mmio.c 模拟（ADR-0010），从不落到真实硬件。`, append:
```
该 trap 由 Stage-2 punch-hole 落实：stage2_init 把覆盖 GICD/GICR 的那个 2 MB L2 entry 设为 invalid（见 docs/superpowers/specs/2026-06-21-stage2-gic-punch-hole-design.md 与 docs/reference/arm/2026-06-21-gicd-gicr-trap-investigation.md），否则 l1_table[0] 的 identity Device 直通会让访问命中物理 GIC。
```

- [ ] **Step 3: Commit**

```bash
cd /home/corsair/Music/virtual/hyp-/hypervisor-/
git add docs/adr/0012-physical-gicv3-ownership.md
git commit -m "docs(adr-0012): note GICD/GICR trap is realized by a Stage-2 punch-hole

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Done criteria

- `make` clean (zero warnings), entry point `0x40080000`.
- Real boot shows `[VERIFY] vgicd_mmio_handler HIT` and/or `vgicr_mmio_handler HIT` (was 0 before).
- Instrumentation reverted; tree clean except the two committed changes (stage2.c, ADR-0012).
- ADR-0012 points at the implementation.
