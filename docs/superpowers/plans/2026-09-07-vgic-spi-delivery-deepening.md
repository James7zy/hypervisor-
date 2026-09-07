# vGIC SPI Delivery Deepening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. The parent owns review/delegation; workers must not launch agents.

**Goal:** Synchronize the existing static PL011 SPI path, establish a guest-observable regression on its old producer structure, then consolidate delivery responsibility inside `vgic_inject_spi()`.

**Architecture:** Retain vuart device semantics and fixed LR0/LR1/LR2 assignments. Independently serialize mutable UART, GIC MMIO and LR1 publication/consumption state; subsequently move placement/publication/kick orchestration behind the existing vGIC interface. One dedicated `vspi` fixture grows vertically across the tasks.

**Tech Stack:** Freestanding AArch64 C/assembly, existing ticket spinlocks, GNU make/toolchain, POSIX shell, QEMU virt/GICv3.

**Spec:** `docs/superpowers/specs/2026-09-07-vgic-spi-delivery-deepening-design.md`; independent prerequisite: `docs/superpowers/specs/2026-09-07-vgic-spi-synchronization-design.md`. Read both, CONTEXT.md, AGENTS.md, ADR-0014, ADR-0010 amendments, ADR-0001 and the M11 design before execution.

## Global Constraints

- Execution baseline `7cc4e5771ce1d655430fa67643225aef5b7cec47`; work/commit on current `master`. No branch/worktree/stash/push/merge/amend or agent-settings changes. Do not edit AGENTS.md or CLAUDE.md.
- Static 1:1 pinning only. LR0 timer, LR1 PL011 software HW=0 Group1 priority `0xA0`, LR2 SGI; no allocator, IRQ queue, scheduler, register mock/adapter or lifecycle policy.
- `vgic_inject_spi(struct vcpu *target, u32 intid)` returns `void`; deepen it, do not add a second producer interface. Only the supported PL011 SPI is promised.
- vuart retains ring/register/mask semantics and `&m->vcpu[0]` target selection. IRQ dispatch retains target-side `vgic_reload_spi_lr(current_vcpu())` and existing timer HW-forwarding/EOIR/DIR.
- Do not change shell/focus behavior or introduce a Linux gate. Self-IPIs do not exercise the physical kick/reload path. No IRQ-per-character assumption.
- `.config` exists; preserve `-mgeneral-regs-only`, `-Werror`, supported printk `%s %c %d %u %x %lx %%`. Header edits require a fresh BUILD_DIR or clean rebuild; Makefile lacks header dependencies.
- Stop on native tooling/setup failure; do not change execution modes. Escalate unresolved source/contract gaps rather than weakening detectors or exclusions.
- Source-level access/ordering argument **and** bounded guest stress are required. No claim of formal race proof from green QEMU runs or nominal M11 S0 completion.
- Mapping exception authorized during planning: correct guest GICR frame -> physical PPI target (`pcpu_base + frame`), as a separately identifiable prerequisite bugfix. No new physical-gate policy or lock is planned; the independent design gives the live-VENG1/init exclusion argument.
- Planning ran no builds/tests. Baseline `make defconfig && make test` was previously reported passing at `/tmp/vgic-spi-baseline-N1euVpfd.log`. Reserve **one full `make test`** for final validation; intermediate checks are targeted.

---

## File map and execution order

| File | Responsibility / task |
|---|---|
| `hypervisor/arch/arm64/irq/vgic_v3_mmio.c` | Task 1a physical target correction; Task 1b all GICD/GICR shadow access locking |
| `hypervisor/dm/vuart.h`, `hypervisor/dm/vuart.c` | Task 1b device lock and snapshot; Task 3 delete producer orchestration |
| `hypervisor/include/vm.h`, `hypervisor/include/spinlock.h` | Task 1b append SPI lock / clarify synchronization ownership |
| `hypervisor/arch/arm64/irq/vgic.c`, `hypervisor/arch/arm64/irq/vgic.h` | Task 1b complete LR1 protocol and init/save/restore contracts; Task 3 deep producer interface |
| `hypervisor/arch/arm64/irq/irq_handler.c`, `hypervisor/arch/arm64/irq/vgic_sgi.h`, `hypervisor/arch/arm64/irq/vgic_sgi.c` | Task 3 stale producer cross-references only, not dispatch/kick behavior |
| `tests/vspi/vspi_main.c`, `tests/vspi/vspi_entry.S`, `tests/vspi/vspi_vectors.S`, `tests/vspi/vspi.lds` | Create in Task 1; guest-visible timer/mapping/stress fixture; Task 2 grows replay stages |
| `tests/run_vspi_test.sh`, `Makefile` | Create/extend Task 1; readiness-driven bounded runner, dual labelled binaries using existing `HV_GUEST=svm_dual`; Task 2 adds full-regression mode and suite integration |
| `docs/superpowers/specs/2026-09-07-vgic-spi-synchronization-design.md` | Update validated source argument/evidence at Task 1 gate; no speculation disguised as proof |
| This plan | Check completed steps and record commit/log evidence as work proceeds |

`svm5` is untouched/reserved for M11. No production `HV_GUEST=vspi` profile is needed: the existing `svm_dual` profile supplies the correct memory/CPU map. No change to `scripts/run-qemu.sh` is needed.

## Shared fixture specification (actual algorithms for Tasks 1–2)

These algorithms are the test implementation contract, not a private hypervisor-state seam. Fixture code must neither include vCPU structs nor read LR shadow/pending fields. Build twice with `-DVSPI_VM_ID=0` and `=1`; do not infer VM ID from identical virtual MPIDRs.

### Files, entry and guest interfaces

`vspi_main.c` defines `void vspi_primary(void)`, `void vspi_secondary(void)`,
`void vspi_irq_handler(void)`, and non-returning `void vspi_exception(void)`.
Only the primary prints. Secondary/IRQ failures store per-core nonzero error
codes for primary reporting; unexpected exceptions store an error then park,
so host timeouts still fail if primary cannot report.

```ld
OUTPUT_FORMAT("elf64-littleaarch64")
ENTRY(_start)
PHDRS { text PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); }
SECTIONS {
    . = 0x40200000;
    .text : { KEEP(*(.text.start)) *(.text*) } :text
    .rodata : { *(.rodata*) } :text
    . = ALIGN(0x1000);
    .data : { *(.data*) } :data
    .bss (NOLOAD) : {
        . = ALIGN(16); __bss_start = .;
        *(.bss*) *(COMMON)
        . = ALIGN(16); vspi_stack0 = .; . += 0x4000;
        vspi_stack1 = .; . += 0x4000;
        __bss_end = .;
    } :data
}
```

`vspi_entry.S` owns `_start` in `.text.start`: mask DAIF, set SP to
`vspi_stack0+0x4000`, clear `[__bss_start,__bss_end)` with an assembly
8-byte STR loop **before calling C**, then branch to `vspi_primary`.
Export `vspi_secondary_entry`: mask DAIF, set SP to `vspi_stack1+0x4000`,
branch to `vspi_secondary`; do not clear shared BSS there. No secondary C
instruction may run on PSCI's uninitialized guest SP_EL1.

