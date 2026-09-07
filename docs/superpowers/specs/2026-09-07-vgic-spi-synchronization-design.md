# Static vSPI synchronization prerequisite design

**Status:** Implemented and independently accepted at `91712ed` (review run
`e5e42319-e0bf-4007-ad86-5dca9886677b`, structured verdict `clear`, verified by
the execution supervisor). The source argument and Task 1 evidence below
record the synchronized old producer. Task 2's regression gate was subsequently
accepted at `857abfc`; Task 3 consolidates the same protocol in `vgic_inject_spi`
and removes the public shadow setter. See the parent delivery spec/plan for
current implementation status; final delivery review/full validation remain pending.
Independent prerequisite to
[the approved delivery design](2026-09-07-vgic-spi-delivery-deepening-design.md).
Execution baseline: `7cc4e5771ce1d655430fa67643225aef5b7cec47`.
The execution ruling requires **both source-level synchronization reasoning and
bounded guest stress**. Green stress is not a formal race proof or a substitute
for reviewing every access. This document does not implement M11 scheduling.

## Scope and source findings

Read alongside ADR-0014, ADR-0010 and its M9/M10 amendments, ADR-0001, and
`2026-08-10-m11-vcpu-context-switch-design.md` (S0 prerequisites only).
Reference consulted before selecting synchronization:
`../bao-hypervisor/src/arch/armv8/aarch64/inc/arch/spinlock.h` (ticket/acquire/release),
and `../bao-hypervisor/src/arch/armv8/vgic.c:968-987` (serialize emulated
GICD accesses and interrupt state). Reuse this repository's ticket lock; do not
copy Bao's allocator, interrupt ownership system, or WFE/event protocol.

The existing producer structure is retained until this prerequisite and its
regression baseline are independently accepted. Relocating code is not a race fix.

| State / actual access paths | Implemented rule |
|---|---|
| `struct vuart`: ring, RIS, IMSC, divisor/control/IFLS registers | Physical UART RX on pCPU0 races with MMIO from either vCPU. One embedded `struct spinlock lock`, initialized by BSS; all mutable reads/writes take it. |
| `vuart_rx_has_room()` | Also takes the vuart lock; parameter is `struct vm *` because locking mutates the object. It is a snapshot, not a reservation. Only pCPU0 produces; concurrent consumers only free space between check and push. |
| `console_focus`, shell state | Read/written only by the pCPU0 physical UART/shell path. Keep existing behavior and ownership; not moved into the vuart lock. |
| `g_vgicd[vmid]` | Shared by both vCPUs. One file-static `vgic_mmio_lock[NR_VMS]` covers each complete MMIO access including the handler's 64-bit IROUTER fast path and all read paths. |
| `g_vgicr[vmid][frame]` | Also shared: frame is selected by guest IPA offset, **not the accessing vCPU**. The same per-VM MMIO lock covers RD and SGI frames, all reads and writes. No nested per-frame lock. |
| `ich_lr[1]`, `spi_shadow_pending` | Appended `struct spinlock spi_lock` after the asm-visible vCPU prefix; ordinary `bool`, not volatile synchronization. Payload, pending, live write, local completion and target reload form one critical section per operation. |
| `ich_lr[0,2,3]`, live LR registers, live VMCR | Still target-PE-owned under static pinning. SGI bitmap uses the existing independent `sgi_lock`; it is released before LR2 injection. No broad vCPU lock. |

### Narrow prerequisite exception: physical PPI target mapping

Source discovery, explicitly authorized by the execution supervisor: in
`vgic_v3_mmio.c`, `vgicr_write_sgi(cpu, ...)` passes a VM-local frame number to
`gic_ppi_set_enable()`, which indexes a **physical** GICR in `gic_v3.c`.
VM1's frame 0/1 currently re-enables physical CPU0/1 instead of CPU2/3.
Correct to `m->config->pcpu_base + cpu`, where `cpu` is the addressed frame,
not `current_vcpu()->vcpu_idx`. Keep the logical index for GICR_TYPER/shadows.
This is a separately identified bugfix, never part of the later SPI relocation.
No change to timer LR0/HW forwarding, EOIR/DIR, ICENABLER guest semantics,
physical timer readiness policy, or lifecycle policy is authorized.

### Physical state is not shadow state

Do **not** add a physical-GICR lock. `gic_ppi_set_enable()` performs one W1S or
W1C write plus `dsb sy; isb`; it does not read-modify-write a C word.
`gic_init()` and `secondary_main()` initially W1-enable PPI27 and the kick SGI;
remote PPI enables commute with these enables. Their other GICR setup writes
(group, priority, WAKER) are PE-local initialization, not runtime shared RMW.
The target cannot handle a gate IRQ during its initialization: EL2 DAIF is
masked through the one-time restore and first guest entry.

Runtime gate ordering in `irq_handler.c` is deliberately narrower than a
virtual timer readiness state machine:

1. The target reads **its own live** `ICH_VMCR_EL2.VENG1`.
2. If false, the target software-injects, drops/deactivates the physical IRQ,
   then masks its physical PPI before returning to EL1.
3. Only the target guest can change that live VENG1. It cannot run during this
   masked EL2 handler; another PE's GICR MMIO changes a shadow/enable register,
   not this target's live VMCR. Thus the false decision cannot become stale
   against a concurrently changed VENG1 before the disable.
4. A remote enable before the target's W1-disable linearizes earlier and may
   be superseded. A remote enable after it re-arms the PPI; if VENG1 is still
   false the target may gate again. This preserves the existing policy, not a
   guarantee that an early remote enable makes an unready target ready.
5. The supported ready sequence is target EL1 `ICC_IGRPEN1_EL1=1; isb`, then
   a virtual PPI enable. Any earlier target false gate has completed before
   this target sequence; subsequent target IRQs see VENG1 enabled. The guest
   test intentionally exercises that sequence after an early expiration.

A lock around only the final hardware write would not establish a different
readiness ordering. Arbitrary remote writes on behalf of an unready target,
future descheduling, or turning VENG1 off again are not new guarantees here.
If implementation finds another live-VMCR writer or target IRQ execution during
initialization, stop: this exclusion argument no longer holds.

## Critical sections and lock graph

`vuart_mmio_handler()` forwards DR writes to `console_putc()` **outside** the
vuart lock. All other emulated register operations execute under the lock;
static AMBA identity reads may harmlessly share this small wrapper. In
`vuart_rx()`, push/check ring, set RIS and snapshot `imsc & RX_MASK` under the
lock, then unlock before the old local/shadow/kick branch. A concurrent IMSC
write linearizes before or after that snapshot; do not add retroactive delivery
on unmask or retract already initiated delivery. A late notification may find
an already-drained FIFO, as in the existing coalescing model.

No new lock is held over console output, debug printing, a kick, PSCI CPU_ON,
or any wait for target/guest acknowledgement. GIC debug output is before its
MMIO lock. The IROUTER branch uses the common unlock/return. The GICR physical
W1 enable may remain inside the tiny MMIO critical section: it has no lock or
wait for another PE, and orders the side effect with its shadow access.

```mermaid
flowchart LR
    UART[Physical RX / pCPU0] --> U[vuart lock: device state + mask snapshot]
    MMIO[Guest UART MMIO] --> U
    MMIO --> P[print lock: DR TX only]
    U -. unlock before delivery .-> S[per-vCPU SPI lock]
    GMMIO[Guest GICD or any GICR frame] --> G[per-VM MMIO lock]
    G --> W[Physical PPI W1 enable: no lock]
    S -. unlock then dsb ish .-> K[Physical kick: no lock]
    K --> IRQ[Target masked EL2 IRQ]
    IRQ --> B[SGI bitmap lock]
    B -. unlock before LR2 and LR1 .-> S
```

All graph dashed edges release the preceding lock, so there are **no new
nested lock-acquisition edges**. Existing print and SGI locks are leaves too.
`spinlock.h` describes actual shared state and states explicitly that locks
do not mask interrupts. Every current caller runs in boot with DAIF masked or
an EL2 synchronous/IRQ exception with DAIF masked. No path enables EL2 IRQs
inside these sections; `irq_handler_asm.S` deliberately retains masking until
`eret` (its old file-header claim otherwise is stale, not actual code).
No IRQ masking instruction or IRQ routing behavior needs changing.

## Complete LR1 protocol