Copy `tests/svm3/svm3_vectors.S` to `tests/vspi/vspi_vectors.S`, replace its
`svm3_` symbol prefix with `vspi_`, preserve all 16 vector positions, 2048-byte
alignment, the 176-byte IRQ frame and the AAPCS caller-clobbered/ELR/SPSR saves.
Replace `svm3_park` destinations by a branch to `vspi_exception` (no return).
IRQ entry calls `vspi_irq_handler`. Compile **all** vspi C with
`$(SVM_CFLAGS) -mgeneral-regs-only -mstrict-align -fno-stack-protector`.

Guest-local definitions (no hypervisor include dependency): `u32 = unsigned
int`, `u64 = unsigned long`; 32/64-bit explicit LDR/STR MMIO accessors with memory clobbers (no post-index addressing: the current EL2 decoder requires ISV); `now()` reads
CNTVCT_EL0 with ISB, `freq` reads CNTFRQ_EL0. Use release stores/acquire loads
for guest shared handshakes (`__atomic_store_n`, `__atomic_load_n`), each
counter single-writer; no out-of-line atomic RMW helpers are needed. Every
busy wait checks `now() - start < 5 * freq` and per-core error slots. Never
rely solely on timer IRQs for a timeout (a broken timer must time out too).
The host has its own 180-second whole-run timeout. The initial wait for host
mode selection is the explicit exception to the 5-second phase deadline: use
180*freq, because VM1 waits while VM0 completes its entire experiment. CPU1
also waits up to 180*freq for primary to publish the selected mode; G then
idles, S/R enter batch handshakes. Completed guests idle with timers enabled
without declaring a missing next command to be failure. Guest `uart_puts`/decimal
formatting emit via DR; only VM0 may emit an unsolicited boot line, preventing
cross-VM character interleaving from corrupting markers. BOOT alone retries
until mode to recover finite EL2 startup-message interleaving.

Use named failure codes `EARLY_TIMER=1`, `DEADLINE=2`, `CPU_ON=3`, `RX_DATA=4`,
`GIC_STATE=5`, `UNEXPECTED_IRQ=6`, `UART_REPLAY=7`, `EXCEPTION=8`. Primary emits
`VSPI VMn FAIL code` (decimal code) and never DONE after a failure. A failed
early-timer check latches EARLY_TIMER but continues to UART setup/mode reception
so VM1 can report after focus arrives; it must not claim GATE PASS.

### CPU_ON, interface setup, and early-timer mapping detector

Constants: UART `0x09000000`, GICD `0x08000000`, GICR `0x080A0000`, stride
`0x20000`, SGI frame `+0x10000`; timer INTID27, UART INTID33, test SGI INTID1.
Use these named constants in the fixture.

Each vCPU, with its new stack and VBAR installed, runs this ordered algorithm:

```text
write ICC_SRE_EL1 = 7; ISB; ICC_PMR_EL1 = 0xff
write ICC_CTLR_EL1 = 0                 # virtual combined EOI
write ICC_IGRPEN1_EL1 = 0; ISB
write CNTV_TVAL_EL0 = freq/100; CNTV_CTL_EL0 = 1; ISB
busy-wait CNTVCT for freq/20            # expire while VENG1 is false
# HCR.IMO routes physical IRQs to EL2 despite guest PSTATE.I being masked.
# EL2 software-pends then masks the physical timer PPI.
write own virtual GICR WAKER = 0
write own SGI-frame IGROUPR0 = (1<<27)|(1<<1)
write ICC_IGRPEN1_EL1 = 1; ISB
write own SGI-frame ISENABLER0 = (1<<27)|(1<<1)
DSB SY; ISB; unmask guest IRQ (DAIFClr #2)
require timer_count[cpu] >= 5 within 5*freq counter ticks
```

The handler re-arms every timer tick to `freq/100` and writes ENABLE=1,
ISBs, writes ICC_EOIR1_EL1 with the original IAR (combined drop/deactivate), ISBs, then
publishes completion count. Exactly one initial software-pending tick is
insufficient; VM1 must progress on physical CPU2/3 after the mapping fix.

Primary additionally configures GICD CTLR `0x12`, IGROUPR1 bit1, ISENABLER1
bit1, priority byte for INTID33 `0xA0`, UART CR `0x301`, ICR `0x50`, and IMSC
`0x50`. It calls PSCI CPU_ON using x0=`0xC4000003`, x1=`1`,
x2=`vspi_secondary_entry`, x3=`0`; HVC inline asm declares x0 output and
x1/x2/x3 inputs plus memory clobber. Require return zero and a separate guest
`secondary_ready` release/acquire handshake after CPU1's own timer detector.
PSCI's EL2 `online` is not guest readiness. On failure CPU1 sets its own error
and ready flag, so the primary can report rather than hang without attribution.
No CPU_OFF/SYSTEM_OFF or HC_GUEST_DONE is used.

UART setup precedes the primary timer detector so even a failed detector
retains the host mode byte. VM0 retries `VSPI VM0 BOOT` every quarter second
while waiting for mode, VM1 prints nothing unsolicited. Before sending mode,
the host requires an exact BOOT line and all existing pCPU1/2/3 online
announcements as fixed substrings: transport quiescence only, not guest readiness.
The first byte accepted **only in the UART IRQ** selects `G` (gate-only), `S`
(stress), or, in Task 2, `R` (full regression). Until that byte, retain failures
but print nothing else. Then print `VSPI VMn ACTIVE`, report any latched error
as `VSPI VMn FAIL <code>`, or require both early-timer detectors and print
`VSPI VMn GATE PASS`. For G, print `VSPI VMn DONE` and enter the command-finished
idle loop with timers still running. This lets the host focus VM1 before it
prints, including its deterministic mapping failure report.

### UART IRQ handling and stress data protocol

IRQ handler reads ICC_IAR1_EL1. INTID1023 returns without EOI/DIR; other
unexpected INTIDs record failure. For INTID33, increment `uart_irqs`, clear
ICR `0x50` **before** draining, then loop while FR.RXFE is clear, reading DR
and validating/storing bytes. There is no RX polling in the primary loop.
Before command selection the one byte is the mode; afterwards expected byte
at payload position `i` is `'a' + ((i + VSPI_VM_ID*7) % 26)`.
Reject extra/mismatched bytes. Publish `rx_bytes` after processing each byte.
Finish every real IRQ with EOIR, ISB in combined-EOI mode. Empty IRQs during active input are
allowed; delayed/coalesced injection is not an IRQ-per-byte contract.
Main runs only after its handler returns, so main-issued stage acknowledgements
are necessarily after virtual interrupt completion.

Stress consists of 128 batches × 32 bytes per VM (4096 bytes, multiple ring
wraps). Print `VSPI VMn RX READY b` for `b=0..127`; host sends exactly that
batch in 8 four-byte writes, 2ms apart. Run 32 shared-GIC rounds per batch,
then require `rx_bytes == (b+1)*32`; print `VSPI VMn RX PASS b`. CPU1 never
consumes DR; it concurrently reads FR/RIS/MIS, writes IMSC=`0x50`, and writes
stored IBRD/FBRD/LCR_H/CR/IFLS values (1/0/0x70/0x301/0). It continues these accesses while awaiting the paced RX batch after fast GIC
rounds finish. It does not disable RX or clear RIS during payload transfer. Primary checks the exact sequence
and aggregate count, not UART IRQ count.

Each shared-GIC round uses a reusable two-party barrier: each participant
release-publishes its increasing phase in `arrived[cpu]`, then acquire-waits
for `arrived[other] >= phase`, subject to the counter deadline. A primary
release-published batch number starts CPU1's 32 rounds; do not synchronize
between the two competing writes inside any one round.

```text
repeat 32 rounds per batch on both vCPUs:
    barrier                           # initial round state was cleared
    GICD_ISENABLER2 := 1<<cpu          # shared enabled word, INTID64/65
    GICR0_SGI_ISPENDR0 := 1<<(5+cpu)   # BOTH CPUs address frame 0
    IROUTER for INTID64, half[cpu] := 0x11110000+round (cpu0)
                                             or 0x22220000+round (cpu1)
    barrier
    both require GICD_ISENABLER2 & 3 == 3
    both require GICR0_ISPENDR0 & 0x60 == 0x60
    both 64-bit-read IROUTER64 and require the combined two halves
    barrier                           # neither clears before checks finish
    GICD_ICENABLER2 := 1<<cpu
    GICR0_SGI_ICPENDR0 := 1<<(5+cpu)
    barrier
    both require enabled & 3 == 0 and pending & 0x60 == 0
    barrier                           # no next-round set before checks
```

IROUTER64 is at GICD+`0x6200`; its two 32-bit halves exercise the existing
software RMW path and the 64-bit read fast path. GICR pending bits5/6 are
shadow-only in this implementation, not additional LR2 injections. Initialize
these test bits to zero once before starting CPU1 rounds. Use the monotonically
increasing total round index (0..4095) in both half-patterns.

After all batches, both participants stop GIC rounds by release/acquire
handshake. In a no-input stage with FIFO empty, run 256 synchronized UART
register iterations: CPU1 alternates IMSC=0/0x50, primary reads IMSC/RIS/MIS
and writes ICR=0x50, then barrier; finish IMSC=0x50 and require RXFE, RIS/MIS
RX bits zero. No pending data may be silently masked at the last byte.
Require both timer counts to advance by at least five since stress start,
exactly 4096 bytes and no errors; print `VSPI VMn STRESS PASS`. For S, print
DONE. Task 2 adds R's quiet/IPI stages before DONE.

### Runner algorithm and Makefile interfaces

`tests/run_vspi_test.sh` accepts required `HYPERVISOR_ELF`, `SVM_BIN`,
`SVM_BIN2`, optional `VSPI_MODE=gate|stress|regression` (Task 1 default stress,
Task 2 default regression) and optional `VSPI_LOG`. Use `set -eu` and a
`mktemp -d` directory for a FIFO and default log, never repo-root scratch.
Print the log path and preserve the log on failure. Do not reuse the existing
shell test's `timeout ... || true` success convention.

```sh
mkfifo "$tmp/input"
exec 3<>"$tmp/input"
LINUX_IMAGE= timeout 180 ./scripts/run-qemu.sh <"$tmp/input" >"$log" 2>&1 &
qemu_pid=$!
# Required exported image variables are inherited by run-qemu.sh.
# Trap EXIT/INT/TERM: kill/reap a still-running qemu_pid, close fd3;
# remove the FIFO, preserve/copy log at VSPI_LOG, never conceal exit status.
```

Implement `wait_marker()` as at most 300 iterations of: fail immediately on
`grep -q 'VSPI .* FAIL' "$log"` or dead qemu_pid, succeed on `grep -qF` of
the complete marker line, otherwise sleep `0.05`; exhaustion prints log and
exits 1. All required VM/phase/batch markers must be checked, not just DONE.
Use anchored full-line matches after removing CR when checking numbered
batches (avoid READY 1 matching READY 10). Do not wait for a nonexistent
second unsolicited boot banner.

```text
wait VM0 BOOT and existing pCPU1/2/3 online announcements
for vm in 0,1:
    if vm==1:
        write Ctrl-T (printf '\024' >&3); wait new 'hv> ' prompt
        write 'vm_console 1\r' >&3; wait '[hv] console: VM1'
    write mode byte G/S/R >&3
    wait ACTIVE; wait GATE PASS
    if S or R:
        for b=0..127:
            wait RX READY b
            send eight four-byte chunks using the specified alphabet formula
            wait RX PASS b
        wait STRESS PASS
    if R: require all Task 2 quiet/IPI/replay markers, send NO payload
    wait DONE
write QEMU monitor escape-exit '\001x' >&3
wait qemu_pid; require status 0 (timeout 124 and any other error FAIL)
require no guest FAIL/error markers; print 'VSPI ALL PASS'
```

Only one VM prints solicited stages at a time; CPU1 never prints. Shell
attachment is transport setup using existing policy, not a focus-behavior test.
Gate-only mode can diagnose the mapping defect without requiring stress to pass.

Makefile: add `vspi` target building `$(BUILD_DIR)/vspi/vspi-vm0.elf/.bin` and
`vspi-vm1.elf/.bin` from the four named guest files. A `%` ELF rule sets
`-DVSPI_VM_ID=$*`; depend explicitly on all four inputs and objcopy each ELF.
Add `.PHONY: vspi test-qemu-vspi`. The test target uses its own
`VSPI_BUILD_DIR ?= build/test-vspi`, invokes
`$(MAKE) BUILD_DIR=$(VSPI_BUILD_DIR) HV_GUEST=svm_dual all vspi`, then exports
the three image paths to `sh tests/run_vspi_test.sh`. Task 2 adds
`test-qemu-vspi` to `test` prerequisites. Do not change existing SVM targets.

---

### Task 1: Independent prerequisites, focused mapping red/green and bounded stress

**Files:** Create all four `tests/vspi/` files and `tests/run_vspi_test.sh`;
modify `Makefile`, `vgic_v3_mmio.c`, `vuart.c/.h`, `vm.h`, `spinlock.h`,
`vgic.c/.h` at the exact existing functions/structs named above. Update the
independent synchronization design with measured evidence. No producer
placement/kick relocation in this task.

**Interfaces:** Preserve public `void vgic_set_spi_shadow(struct vcpu *, u32)`
and local-only `void vgic_inject_spi(struct vcpu *, u32)` until Task 3.
Add private `static void vgic_set_spi_shadow_locked(struct vcpu *, u32)`.
Change room check to `bool vuart_rx_has_room(struct vm *m)`; its sole caller
already passes mutable `&vm[console_focus]`. New locks are `vuart.lock`,
`vcpu.spi_lock`, and file-static `vgic_mmio_lock[NR_VMS]`.

- [x] **1. Implement the fixture's G mode and bounded runner first.** Follow
  the entry/CPU_ON/timer/IRQ/mode algorithms above; gate mode sends no payload.
  Add `vspi` and isolated `test-qemu-vspi` targets, not the full suite yet.