A private `vgic_set_spi_shadow_locked(vcpu, intid)` encodes the existing
software HW=0, Group1, priority 0xA0, pending LR1 and sets pending. It requires
`spi_lock` and does not touch live hardware. During the prerequisite, public
`vgic_set_spi_shadow()` remains a locking wrapper for the **old** producer.
Local `vgic_inject_spi()` takes the lock once, calls the private locked helper,
writes live LR1, clears pending, unlocks; never calls the locking wrapper from
inside its lock. Target reload locks, conditionally writes live LR1 and clears
pending under that same lock, then unlocks. Never copy payload out and write
hardware after unlocking. Later consolidation changes only who selects these
operations and kicks, not their serialization.

```mermaid
sequenceDiagram
    participant P as Remote producer
    participant L as Target SPI lock / shadow
    participant T as Target PE
    P->>L: lock; payload := encode; pending := true; unlock
    Note over P: dsb ish before kick, outside all locks
    P->>T: physical kick
    T->>L: lock; if pending: live LR1 := payload; pending := false
    T->>L: unlock
    Note over L,T: local injection holds this same lock through live write and completion
    P->>L: subsequent publication (before or after consumer lock)
    T->>L: unrelated kick: no pending means no LR1 write
```

The ticket lock's acquire/release plus compiler memory clobbers make the
payload/pending pair indivisible to producers/consumer. A publication before
consumption is consumed; a publication after consumption leaves pending set
for its kick. No consumer can clear a newer publication inside an unlocked
read/clear/write gap. Two publications may coalesce; there is no IRQ-per-byte
or queued-IRQ guarantee. A local completion cannot clear a later remote
publication because its live write and clear remain under the lock. Delayed
kicks are harmless: the first may consume the newest pending payload, later
ones observe false. Keep the remote producer's `dsb ish` after unlock and
before `vgic_kick_pcpu()`; release is mutual exclusion/publication, the barrier
orders publication before notification, and neither substitutes for the other.

## Initialization / restore exclusions (not assertions of future safety)

- `vm[]` (including vuart locks, IMSC and vCPU SPI locks) is BSS-zeroed once
  in `head.S`, before any secondary starts. File-static MMIO locks/shadows are
  initialized before VM execution. Never reset a lock in `vgic_init()`.
- `vm_init()` initializes each boot vCPU's LR state before booting other PEs;
  `secondary_main()` initializes its own LR state again before restore. Boot
  IMSC is zero. Only guest MMIO can enable IMSC, and that guest cannot execute
  until its target's `vgic_init()` and sole `vgic_restore()` have finished.
- Therefore early routed RX may modify the protected ring/RIS, but cannot
  publish LR1 while initialization/restore is running. The vuart lock's
  IMSC observation also carries the ordering from the guest's enable. In the
  current source only console vCPU0 is an SPI target; sibling init cannot
  reset its state. No new preboot/offline delivery guarantee is inferred.
- `vgic_restore()` has only the two initial-entry call sites in `vm_run()`
  and `secondary_main()`; neither `vcpu_run()` loop restores again.
  `vgic_save()` has **no callers**. Both exclusions are documented beside these
  functions, replacing the inaccurate anticipated timer-switch comment.
  Do not lock dead save code and call it scheduler-safe: a future save could
  overwrite a remotely published shadow with live state even under a lock.
- Owner/config/index and static pCPU mapping are prepared before entry and
  never change. After consolidation direct live injection requires
  `target == current_vcpu()`; the remote target is computed from its owner
  config plus index. M11 must re-audit all these exclusions.

```mermaid
classDiagram
    class VM {
        u32 id
        Vuart vuart
    }
    class Vuart {
        spinlock lock
        RX ring
        u32 ris
        u32 imsc
    }
    class VCPU {
        VM owner
        u32 vcpu_idx
        spinlock spi_lock
        u64 ich_lr1
        bool spi_shadow_pending
    }
    class MMIOState {
        spinlock per_VM_lock
        distributor_shadow
        redistributor_shadows
    }
    VM "1" *-- "2" VCPU
    VM "1" *-- "1" Vuart
    VM "1" --> "1" MMIOState : indexed by id
```

## Evidence and acceptance limits