- [x] **2. Run the focused gate against unmodified production source.**

```sh
set -o pipefail
B=$(mktemp -d /tmp/vspi-gate-red-XXXXXX)
make BUILD_DIR="$B" HV_GUEST=svm_dual all vspi check-offsets check-offsets-target \
  2>&1 | tee /tmp/vspi-gate-red-build.log
VSPI_MODE=gate VSPI_LOG=/tmp/vspi-gate-red-qemu.log \
HYPERVISOR_ELF="$B/hypervisor.elf" SVM_BIN="$B/vspi/vspi-vm0.bin" \
SVM_BIN2="$B/vspi/vspi-vm1.bin" sh tests/run_vspi_test.sh
```

  Expected nonzero result: VM1 early timer progress fails (one software tick
  does not count as periodic progress); preserve the precise result. If it
  unexpectedly passes, inspect that VENG1 was zero during expiration and
  require the actual multi-tick test; do not infer coverage from silence.
- [x] **3. Fix only the physical frame mapping.** Include `vm_config.h` in
  `vgic_v3_mmio.c`; pass physical index to the existing helper without changing
  the logical shadow/frame index:

```c
/* In vgicr_mmio_handler's SGI write branch: */
vgicr_write_sgi(m->config->pcpu_base + cpu, r, off, (u32)acc->data);
/* Rename vgicr_write_sgi's first parameter to pcpu, and its existing
 * gic_ppi_set_enable(cpu, ...) argument to pcpu. Nothing else changes. */
```

- [x] **4. Re-run G green in a fresh build, then commit this attributable bugfix
  with its focused test.** Use the command above with fresh `/tmp/vspi-gate-green-XXXXXX`
  and green log names. Require GATE PASS/DONE for both VMs and runner status 0.
  Stage only mapping/fixture/Makefile files; commit
  `fix(vgic): rearm timer on the addressed VM physical CPU`.
- [x] **5. Grow the fixture to S mode before race locking.** Implement exactly
  the 128×32 RX and GIC rounds, cross-frame accesses, mask/status phase and
  timer checks above. Run once on the old unlocked path and retain output;
  a pass is possible and is not proof of race absence. The deterministic red
  test in step 2 proves only the mapping detector, not synchronization.
- [x] **6. Implement the minimal device/MMIO locking.** Reuse `spinlock.h`,
  add embedded vuart lock and the BSS-zeroed per-VM MMIO lock array. For UART
  TX special-case DR writes in the MMIO wrapper and return after console output;
  remove the now-unreachable DR output case from the internal write helper.
  All other reads/writes use one lock/unlock wrapper. Lock room checks.

```text
vuart_rx(m,ch):
    lock m.vuart.lock
    preserve existing full-check/push, RIS |= RX_MASK
    notify := (IMSC & RX_MASK) != 0
    unlock
    if notify: execute the EXISTING local/shadow/dsb/kick branch unchanged

vgicd_mmio_handler(acc):
    existing debug output
    lock vgic_mmio_lock[vmid]
    execute either complete 64-bit IROUTER branch or existing normal handler
    unlock exactly once; return 0
vgicr_mmio_handler(acc):
    existing debug output and invalid-frame rejection
    lock vgic_mmio_lock[vmid]
    execute addressed RD/SGI read or write including corrected physical enable
    unlock; return 0
```

  Snapshotting notification outside the vuart lock preserves a linearized mask
  decision; do not add delivery-on-unmask or hold any lock over a kick/print.
- [x] **7. Implement the entire LR1 protocol, not just a pending flag lock.**
  Append `struct spinlock spi_lock` to `struct vcpu`, replace volatile pending
  with ordinary bool, include spinlock definitions. Do not move asm-visible
  fields. Do not reset the BSS lock from `vgic_init()`.

```text
set_spi_shadow_locked(v,id): ich_lr[1] := encode(id); pending := true
set_spi_shadow(v,id): lock; set_spi_shadow_locked; unlock
inject_spi(v,id): lock; set_spi_shadow_locked; live LR1 := ich_lr[1];
                  pending := false; unlock
reload_spi_lr(v): lock; if pending:
                           live LR1 := ich_lr[1]; pending := false
                       unlock
```

  Encoding stays in `vgic_spi_lr_encode()`. Never call the public locking
  setter while holding `spi_lock`. Update header/local comments and
  `vgic_init/save/restore` comments with the current source exclusions argued in
  the synchronization design and rechecked at the prerequisite review gate. Correct the stale lock-free claim in spinlock.h;
  state that callers enter with EL2 IRQs masked. Do not add runtime save callers,
  reset/reinitialize running state, irqsave wrappers, or physical-GICR locks.
- [x] **8. Validate the source-level argument independently of QEMU.**

```sh
git grep -n -E 'spi_shadow_pending|ich_lr\[1\]|vgic_(init|save|restore)\('
git grep -n -E 'vuart_rx_has_room|vuart_read|vuart_write|->(ris|imsc|rx_head|rx_tail)'
git grep -n -E 'g_vgic[dr]|gic_ppi_set_enable|daifclr|spin_lock|spin_unlock' hypervisor
```

  Inventory every hit: UART status/ring/config read or write; 64/32-bit GICD
  paths; cross-vCPU GICR paths; SPI init/save/restore/live writes/pending. Verify
  no hidden writer defeats the IMSC=0/pre-entry exclusion, no lock covers
  console/kick/wait, and hardware W1 operations/live VENG1 ownership satisfy
  the documented ordering. Any failure blocks Task 2/3, not a comment fix.
- [x] **9. Fresh targeted builds, five bounded stress runs, and single-scenario
  regressions.** Save exact build directory and logs in the sync design.

```sh
set -o pipefail
B=$(mktemp -d /tmp/vspi-sync-green-XXXXXX)
make BUILD_DIR="$B" HV_GUEST=svm_dual all vspi svm svm4 check-offsets check-offsets-target \
  2>&1 | tee /tmp/vspi-sync-build.log
for n in 1 2 3 4 5; do
  VSPI_MODE=stress VSPI_LOG="/tmp/vspi-sync-stress-$n.log" \
  HYPERVISOR_ELF="$B/hypervisor.elf" SVM_BIN="$B/vspi/vspi-vm0.bin" \
  SVM_BIN2="$B/vspi/vspi-vm1.bin" sh tests/run_vspi_test.sh || exit 1
done
HYPERVISOR_ELF="$B/hypervisor.elf" SVM_BIN="$B/svm/svm.bin" \
SVM_BIN2="$B/svm4/svm4.bin" sh tests/run_svm4_test.sh \
  2>&1 | tee /tmp/vspi-sync-svm4.log
HYPERVISOR_ELF="$B/hypervisor.elf" SVM_BIN="$B/svm/svm.bin" \
SVM_BIN2="$B/svm4/svm4.bin" sh tests/run_shell_test.sh \
  2>&1 | tee /tmp/vspi-sync-shell.log
```

- [x] **10. Commit and prerequisite review gate.** Record five runs × two VMs
  ×4096 ordered bytes, 4096 shared-GIC rounds/VM/run, periodic progress and
  source audit, with bounded-evidence limitations. Stage only Task 1b files;
  commit `fix(vgic): serialize vuart MMIO and SPI publication state`.
  No continuation to responsibility consolidation until the independent
  prerequisite contract review accepts the complete protocol and evidence.

### Task 2: Positive UART + sibling IPI + timer regression on synchronized old producer

**Files:** Modify `tests/vspi/vspi_main.c`, `tests/run_vspi_test.sh`, `Makefile`.
Production behavior remains exactly Task 1's old vuart placement branch.

**Interfaces:** Add command `R`, retaining G/S modes. Add guest single-writer
`ipi_request`, `ipi_sent`, and `ipi_completed` monotonic counters. CPU1 services
requests only after its stress rounds finish; INTID1 completion is published
by CPU0's IRQ handler after combined EOIR/ISB. No additional hypercall or test-only
hypervisor hook.

- [x] **1. Extend R after STRESS PASS with the actual replay detector.**

```text
after RX==4096 and STRESS PASS, CPU0:
    require FR.RXFE and RX bits of RIS/MIS zero
    observe 10 completed timer ticks without host input, allowing in-flight
      UART IRQs to settle; require both CPUs' timer counts advance
    wait for a final 10-tick interval in which uart_irqs is unchanged
      (restart interval on change; whole settling deadline remains 5*freq)
    # all handler completion is past: execution is now in primary main
    print VSPI VMn DRAINED
    for k=1..16:
        snapshot uart_irqs, rx_bytes, both timer counts, ipi_completed
        publish ipi_request=k (release)
        CPU1 observes request (acquire), writes ICC_SGI1R_EL1=(1<<24)|1
          then ISB, publishes ipi_sent=k; never send a second outstanding SGI
        CPU0 requires ipi_sent==k and ipi_completed==previous+1 by deadline
        print VSPI VMn IPI PASS k
        observe 10 further CPU0 timer completions and CPU1 timer advance
        require uart_irqs unchanged, rx_bytes==4096, FR.RXFE,
          RIS/MIS RX bits zero, no errors, ipi_completed==previous+1
        print VSPI VMn QUIET PASS k
    print VSPI VMn REPLAY PASS
    print VSPI VMn DONE
```

  CPU1's request loop checks deadlines/failure and keeps guest IRQs unmasked;
  idle primary keeps timer active. The SGI targets VM-local CPU0 from sibling
  CPU1 for **both VM0 and VM1**, never self. Do not use broadcast or multiple
  SGI INTIDs (fixed LR2 capacity). Check every IPI/QUIET marker in the runner.
  Fail even an empty UART IRQ after DRAINED: it is the stale-LR replay symptom.
  The initial settling interval does not erase the later observation window.
- [x] **2. Run R on synchronized old production before changing delivery.**

```sh
B=$(mktemp -d /tmp/vspi-old-producer-XXXXXX)
make BUILD_DIR="$B" HV_GUEST=svm_dual all vspi check-offsets check-offsets-target
VSPI_MODE=regression VSPI_LOG=/tmp/vspi-old-producer-regression.log \
HYPERVISOR_ELF="$B/hypervisor.elf" SVM_BIN="$B/vspi/vspi-vm0.bin" \
SVM_BIN2="$B/vspi/vspi-vm1.bin" sh tests/run_vspi_test.sh
```

  All required markers and status 0 are necessary. If it fails, distinguish
  fixture failure from pre-existing behavior; do not proceed or bundle an
  unapproved delivery fix into Task 3.
- [x] **3. Prove detector sensitivity with controlled, uncommitted mutations.**
  Start with clean committed production from Task 1; keep fixture edits intact.
  First temporarily suppress `vgic_inject_spi()` delivery (return without
  publication/live write), build in fresh `/tmp/vspi-mutation-rx-XXXXXX`, run R:
  VM0 must fail to receive the mode/payload or reach required RX markers, and
  runner **must exit nonzero**. Save `/tmp/vspi-mutation-rx.log`. Restore only
  this experimental production file from its committed version and verify diff.
  Second temporarily change reload's `if (pending)` to unconditional LR1 write,
  build fresh `/tmp/vspi-mutation-replay-XXXXXX`, run R: VM0 must report replay
  failure after sibling IPI. Repeat with this mutation conditional on
  `vcpu->owner->id == 1` (normal pending gate for VM0), fresh build/log, so VM1's
  detector must fail too. A mutation that only times out before DRAINED does
  **not** validate replay detection: require the guest replay-failure marker
  after that VM's DRAINED and IPI stage (`VSPI VMn FAIL 7`, the UART_REPLAY
  code). No permanent fault-injection switch.

```sh
# Record actual nonzero status, never turn an unexpected pass into expected failure.
set +e
VSPI_MODE=regression VSPI_LOG="$mutation_log" \
HYPERVISOR_ELF="$mutation_build/hypervisor.elf" \
SVM_BIN="$mutation_build/vspi/vspi-vm0.bin" \
SVM_BIN2="$mutation_build/vspi/vspi-vm1.bin" sh tests/run_vspi_test.sh
rc=$?
set -e
[ "$rc" -ne 0 ] || { echo 'detector failed to reject mutation'; exit 1; }
```

- [x] **4. Restore experiments; rerun R green in a new build.** Confirm
  `git diff -- hypervisor/` is empty before fixture commit, then freshly rebuild
  and obtain `/tmp/vspi-old-producer-restored.log`. Never reuse mutation objects.
- [x] **5. Integrate and commit regression.** Add `test-qemu-vspi` to `test`
  prerequisites and update the stale Makefile comment claiming shell is the
  only serial-input test. Do not execute full `make test` yet. Commit only
  fixture/runner/Makefile and evidence docs:
  `test(vgic): detect UART replay after sibling IPIs in both VMs`.
  Record old-producer green plus RX/VM0-replay/VM1-replay mutation failures and
  restored-green log paths. Gate this regression before Task 3.

### Task 3: Consolidate SPI responsibilities without changing synchronized behavior

**Files:** Modify `hypervisor/dm/vuart.c`, `hypervisor/arch/arm64/irq/vgic.c`,
`vgic.h`, and necessary producer cross-references in `irq_handler.c`,
`vgic_sgi.h`, `vgic_sgi.c`. Tests stay unchanged.

**Interfaces:** Public producer remains `void vgic_inject_spi(struct vcpu *target,
u32 intid)`; private locked helper remains internal; remove public
`vgic_set_spi_shadow()` definition/declaration after deleting its sole external
caller. Keep `void vgic_reload_spi_lr(struct vcpu *)` and physical kick API.

- [x] **1. Verify the green synchronized-old baseline and inspect its diff.**
  No synchronization/mapping defect remains unresolved; source review must
  approve Task 1, behavior review must approve Task 2. Do not restart full suite.
- [x] **2. Deepen the existing producer with precisely this algorithm.** Add
  `percpu.h`, `vm_config.h`, and `vgic_sgi.h` includes where vGIC now needs them.