The companion [implementation plan](../plans/2026-09-07-vgic-spi-delivery-deepening.md)
provides executable task algorithms and commands. One `vspi` guest fixture
builds into two VM-labelled binaries, using the existing `svm_dual` hypervisor
profile; **svm5 remains reserved for M11**.

1. A focused red/green early-timer test exposes the physical mapping defect:
   expire before VENG1, then enable interface/PPI and require at least five
   periodic IRQ completions on each of all four guest vCPUs. One software
   pending tick cannot satisfy it.
2. Bounded stress: five QEMU runs, each VM receives 4096 ordered bytes in
   bounded batches while its two vCPUs contend on same-word GICD set/clear,
   split IROUTER writes, and cross-vCPU accesses to one GICR frame; exercise
   UART status/mask/config accesses concurrently. Check combined values,
   exact RX data, both-vCPU timers, deadlines and explicit completion.
3. On the synchronized **old producer**, add positive UART/SGI/timer stages:
   each VM drains, completes interrupts, observes quiet, then sibling sends
   a serialized IPI to console vCPU0. Verify IPI arrival and no UART replay
   over a timer-progress window. Repeat after consolidation unchanged.
4. Temporarily suppress delivery and temporarily break pending consumption
   to demonstrate detector failure; restore experiments before commits.

Source review must inventory all access paths again, not merely compare this
text to itself. Stress explores a finite QEMU schedule set, not all races;
fixed LR capacity/coalescing, startup/offline delivery, Linux boot, scheduler
save/restore and lifecycle defects remain outside its claim. Mapping-test
red/green evidence is deterministic bug evidence, not evidence that unlocked
races must fail on every run. No builds or tests were run during planning; implementation evidence follows.

## Task 1 implementation evidence and access audit

The implemented lock graph is the graph above: **no new nested acquisition
edges**. The old vuart local/shadow/dsb/kick producer is still intact, and
`vgic_set_spi_shadow()` is still public. Task 2's sibling-IPI/quiet/replay
regression and mutation experiments, and Task 3's consolidation, have **not**
been executed. No full `make test` was used here; its reserved execution remains
for the final stage.

### Rechecked source paths (not inferred from test output)

- `vuart.c:64–129`: empty/full/pop/read/write are private and only reached
  under the device lock. DR read advances tail and updates RIS; FR, RIS/MIS,
  IMSC and stored config reads/writes share that lock. `vuart_mmio_handler`
  special-cases DR TX before locking (print lock only), otherwise holds one
  device lock through the complete operation. `vuart_rx_has_room` snapshots
  under the same lock; sole caller is physical pCPU0's IRQ drain. `vuart_rx`
  locks push/RIS/mask snapshot, then releases before either old producer branch.
  Search found no device state reset or other accessor outside this file;
  `vm[]` is BSS and `vm_init` does not memset/reset it at runtime. The single
  producer means intervening consumers can only increase room.
- `vgic_v3_mmio.c`: only `vgicd_mmio_handler` reaches the private distributor
  read/write helpers or 64-bit IROUTER fast path; the entire operation is
  protected by `vgic_mmio_lock[m->id]`, including reads and split-word RMW.
  `vgicr_mmio_handler` rejects invalid frames without accessing state and then
  protects all addressed RD/SGI helpers with that same per-VM lock. Both
  vCPUs may select frame 0: no current-vCPU ownership assumption is used.
  `g_vgicd` is BSS, `g_vgicr` static initialized data; neither has a runtime
  reset path. Debug printk precedes locking in both handlers.
- `vgic.c`: the private locked encoder/helper publishes LR1 payload/pending;
  the public setter locks that operation. Local inject holds the same lock
  through the helper, live write and pending clear; target reload holds it
  through the conditional live write and clear. No payload is copied out
  for a later unlocked write. Search found no runtime LR1/pending writer
  elsewhere. `irq_handler` still calls reload after SGI drain, whose independent
  bitmap lock has already been released. `vuart_rx` retains `dsb ish` after
  shadow publication unlock and before kick. Encoding remains HW=0, Group1,
  priority 0xA0, LR1; LR0 timer and LR2 SGI code is unchanged.
- `head.S` zeroes BSS before secondaries exist; both entry paths mask DAIF
  before C. `vm_init` initializes boot vCPUs and owner/index/config before
  `vm_run` starts VM1. `secondary_main` initializes its own vGIC before sole
  initial restore/entry. Only these two initial-entry functions call restore;
  save has no callers. IMSC starts zero, and enabling it requires guest
  execution after restore. A sibling cannot enable it earlier: only the
  already-running console vCPU can request that sibling's CPU_ON. Thus no
  runtime SPI publication overlaps initial LR reset/restore, even if early RX
  fills ring/RIS. Locks are never reinitialized by `vgic_init`.
- `vmexit.c` PMR/CTLR traps update only the executing PE's live VMCR and
  preserve VENG1; initial restore is the other EL2 live VMCR write. Guest
  IGRPEN1 is target-local. These do not run concurrently with that target's
  IRQ handler. `irq_handler_asm.S` has no DAIFClr instruction (its old header
  comment is stale); the actual tail explicitly retains IRQ masking to eret.
  `vmexit_asm.S` likewise does not enable IRQs in EL2. Therefore no supported
  same-PE reentry can deadlock any new lock.
- `gic_ppi_set_enable` is one physical W1 write plus dsb/isb, not C RMW.
  `gic_init`/`secondary_main` own other physical GICR initialization. Runtime
  enable from virtual SGI-frame MMIO now targets `pcpu_base + addressed frame`;
  disable in IRQ remains target-local. The live-VENG1 false-gate exclusion
  above was checked against these writers; no physical lock or readiness
  policy change was introduced. Console/shell state remains pCPU0-owned.

Search commands from Task 1 step 8 are captured in
`/tmp/vspi-sync-source-audit.log`; source inspection additionally covered
`vm.c`, `head.S`, `secondary.c`, `irq_handler{,_asm.S}`, `vmexit{,_asm.S}`,
`vgic_sgi.c`, `gic_v3.c`, `print.c`, and all touched helpers/initializers.
The argument is confined to current static pinning, masked EL2, initial-only
restore and a single console producer. A future save, reset, device producer,
IRQ-unmask, or scheduler requires a fresh audit.

### Fixture adjustments approved during execution

The guest uses **combined EOI** (`ICC_CTLR_EL1=0`, original IAR written to
EOIR then ISB), completing/deactivating IRQs before any main-stage marker.
The initial plan's split-EOI assumption hit an existing unhandled TC-trapped
ICC_DIR_EL1, not a synchronization bug. Split-mode support remains deferred;
EL2 physical split EOI and HW timer linkage are unchanged. Explicit guest
LDR/STR MMIO accessors avoid compiler post-index stores (ISV=0 is unsupported
by the existing decoder), without any production adapter or test hook.

UART setup precedes the primary early-timer detector so a red VM1 can retain
its command while waiting five seconds for missing timers. Only VM0 retries
BOOT, every quarter second until mode. Host requires an exact BOOT line plus
all three existing secondary-online printk announcements before sending mode.
These announcements are **transport quiescence only**, not evidence of guest
readiness; per-core secondary_ready/timer checks remain mandatory. This bounded
bootstrap handles startup printk interleaving without stitching guest markers
or using a guessed sleep. It depends narrowly on the existing startup log.

Stress adds UART register traffic while CPU1 waits for the paced RX batch,
not only inside GIC rounds: fast GIC rounds cannot accidentally leave the
entire 2ms-spaced input window without a concurrent device accessor. RX is
consumed only by CPU0's virtual UART handler. All barrier/batch waits use
CNTVCT deadlines plus error slots; even the IRQ FIFO drain has a deadline.
The 180-second mode wait and post-DONE timer-enabled idle are deliberate
exceptions to five-second phase waits.

### Commands and measured outcomes

- Mapping commit: `6a0b4b5` (`fix(vgic): rearm timer on the addressed VM physical CPU`).
  Supported final red `/tmp/vspi-gate-final-red-qemu.log`: exit 1, VM0
  GATE PASS/DONE, VM1 ACTIVE then FAIL 1. Fresh mapped green
  `/tmp/vspi-gate-final-green-qemu.log`: both GATE PASS/DONE, exit 0.
  Detailed initial fixture-confounder logs and directories are in the plan's
  Task 1a evidence; none is claimed as mapping or race evidence.