```c
void vgic_inject_spi(struct vcpu *target, u32 intid)
{
    bool local = target == current_vcpu();
    u32 pcpu = target->owner->config->pcpu_base + target->vcpu_idx;

    spin_lock(&target->spi_lock);
    vgic_set_spi_shadow_locked(target, intid);
    if (local) {
        SYSREG_WRITE(ICH_LR1_EL2, target->ich_lr[1]);
        target->spi_shadow_pending = false;
    }
    spin_unlock(&target->spi_lock);

    if (!local) {
        asm volatile("dsb ish" ::: "memory");
        vgic_kick_pcpu(pcpu);
    }
}
```

  No pending clear/live write outside the lock; no target wait; no unsupported
  non-current-same-pCPU scheduling case. Document initialized target/owner/index,
  masked EL2 context, PL011-only fixed encoding, initiation-not-guest-ack return,
  live write only for current target, remote target online/kick-capable precondition,
  and no startup/offline/lifecycle guarantees in `vgic.h`.
- [x] **3. Remove vuart delivery knowledge and public shadow wrapper.** After
  the already-locked device snapshot, the complete producer code becomes:

```c
if (notify)
    vgic_inject_spi(&m->vcpu[0], BOARD_PL011_IRQ);
```

  Delete the physical target check, barrier/kick, shadow call and their stale
  explanation from vuart.c. Remove now-unused `vm_config.h` and `vgic_sgi.h`
  includes; retain `percpu.h` if needed by current_vcpu access. Keep
  `vgic_set_spi_shadow_locked()` static, delete only its public locking wrapper.
  Update IRQ/kick comments to name `vuart_rx -> vgic_inject_spi -> kick/reload`;
  do not modify ack/deactivate, SGI draining, VM-off handling or timer branches.
- [x] **4. Fresh targeted regression and discoverability audit.**

```sh
B=$(mktemp -d /tmp/vspi-deep-green-XXXXXX)
make BUILD_DIR="$B" HV_GUEST=svm_dual all vspi check-offsets check-offsets-target
VSPI_MODE=regression VSPI_LOG=/tmp/vspi-deep-regression.log \
HYPERVISOR_ELF="$B/hypervisor.elf" SVM_BIN="$B/vspi/vspi-vm0.bin" \
SVM_BIN2="$B/vspi/vspi-vm1.bin" sh tests/run_vspi_test.sh
git grep -n 'vgic_set_spi_shadow' -- hypervisor
git grep -n -E 'pcpu_base|current_vcpu_id|dsb ish|vgic_kick|spi_shadow|ICH_LR' \
  -- hypervisor/dm/vuart.c
git diff --check
```

  First grep may find only private vGIC helper/comments, not public interfaces
  or producer references; second must have no matches. A grep status of 1 here
  is expected absence, not a tooling failure. Review that Task 3 changes only
  responsibility placement/contracts, not lock semantics or test expectations.
- [ ] **5. One final complete suite, with clean header-dependent objects.**

```sh
set -o pipefail
make clean
make BUILD_DIR=build/test-svm clean
make BUILD_DIR=build/test-svm-dual clean
make BUILD_DIR=build/test-vspi clean
make 2>&1 | tee /tmp/vspi-final-build.log
make test 2>&1 | tee /tmp/vspi-final-make-test.log
```

  Require warning-free normal build, host/target offsets, all existing scenarios,
  and vspi regression. This is the reserved full suite execution. If it fails,
  retain the failure, stop/escalate and use targeted diagnosis; no unsupported
  success claim or silent repeated full-suite budget consumption.
- [x] **6. Commit refactoring separately.** Execution supervisor assigned this
  commit after targeted validation, **before** step 5's reserved full suite and
  final reviewer gates; this checkbox is implementation delivery, not final
  acceptance. Stage only Task 3 files plus
  accurate evidence docs; commit `refactor(vgic): own static SPI delivery orchestration`.
  Report exact commits/files/logs, source proof limits, all mutation outcomes,
  no staged files and required reviewer gate. No push or follow-on scheduler work.

## Every writer's evidence and handoff (including planning writer)

Regenerate `/tmp/vgic-spi-review-20260907-7cc4e57-437617b1.md` after the final
commit of the assigned stage. Reviewers cannot use bash. Include validated
baseline/HEAD, commit list, triple-dot stat and full context diff; separately
include uncommitted work if blocked. The package is outside the repository.