- Stress added **before locks**: `make BUILD_DIR=/tmp/vspi-stress-unlocked-3jNZkH
  HV_GUEST=svm_dual all vspi check-offsets check-offsets-target` passed
  (`/tmp/vspi-stress-unlocked-build.log`). First run exited 1 with VM0
  FAIL 5 at RX READY 30 (`/tmp/vspi-stress-unlocked-qemu.log`).
  Final strengthened fixture was guest-only rebuilt against that same saved
  unlocked hypervisor (`/tmp/vspi-stress-final-unlocked-fixture-build.log`):
  `/tmp/vspi-stress-final-unlocked-qemu.log` exited 1 with VM0 STRESS PASS/DONE
  and VM1 FAIL 5 at RX READY 15. These are observed shared-GIC consistency
  failures, not guaranteed deterministic reproductions or proof of each
  UART/LR1 race individually. No production mutation was needed for this red.
- Final fresh dual build: `make BUILD_DIR=/tmp/vspi-sync-final-6xIYbZ
  HV_GUEST=svm_dual all vspi svm svm4 check-offsets check-offsets-target`
  passed; `/tmp/vspi-sync-final-build.log`. C/EL2 compilation was warning-free,
  but the initial fixture ELF linker reported RWX LOAD segments (see below). New header
  layouts passed both host and cross-compiled assembly-offset checks.
- For `n=1..5`, ran `VSPI_MODE=stress
  VSPI_LOG=/tmp/vspi-sync-checked-stress-$n.log HYPERVISOR_ELF=$B/hypervisor.elf
  SVM_BIN=$B/vspi/vspi-vm0.bin SVM_BIN2=$B/vspi/vspi-vm1.bin
  sh tests/run_vspi_test.sh`, with B the final fresh dual build above and its final fixture-only rebuild
  (`/tmp/vspi-sync-final-fixture-build.log`). All
  five exited 0 with VSPI ALL PASS. Each required two GATE PASS, 128 exact
  RX READY/PASS pairs per VM, two STRESS PASS and two DONE. Totals: 40,960
  ordered RX bytes and 40,960 shared-GIC rounds across five runs/two VMs,
  plus 256 empty-FIFO UART mask rounds per VM/run and timer progress on all
  four guest vCPUs. Earlier five green runs (before stronger paced-window
  UART access) remain `/tmp/vspi-sync-stress-{1..5}.log`.
- Same final dual images: `sh tests/run_svm4_test.sh` and
  `sh tests/run_shell_test.sh` passed with HYPERVISOR_ELF/SVM_BIN/SVM_BIN2
  pointing to that build's hypervisor/svm/svm4 artifacts; logs
  `/tmp/vspi-sync-final-svm4.log`, `/tmp/vspi-sync-final-shell.log`.
- Additional fresh NR_VMS=1 build: `make BUILD_DIR=/tmp/vspi-sync-single-aMp5Q5
  HV_GUEST=svm all svm3 check-offsets check-offsets-target` passed
  (`/tmp/vspi-sync-single-build.log`, existing svm3 ELF RWX linker warning
  retained, no C warnings); its `sh tests/run_svm3_test.sh` passed
  (`/tmp/vspi-sync-single-svm3.log`) with the matching image variables.
- Final fixture linker script separates RX and RW PT_LOAD segments (data
  page-aligned), removing its RWX warning without suppressing diagnostics.
  Entry IPA and binary-loader semantics are unchanged. The five checked
  stress logs above use this final layout; the earlier layout's five runs
  are `/tmp/vspi-sync-final-stress-{1..5}.log`.
- Additional **fresh, zero-warning** final source build: `make
  BUILD_DIR=/tmp/vspi-sync-warning-free-cMZcFr HV_GUEST=svm_dual all vspi
  check-offsets check-offsets-target`, `/tmp/vspi-sync-warning-free-build.log`.
  Both GATE PASS/DONE with that build's matching images, exit 0:
  `/tmp/vspi-sync-final-gate.log`.
- `git diff --check` and `sh -n tests/run_vspi_test.sh` passed.

Bounded stress explores finite QEMU schedules. The source-level critical-section
and initialization argument, independently reviewed at the prerequisite gate,
is essential even with observed red and the bounded green runs. No formal race proof,
IRQ-per-byte guarantee, queueing, offline/startup reliability, Linux gate,
scheduler safety or later replay-detector acceptance is claimed.