```sh
BASE=7cc4e5771ce1d655430fa67643225aef5b7cec47
PKG=/tmp/vgic-spi-review-20260907-7cc4e57-437617b1.md
{
  printf '# vGIC SPI review package\n\n## Revisions\n```text\n'
  git rev-parse "$BASE"; git rev-parse HEAD
  printf '\n## Commits\n'; git log "$BASE"..HEAD --oneline
  printf '\n## Committed stat\n'; git diff --stat "$BASE"...HEAD
  printf '\n```\n## Committed diff\n```diff\n'; git diff -U10 "$BASE"...HEAD
  printf '\n```\n## Uncommitted diff\n```diff\n'; git diff -U10
  git diff --cached -U10
  printf '\n```\n## Worktree status\n```text\n'; git status --short
  printf '\n```\n'
} > "$PKG"
git diff --cached --quiet
```

Write the stage findings to the runtime-configured external report artifact,
not repo-root scratch. The final structured acceptance report must contain
changed-files, tests-added/updated, commands/logs, residual risks and
`noStagedFiles: true` only after actually checking the index. Planning completion
means docs committed and reviewed by the writer, **not implementation accepted**.

## Planning self-review (completed before planning commit)

- Binding spec §§1–4: Task 3 preserves interface, supported target scope, fixed
  encoding and target reload; deletes external shadow interface/producer obligations.
- §5: independent design inventories UART, GICD, cross-core GICR, live physical
  PPI effects, complete LR1 protocol, init/restore exclusions and lock graph;
  Task 1 separately fixes mapping and synchronization, with source audit/stress.
- §6: fixture initializes secondary stacks/vectors/IRQs via PSCI, rearms timers,
  runs local and remote UART, sibling-to-console IPIs and quiet windows for both
  VMs; Task 2 requires fail-closed RX/replay mutations and old-structure green.
- §§7–8: no scheduler/allocator/lifecycle/focus/Linux expansion; only explicitly
  authorized physical-frame correction is an additional prerequisite bugfix.
- Type/interface consistency: mutable room-check parameter, private locked helper,
  void producer/reload functions and runner image variables have one spelling.
- Initialization review: initial IMSC=0 plus only post-restore guest unmask excludes
  SPI runtime publication from init/restore; unused save is not advertised safe.
- Hardware review: no lock falsely claims to fix stale VENG1 decisions; target
  cannot modify live VMCR while in its own masked IRQ. Cross-frame shadow MMIO
  is genuinely shared and locked regardless of accessing CPU.
- Finite stress does not guarantee an unlocked-race reproduction; explicit mapping
  red/green and detector mutations are distinct evidence. No tests run in planning.

## Task 1a measured evidence

- Execution supervisor corrected the fixture-only plan assumption: combined
  guest EOI completes/deactivates IRQs through EOIR+ISB. Existing TC-trapped
  ICC_DIR_EL1 is not emulated; split-EOI guest support is deferred. No EL2
  physical EOI policy or runtime trap handler changed.
- Initial confounded red `/tmp/vspi-gate-red-qemu.log` had unsupported DIR
  and compiler post-index MMIO (ISV=0); explicit fixture LDR/STR fixes the latter.
  `/tmp/vspi-gate-supported-red-qemu.log` lost a pre-IMSC VM1 mode byte; UART
  setup now precedes the timer detector. Neither log is mapping evidence.
- Initial mapped run `/tmp/vspi-gate-green-qemu.log` caught BOOT interleaving
  with the existing secondary online printk. Bounded BOOT retry plus host
  startup-announcement wait was approved; acceptance markers remain exact.
- Final supported red: `/tmp/vspi-gate-final-red-qemu.log`, exit 1 with
  VM0 GATE PASS/DONE and VM1 ACTIVE/FAIL 1. Used unmapped hypervisor from
  `/tmp/vspi-gate-ready-red-bytBMR` (all/offset build log
  `/tmp/vspi-gate-ready-red-build.log`) plus rebuilt finalized fixtures
  (`/tmp/vspi-gate-final-red-fixture-build.log`).
- Fresh mapped green: /tmp/vspi-gate-final-green-qHoFgP,
  `/tmp/vspi-gate-final-green-build.log` (all/vspi/host+target offsets),
  `/tmp/vspi-gate-final-green-qemu.log` (both GATE PASS/DONE, exit 0).
  This specifically establishes early-timer mapping, not race absence.

## Task 1b completion (prerequisite review accepted)

All implementation/validation steps are complete. Independent reviewer run
`e5e42319-e0bf-4007-ad86-5dca9886677b` accepted the synchronization gate at
`91712edaf56fc54a05760be4b1b9bff660cd4242`; supervisor verified its native
structured verdict `clear` before Task 3. The evidence below records Task 1's
state at completion, not the later producer structure.
See the synchronization design's implementation evidence for the full access
inventory, actual lock graph, init/live-VMCR exclusions, final clean build
directories and logs. Unlocked stress failed with guest GIC_STATE before locks;
final five bounded stress runs, dual-SVM/shell and a fresh single-VM timer
scenario passed. Producer branching/public shadow setter remain untouched.
No Task 2/3 work, replay mutation, suite integration or full `make test` yet.

Fixture ELF permissions are now explicitly RX/RW with page-aligned data to
eliminate the new fixture's linker RWX warning, without disabling diagnostics.
The existing svm3 linker warning is disclosed and unchanged. Final zero-warning
all/vspi/offset build and final-layout five stress logs are recorded in the
synchronization design. No source edits followed those runs.


## Task 2 completion (regression review accepted)

Implemented only the regression on synchronized old production at `91712ed`.
The standalone test commit retains vuart's local/shadow/kick branch and the
public shadow setter unchanged. Independent reviewer run
`e05670d1-a9d2-4aad-9e29-079436768d8a` accepted the regression gate at
`857abfca1a63cb16e44ae9bcf3a2113c4f738649`; supervisor verified its native
structured verdict `clear` and explicitly authorized Task 3. The evidence
below records Task 2's unchanged old-producer baseline.

- `R` retains the existing interrupt-only mode/payload consumer, exact 4096
  byte sequence per VM, CPU_ON/stacks/vectors, shared-GIC/UART stress and
  periodic timer rearm. It adds a ten-tick settling interval then a stable
  ten-tick interval, both bounded by one five-second counter deadline.
- After DRAINED, CPU1 sends 16 serialized INTID1 SGIs to VM-local CPU0 in
  each VM. Primary release-publishes requests; sibling acquire-observes,
  checks the prior completion, writes ICC_SGI1R/ISB, release-publishes sent.
  Only the CPU0 IRQ handler publishes completion, after combined EOIR/ISB.
  Primary acquire-waits for both sent and completed before IPI PASS. Every
  quiet window requires ten further CPU0 timer completions and CPU1 progress,
  exact RX total/empty FIFO/clear RX status and unchanged UART IRQ count.
  The drained IRQ count is retained across all rounds, not reset at markers.
  There is at most one outstanding SGI, no self-IPI and no LR-private inspection.
- Runner defaults to regression, checks every DRAINED/IPI/QUIET/REPLAY/DONE
  marker exactly, and sends no payload after stress. `timeout -k 2 180`
  confines cleanup to the child's process group with a two-second KILL grace
  after TERM; failure runs leave no QEMU child. `test-qemu-vspi` is now a
  default `test` prerequisite; no full `make test` was executed in this task.

### Native commands and evidence

For each build below, `make BUILD_DIR=$B HV_GUEST=svm_dual all vspi` passed.
Old-producer and restored builds additionally ran `check-offsets
check-offsets-target`. All five build logs below contain no warnings.
All direct runs used `VSPI_MODE=regression VSPI_LOG=$log
HYPERVISOR_ELF=$B/hypervisor.elf SVM_BIN=$B/vspi/vspi-vm0.bin
SVM_BIN2=$B/vspi/vspi-vm1.bin sh tests/run_vspi_test.sh` unless noted.

| Experiment | Fresh build / build log | QEMU log / result |
|---|---|---|
| Old synchronized producer | `/tmp/vspi-old-producer-ZIz3Zv`; `/tmp/vspi-old-producer-build.log` | `/tmp/vspi-old-producer-regression.log`: exit 0, both VMs all 128 RX batches and all 16 IPI/QUIET pairs, REPLAY PASS/DONE |
| Local injection suppressed (temporary no-op in `vgic_inject_spi`) | `/tmp/vspi-mutation-rx-jKWG1s`; `/tmp/vspi-mutation-rx-build.log` | `/tmp/vspi-mutation-rx.log`: exit 1, missing VM0 ACTIVE; IRQ-only mode receipt never succeeds |
| Unconditional reload (temporary pending-gate removal) | `/tmp/vspi-mutation-replay-Hx5w6W`; `/tmp/vspi-mutation-replay-build.log` | `/tmp/vspi-mutation-replay.log`: exit 1, VM0 DRAINED, IPI PASS 1, FAIL 7 |
| VM1-only unconditional reload | `/tmp/vspi-mutation-replay-vm1-2R1qUD`; `/tmp/vspi-mutation-replay-vm1-build.log` | `/tmp/vspi-mutation-replay-vm1.log`: exit 1, VM0 all replay stages pass, VM1 DRAINED, IPI PASS 1, FAIL 7 |
| Restored production | `/tmp/vspi-old-producer-restored-yBCsJ0`; `/tmp/vspi-old-producer-restored-build.log` | `/tmp/vspi-old-producer-restored.log`: exit 0 via default-mode `VSPI_LOG=... make VSPI_BUILD_DIR=$B test-qemu-vspi`; both VMs all required stages |

Runner stdout/stderr logs are `/tmp/vspi-old-producer-run.log`,
`/tmp/vspi-mutation-{rx,replay,replay-vm1}-run.log`, and
`/tmp/vspi-old-producer-restored-target.log`. Mutation exit codes are also in
`/tmp/vspi-mutation-{rx,replay,replay-vm1}.status` (all 1).

The first VM1 mutation invocation had an operator typo in SVM_BIN2
(`vspi-vspi-vm1.bin`); the runner exited 1 on child startup failure. Preserved
`/tmp/vspi-mutation-replay-vm1-startup-failed{,-run}.log` and `.status` are
**not replay evidence**. Execution stopped and the supervisor explicitly
approved one same-protocol retry correcting only that path. Before retry,
all three image files were checked and SHA256 identities/build provenance
recorded in `/tmp/vspi-mutation-replay-vm1-images.log`. The corrected run above
reused that already-built mutation; production source was already restored.
No execution-mode fallback or permanent mutation switch was used.

Each experiment restored only `vgic.c` from HEAD; `git diff --exit-code --
hypervisor/` passed after restoration and before/after the final fresh build.
Retained G and S modes passed using restored images:
`/tmp/vspi-task2-retained-gate.log`, `/tmp/vspi-task2-retained-stress.log`.
`sh -n tests/run_vspi_test.sh` and `git diff --check` passed. A final process
listing found no remaining QEMU process. No header or production file changed
in this task; assembly entry/vectors/linker layout from Task 1 are unchanged.

This is finite QEMU guest-behavior and detector-sensitivity evidence, not a
formal synchronization proof. Task 1's independent source-level argument and
review remain essential. Fixed LR capacity/coalescing, static pinning,
startup/offline guarantees, Linux, lifecycle and scheduling remain outside
this regression's claims. Task 3 and the reserved full suite remain next.

## Task 3 implementation delivery (final review/validation pending)

Supervisor confirmed both prerequisite reviews above before production edits.
The native review reports are respectively:
`/tmp/pi-subagents-uid-1000/async-subagent-runs/e5e42319-e0bf-4007-ad86-5dca9886677b/structured-output/pi-subagent-structured-gw7d3N/output.json`
and
`/tmp/pi-subagents-uid-1000/async-subagent-runs/e05670d1-a9d2-4aad-9e29-079436768d8a/structured-output/pi-subagent-structured-dCczkv/output.json`.
Task 3 was expressly assigned to commit after focused checks; step 5's one
full suite, Task 3 gate and final Standards/Spec reviews are still pending.

### Source-level preservation argument

- `vgic_inject_spi(target, intid)` now implements the exact planned algorithm:
  local iff `target == current_vcpu()`, physical slot
  `target->owner->config->pcpu_base + target->vcpu_idx`. The existing caller
  still selects console vCPU0. Under current stable 1:1 mapping, that pointer
  comparison is equivalent to the old physical-index comparison; no
  non-current-same-pCPU case or runtime placement change is supported.
- The same private encoder/helper publishes LR1 and pending while holding
  the same `spi_lock`. Local live write and pending clear stay inside that
  critical section. Remote publication unlocks before `dsb ish` and kick,
  exactly as the old caller/wrapper protocol. Consumer reload is unchanged:
  lock, conditional live write/clear, unlock. Thus no new access gap allows
  a consumer to clear a newer publication; delayed kicks see false after
  consumption unless a new publication has intervened. Coalescing is unchanged.
- vuart's device critical section, RIS/IMSC snapshot and unlock are unchanged;
  the subsequent masked decision calls the existing producer once. There is
  no nested device/SPI lock, no lock held across kick, printing or target wait.
  The new producer does not mask/unmask EL2 IRQs. Boot init, initial-only
  restore and dead save are unchanged, so Task 1's exclusions still apply.
- Fixed LR1 HW=0/Group1/0xA0 encoding and LR0/LR2 code are unchanged.
  `irq_handler.c` and `vgic_sgi.c/.h` change only cross-reference comments;
  target reload, ack/deactivate, SGI bitmap and VM-off kick behavior are intact.
  The misleading old claim that PL011 deactivation preceded injection was
  corrected: deactivation remains after physical RX draining in the handler.
- Public `vgic_set_spi_shadow` definition/declaration and its sole external
  use are removed; only the static locked helper remains. vuart removes only
  the newly orphaned vm_config/vgic_sgi includes, retaining percpu for MMIO.
  Header contract states initialization/index, masked context, supported
  PL011-only encoding, current-target live eligibility, online/kick-capable
  remote precondition, initiation-not-ack return and lifecycle limitations.

### Commands and evidence

All build directories were freshly created with `mktemp -d`, not reused after
header edits. Native commands exited zero with no warning diagnostics:

| Command | Build log |
|---|---|
| `make BUILD_DIR=/tmp/vspi-deep-normal-t0OUvC all check-offsets check-offsets-target` | `/tmp/vspi-deep-normal-build.log` |
| `make BUILD_DIR=/tmp/vspi-deep-green-EdezIa HV_GUEST=svm_dual all vspi check-offsets check-offsets-target` | `/tmp/vspi-deep-build.log` |
| `make BUILD_DIR=/tmp/vspi-deep-single-METgd1 HV_GUEST=svm all check-offsets check-offsets-target` | `/tmp/vspi-deep-single-build.log` |

For each `mode` in `regression gate stress`, ran:

```sh
B=/tmp/vspi-deep-green-EdezIa
VSPI_MODE="$mode" VSPI_LOG="/tmp/vspi-deep-$mode.log" \
HYPERVISOR_ELF="$B/hypervisor.elf" SVM_BIN="$B/vspi/vspi-vm0.bin" \
SVM_BIN2="$B/vspi/vspi-vm1.bin" sh tests/run_vspi_test.sh
```

All three returned zero/`VSPI ALL PASS`; corresponding runner output is at
`/tmp/vspi-deep-{regression,gate,stress}-run.log`. R required 128 RX batches
(4096 ordered bytes), shared-GIC/UART stress, DRAINED and all 16 sibling IPI
and quiet windows, REPLAY PASS/DONE per VM. G and S retained their own markers.
No fixture, runner or Makefile edit or permanent mutation was made in Task 3.
Old-producer mutation results remain Task 2's independent sensitivity evidence.

`/tmp/vspi-deep-source-audit.log` records the Task 3 grep audit: shadow setter
matches only the private helper definition/use in vgic.c; forbidden physical
placement/LR/barrier/kick terms have zero matches in vuart.c. It also inventories
LR1/pending/init/save/restore and confirms no diff in tests, Makefile,
hypervisor/include or vgic_v3_mmio.c. `git diff --check` passed.
An initial inspection guessed the wrong percpu.h directory; `find` located
`hypervisor/include/percpu.h`, which was read before builds. No native build,
test or setup failure occurred and no execution-mode fallback was used.

Finite guest stress/regression is not a formal race proof; the accepted
source argument above remains necessary. No scheduler, queue, lifecycle fix,
startup/offline delivery guarantee or Linux gate is claimed. No full
`make test` was run in this stage. The required scratch review package is
regenerated after the standalone refactor commit, with baseline-to-HEAD
committed diff and an actually checked empty index.
