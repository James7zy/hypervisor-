# Slice 4+5 (merged) — RESOLVED ✅ nproc=2 achieved (deterministic 3/3)

## RESOLUTION (lead #3 was correct, and deeper)
ROOT CAUSE: QEMU's `ICH_HCR_EL2.TC=1` (which we set to trap ICC_SGI1R_EL1 for the
cross-core IPI relay) traps a BROADER set than the architectural SGI registers — per
qemu-9.2.4 hw/intc/arm_gicv3_cpuif.c `gicv3_irqfiq_access`, TC also traps the IRQ/FIQ
priority group: **ICC_PMR_EL1, ICC_CTLR_EL1, ICC_RPR_EL1**. Our EC=0x18 handler treated
every trapped sysreg as ICC_SGI1R_EL1, so it SILENTLY DROPPED the guest's PMR/CTLR
writes (and injected garbage SGIs from their values). A secondary whose ICC_PMR_EL1
write is lost runs with the wrong priority mask → masks its own interrupts → wedges.

FIX (vmexit.c handle_sysreg_trap):
- Decode the trapped register from the ISS (op0/op1/crn/crm/op2).
- ICC_SGI*R_EL1 (crn=12,crm=11,op2∈{5,6,7}) → relay as IPI (vgic_sgi_trap), write-only.
- ICC_PMR_EL1 → emulate via ICH_VMCR_EL2.VPMR[31:24] (the guest's VIRTUAL priority mask
  lives there; must NOT touch the physical ICC_PMR_EL1, which is EL2's own and breaks
  the hypervisor's IRQ handling — confirmed: forwarding to physical regressed cpu0 boot).
- ICC_CTLR_EL1 → EOImode<->VMCR.VEOIM, CBPR<->VMCR.VCBPR.
- ICC_RPR_EL1 (RO) → return idle priority 0.

VERIFIED: make clean zero-warning, entry 0x40080000, .text @0x40080000. `make run`
prints "CPU1: Booted secondary processor ..." and "smp: Brought up 1 node, 2 CPUs" /
"SMP: Total of 2 processors activated.", reaches busybox `~ #`. Deterministic: 3/3 runs.

---
# (historical) Slice 4+5 diagnosis log — nproc=2 not yet reached

## What works (verified by run, build clean -Werror, entry 0x40080000)
- Guest DTS has `cpu@1` (reg=0x1, psci); GICR reg widened to 0x40000 (2 RDs). DTB recompiled.
- Per-CPU vGICR emulation: `g_vgicr[NR_CPUS]`, `vgicr_typer(cpu)` sets Aff + Last-on-last,
  handler routes by `offset / VGICR_STRIDE`. (`vgic_v3_mmio.c/.h`)
- Real PSCI CPU_ON handler (`psci_cpu_on_guest` in psci.c): maps affinity->idx, rejects
  out-of-range with INVALID_PARAMETERS, authors vcpu[idx] regs, fires physical CPU_ON,
  spins on online -> SUCCESS. Wired into `psci_handle`.
- Temp slice-2 probe removed from main.c; slice-3 smoke stub removed from secondary_main
  (now enters guest with PSCI-authored regs).
- SGI/IPI path (vgic_sgi.c/.h): traps ICC_SGI1R_EL1 (ICH_HCR_EL2.TC=1 in vgic_init),
  EC=0x18 decode in handle_exit (vmexit.c), per-vCPU pending bitmap under sgi_lock,
  physical kick-SGI (INTID 15), drain+inject (LR2) on target via el2_irq_handler.
  Kick-SGI now enabled on BOTH cpu0 (gic_init) and cpu1 (secondary_main) GICRs.
- printk spinlock (print.c) — de-interleaves cross-core output.
- vtimer + PL011 inject sites switched to current_vcpu()/current_vcpu_id().

## The stall (run13.log)
Boot reaches `smp: Bringing up secondary CPUs ...`, then:
```
[hv] pCPU1 online, entering guest
[hv] IRQ cpu1 intid=27        <- CPU1 takes its vtimer PPI exactly ONCE
[hv] SGI trap: cpu1 vintid=2 sgi1r=0x2000001   <- CPU1 sends its "alive" IPI to cpu0
[hv] SGI drain: cpu0 pend=0x4                   <- cpu0 drains+injects vintid=2 into LR2
```
Then silence. No "Booted secondary processor", no nproc=2. CPU1 takes only ONE timer
IRQ then makes no further progress.

## Leading hypotheses (next iteration)
1. **CPU1 vtimer stops after 1 fire.** Timer is HW-forwarded into LR0, priority-dropped
   not deactivated (ADR-0001). If the guest on CPU1 never completes the virtual EOI/
   deactivate handshake (because it's stuck earlier), the physical line stays Active and
   never re-fires. Check: is CPU1's guest actually running secondary_start_kernel, or
   stuck before local_irq_enable?
2. **The drained SGI on cpu0 (LR2) may not reach the guest.** cpu0 injects vintid=2 into
   ICH_LR2_EL2 directly during the IRQ handler, but if cpu0's vcpu_run/vgic_restore path
   or an LR collision drops it, Linux cpu0 never sees CPU1's completion IPI -> __cpu_up
   times out. Verify LR2 actually presents (check ICH_ELRSR/guest takes the vSGI).
3. **GICR_TYPER affinity/Last encoding** may not match what Linux expects for cpu1's
   redistributor probe (Aff in [63:32]? Processor_Number?). If Linux can't associate
   cpu1 with its redistributor it won't enable cpu1's local timer correctly.

## Iteration 2 findings (run15/run16)
- CPU1 DOES progress through GIC init: traced cpu1 MMIO shows it reads GICR frame 0
  TYPER (0x80a0008), **frame 1 TYPER (0x80c0008 — its own redistributor, found OK)**,
  GICD TYPER (0x8000004), then sends IPI. So per-CPU vGICR emulation + Last/affinity
  encoding WORK; CPU1 finds its redistributor.
- PSCI CPU_ON authored vcpu1 entry=0x414b5490 ctx=0 (looks correct).
- CPU1 sends INTID=2 to TargetList bit0 (cpu0) — `sgi1r=0x2000001`.
- **The cross-core SGI inject on cpu0 is CORRECT:** after drain, cpu0
  `ICH_LR2_EL2=0x50a0000000000002` = State=Pending, prio=0xA0, vINTID=2;
  `ICH_ELRSR=0xa` (LR0,LR2 occupied); `ICH_VMCR=0x4c000a` (VENG1=1 set);
  `ICH_HCR=0x401` (En|TC). So the vSGI is properly pending in an LR with Group1 enabled.
- Yet boot still hangs with no further cpu1 activity after its one IPI, and no
  "Booted secondary processor" / nproc=2.

## Narrowed suspects (iteration 3)
A. **CPU0 guest never consumes the pending vSGI.** VMCR VPMR byte [31:24] read as 0x00
   at drain time (would mask the 0xA0 SGI). If the guest's virtual PMR stays low, the
   IPI never gets taken. BUT cpu0 boots fine normally — verify VPMR over time / whether
   the guest raises it. Confirm cpu0 actually takes the vSGI (does it later EOI/DIR it?).
B. **CPU1 stalls after its single IPI** waiting for a timer that fires once then stops
   (vtimer left Active per ADR-0001, guest never completes virtual deactivate because it
   is blocked). CPU1 took intid=27 exactly once (run13).
C. Linux __cpu_up uses a shared-memory completion (not an IPI) + the secondary calling
   notify_cpu_starting. If CPU1 wrote cpu_online but CPU0's poll doesn't see it, suspect
   a Stage-2 cache/coherency or the shared page mapping. (Less likely — same Stage-2.)

## Iteration 2 CONCLUSION — root cause localized to CPU1 vtimer (suspect B confirmed)
run17 per-cpu IRQ trace: **CPU1 takes intid=27 (vtimer) EXACTLY ONCE**, sends one IPI
(vintid=2 → cpu0), then takes NO further interrupts of any kind. cpu1 IRQ count = 1.
Cross-core SGI delivery to cpu0 is proven correct (LR2 pending, VENG1 set — iter 2 above),
so the IPI path is NOT the blocker. The blocker is: **CPU1's virtual timer fires once
and never re-fires**, so CPU1's idle loop never advances and it never finishes coming
online → no "Booted secondary processor", no nproc=2.

This is an ADR-0001-class issue specific to the secondary core: the vtimer PPI is
HW-forwarded into LR0 and only priority-dropped (left Active); re-fire depends on the
guest deactivating the virtual timer (HW=1 LR linkage releases the physical). On cpu1
that handshake is not completing/repeating.

### Next-iteration plan (start here)
1. Trace cpu1's vtimer deactivate: does cpu1's guest ever write ICC_DIR_EL1 / EOI the
   vtimer? Add a trace in gic_deactivate / on cpu1's LR0 EOI. If never, the physical
   timer stays Active and cannot re-pend.
2. Check CNTHCTL_EL2 / vtimer arming on cpu1 in secondary_main vs vm cpu0 path — is the
   guest able to read CNTPCT and re-program CNTV_CVAL on cpu1? (EL1PCTEN/EL1PCEN = 0x3
   is set, matches cpu0.)
3. Compare to cpu0: cpu0's timer re-fires every tick. Diff the cpu0 vs cpu1 vtimer/LR0
   handling. The el2_irq_handler is now shared (current_vcpu()), so the asymmetry is in
   GICR/timer enable or the Active-state release, not the inject.
4. Candidate fix: ensure the secondary's vtimer PPI Active state is cleared correctly,
   or that ICH_LR0 HW linkage on cpu1 deactivates physical INTID 27 on guest EOI.

## Iteration 3 — ROOT CAUSE FOUND (vtimer fires before guest vGIC ready)
At the instant cpu1 takes its first vtimer PPI (intid=27):
- `CNTV_CTL=0x5` (ENABLE+ISTATUS, IMASK=0) — guest enabled the PHYSICAL vtimer
- `ICH_VMCR_EL2=0x4c0008` — decode: **VENG1=0** (bit1), **VPMR=0x00** (bits[31:24])
  → the guest's VIRTUAL GIC Group-1 interface is NOT enabled yet
- cpu0 for comparison: VMCR=0x4c000a → VENG1=1 (enabled)
- `ICH_HCR_EL2=0x401` (En|TC) ok; LR0=0 before inject (first timer)

So: **CPU1's guest enables the physical vtimer (CNTV_CTL) BEFORE it enables its
virtual GIC CPU interface (VENG1/VPMR).** The vtimer PPI fires into EL2, we inject
HW=1 into LR0 and priority-drop (leave Active per ADR-0001), but the guest CANNOT
take the vIRQ (VENG1=0, VPMR=0 masks all), so it never runs the timer ISR, never
deactivates vINTID 27 → physical PPI 27 stuck Active forever → no more timers →
CPU1 idles forever. nproc stays 1.

Confirmed the line is genuinely asserted: experiment forcing gic_deactivate on cpu1's
timer produced a 393511-IRQ storm (exactly the ADR-0001 storm), and still no progress —
because each deactivate re-pends before the guest (VENG1 still 0) runs an instruction.

### THE FIX (next step)
The premature vtimer must not get wedged Active while the guest's vGIC is not ready.
Options, in order of preference:
1. **Mask the physical vtimer PPI at cpu1's redistributor until the guest enables
   VENG1.** i.e. don't ISENABLE PPI 27 in secondary_main; instead enable it lazily
   when the guest first enables its virtual interface, OR keep it disabled and rely on
   the guest's own redistributor ISENABLER write (trapped via vGICR) to gate it. On
   cpu0 the guest's GIC init enables PPI 27 *after* VENG1, so cpu0 never hits this.
   → CHECK: does cpu0's secondary_main-equivalent (gic_init) pre-enable PPI 27, or does
     the guest? gic_init DOES pre-enable it (mmio_write32 ISENABLER0 ppi). But cpu0
     still works because cpu0's guest enables VENG1 early in boot before arming CNTV.
2. **On a vtimer PPI with VENG1==0 (guest interface not ready): do NOT leave Active —
   mask PPI 27 at the GICR, and re-enable it when the guest enables VENG1.** Needs a
   trap on the guest enabling its interface (ICC_IGRPEN1 is a sysreg the guest writes;
   not currently trapped). Heavier.
3. **Simplest robust fix:** in secondary_main, do NOT enable the vtimer PPI (27) on
   cpu1's redistributor up front. Let the guest's own vGICR ISENABLER0 write (which we
   trap-and-emulate, and which currently only updates shadow state) ALSO program the
   physical GICR enable for PPI 27 on that cpu. That way PPI 27 is only physically
   enabled once the guest's GIC driver enables it — by which point VENG1 is on.
   → Inspect vgicr_write_sgi ISENABLER0 path; make it mirror PPI 27 enable to the
     physical GICR for the current cpu. This is the most architecturally faithful.

Pursue option 3 first; fall back to a targeted mask/unmask if it regresses cpu0.

## Iteration 3b — lazy-enable fix attempt + deeper finding
Implemented "don't pre-enable vtimer PPI 27 on cpu1; enable it physically only when
the guest enables virtual PPI 27 (mirror in vgicr_write_sgi ISENABLER0)". Result:
cpu1 timer count = 0 (premature fire gone) BUT still no secondary boot. And cpu1's
MMIO trace shows it NEVER writes ISENABLER0 — it only READS:
  0x80affe8 (GICR f0 ~PIDR), 0x80a0008 (f0 TYPER), 0x80c0008 (f1 TYPER), 0x8000004
  (GICD TYPER) — then STOPS. No WAKER write, no ISENABLER write.

So CPU1 stalls in EARLY per-cpu GIC init, right after reading GICD_TYPER, BEFORE
enabling any interrupt. In the earlier (wedged-timer) runs it got past this only
because the one premature timer tick advanced a timed-wait (udelay/calibration) in
CPU1's bringup — i.e. **CPU1 needs working timer ticks to progress through early
bringup, but the timer wedges because the guest hasn't enabled VENG1 yet.**

Chicken-and-egg: guest needs timer to reach the code that enables its vGIC interface,
but the HW-forwarded timer gets stuck Active if it fires while VENG1=0.

### The real fix (next)
The vtimer must be deliverable during the window where the guest's virtual interface
is not yet up, WITHOUT wedging. Cleanest robust option:
- **Software-inject the vtimer (HW=0) instead of HW=1, and on each EL2 timer tick
  mask the physical PPI via CNTV_CTL.IMASK (set bit1) at EL2, re-checking.** i.e. EL2
  drives the physical timer: on a tick, set IMASK to silence the physical line
  (no Active wedge, no storm), inject a SOFTWARE vIRQ (HW=0) into an LR; the guest's
  virtual EOI just clears the LR (no physical deactivate needed). Re-arm by clearing
  IMASK when re-entering the guest. This removes the HW-linkage dependency entirely
  for the secondary and avoids both the storm and the wedge.
  CAUTION: must not regress cpu0 (keep cpu0 on the HW=1 path, or move both to the
  software+IMASK model and re-verify cpu0 boots).
- Alternative: trap the guest's CNTV_CTL/IGRPEN1 to sequence timer-enable after
  vGIC-enable. Heavier (more sysreg traps).

Recommend: switch the vtimer path to **software injection + CNTV IMASK gating** for
ALL cpus and re-verify cpu0 still boots, then cpu1. This is the standard KVM-style
"EL2 owns the physical vtimer, injects a virtual timer IRQ" model and sidesteps the
HW-linkage edge cases that only bite on the secondary.

## Iteration 3c — vtimer wedge FIXED; deeper blocker remains (CPU1 does zero GIC writes)
Implemented the chosen fix (VENG1-gated vtimer):
- el2_irq_handler vtimer branch: if guest VENG1=1 → HW=1 forward + leave Active (cpu0
  steady state, ADR-0001). If VENG1=0 → software-inject (HW=0) + deactivate + MASK
  physical PPI 27 at this cpu's GICR (gic_ppi_set_enable(cpu,27,false)). No more storm.
- vgicr_write_sgi ISENABLER0: when guest enables virtual PPI 27, re-enable physical
  PPI 27 on that cpu's GICR (gic_ppi_set_enable(cpu,27,true)). Re-arm half.
- New gic_ppi_set_enable(cpu,intid,enable) in gic_v3.c/.h.
Build clean (-Werror, entry 0x40080000). The timer storm/wedge is gone.

BUT nproc=2 still not reached. Decisive new evidence (run23): CPU1 does exactly
**6 MMIO READS and ZERO WRITES** to the GIC, then sends one IPI (vintid=2), then stalls:
  0x80affe8 (f0 ~PIDR2), 0x80a0008 x2 (f0 TYPER), 0x80c0008 x2 (f1 TYPER), 0x8000004
  (GICD TYPER). It NEVER writes GICR_WAKER, never writes ISENABLER → so the vtimer
  re-enable never triggers either. CPU1 bails out of / stalls during redistributor
  probing, BEFORE waking its redistributor or enabling any interrupt.

My vgicr_typer(cpu1) = 0x0000000100000110 (low32=0x110: Last=1,ProcNum=1; high32=0x1
=affinity). CPU1 reads only TYPER low (offset 0x8), never high (0xC). Suspect the
redistributor affinity match in gic_populate_rdist: if Linux reads TYPER as 2x32-bit
and compares the HIGH word (affinity) but never reads 0xC, OR if our 64-bit TYPER read
path mis-delivers, CPU1 may fail to associate its rdist and bail (would normally print
"CPU1: mpidr X has no re-distributor" — but that print may be lost/not flushed).

### NEXT (iteration 4) — focus on the redistributor probe, not the timer
1. Trace the SIZE of CPU1's TYPER reads (8 vs 4) and what value vgicr_read_rd returns
   for frame1 TYPER. Confirm CPU1 actually reads the affinity (high word). If it reads
   64-bit at offset 8, ensure the mmio bus delivers size=8 and vgicr_read_rd returns the
   full 64-bit typer (currently returns u32! — vgicr_read_rd returns u32, so a 64-bit
   TYPER read may be TRUNCATED, losing the affinity in the high word!). 
   >>> STRONG LEAD: vgicr_read_rd returns u32; the 64-bit GICR_TYPER affinity (high
   >>> word) is likely truncated → CPU1 can't match its redistributor → bails. CHECK
   >>> the mmio handler's handling of size==8 reads for VGICR_TYPER on frame 1.
2. Compare: cpu0 matches affinity 0 = 0x0 (high word 0), so truncation doesn't hurt
   cpu0 — but cpu1 needs high word = 0x1, which truncation drops. THIS likely explains
   why cpu0 works and cpu1 fails the rdist match.

## Iteration 4 — BIG WIN: 64-bit GICR_TYPER truncation bug fixed
**Root bug:** vgicr_read_rd() returned u32, so a 64-bit GICR_TYPER read was truncated
to its low 32 bits, DROPPING the Affinity_Value in bits[63:32]. cpu0 (affinity 0) was
unaffected; cpu1 (affinity 1, in the high word) could not match its redistributor in
Linux's gic_populate_rdist and bailed → did ZERO GIC writes → never came up.
**Fix:** vgicr_read_rd now returns u64 and returns the full vgicr_typer(cpu) for a
64-bit TYPER read (low word only for a 32-bit read; high word for the +4 read).

Result (run24/run25): CPU1 now does ~21+ GIC WRITES (was 0): wakes its redistributor,
writes ISENABLER0 (0x80d0100) enabling its PPIs incl. the vtimer PPI 27, configures
ICFGR, and sends the "secondary online" IPI (vintid=1 → cpu0). The VENG1-gated vtimer
re-enable also fires correctly ("cpu1 guest enabled vtimer PPI -> phys re-enable").
HUGE progress — CPU1 boots through its whole GIC init and signals online.

### Remaining gap (iteration 5)
CPU1 still takes 0 vtimer IRQs (count27=0) after enabling PPI 27, and boot still hangs
before "Booted secondary processor". Two leads:
1. The guest on cpu1 may not have ARMED its CNTV yet (blocked in the online handshake
   with cpu0) — so the physical timer never reaches its comparator. Check: does cpu1
   ever write CNTV_CVAL/CTL after the IPI? (needs a trap or trace; CNTV access doesn't
   trap currently). 
2. The cpu0<->cpu1 online completion handshake: cpu1 sent vintid=1, cpu0 drained+injected
   it (LR2 pending, VENG1=1). Verify cpu0's guest actually TAKES that IPI and completes
   __cpu_up (marks cpu1 online), letting cpu1 proceed to arm its timer / enter idle.
   If cpu0 never consumes the injected SGI, both sides wait forever.
   → Trace whether cpu0 takes a vSGI (intid 0/1) after the drain, or add a cpu0 IRQ trace.

This is now likely the LAST blocker: the cross-core online handshake completing so both
CPUs proceed. The TYPER fix is a clean, isolated, COMMITTABLE bug fix on its own.

## Iteration 5 — spinlock WFE race fixed; final blocker = guest cross-core handshake
Two more fixes this iteration:
- **spinlock.h**: the ticket lock used ldaxr+WFE / plain-store+SEV, which has a
  WFE/event-register race that can wedge a waiter forever. Replaced with a plain
  busy-wait using __atomic acquire/release (ldxr/stxr ticket draw, yield spin). Tiny
  critical sections + 2 cores → simple spin is correct and race-free.
- All debug printks removed (they were under sgi_lock/print_lock and perturbed timing).

Current behavior (run28, clean build, no traces): boot still hangs at
"smp: Bringing up secondary CPUs" right after "[hv] pCPU1 online, entering guest".
cpu0's PSCI CPU_ON DID return (we see the online print, set before guest entry), so
BOTH cores are in the GUEST (EL1) — the deadlock is now the GUEST's cross-core online
handshake, not a hypervisor spin.

IMPORTANT timing clue: WITH the debug printks, interrupts flowed (cpu0 took kick
intid=15, cpu1 took timer intid=27, IPIs exchanged) and it got further; WITHOUT them
it freezes harder. That strongly implies a **memory-ordering / coherency** issue in the
cross-core path — the printks added barriers/delays that let the handshake progress.

### Fixes landed this session (all real, isolated, COMMITTABLE):
1. 64-bit GICR_TYPER truncation (vgicr_read_rd u32→u64) — THE big one; unblocked all of
   CPU1's GIC init. cpu1 now wakes its rdist, enables PPIs, sends IPIs.
2. VENG1-gated vtimer (no wedge/storm on the secondary) + gic_ppi_set_enable re-arm.
3. spinlock WFE/SEV race → plain __atomic busy-wait.

### Final blocker (iteration 6) — guest cross-core handshake / coherency
Leads, in priority:
1. **Memory ordering of percpu[].online and the guest's shared online_mask.** The
   secondary writes online with dmb ish; verify the Stage-2 maps guest RAM as
   Normal Inner-Shareable Write-Back CACHEABLE (not Non-cacheable / Device). If guest
   RAM is mapped with wrong shareability/cacheability, cross-core writes aren't
   coherent → the spin-wait handshake (smp_cond_load) never observes the update.
   → CHECK stage2.c memory attributes for the guest RAM block: must be
     MemAttr=Normal WB, SH=Inner-shareable. This is the #1 suspect given the
     "printks (barriers) help" clue.
2. The guest's secondary releases from its pen via a write the boot CPU polls; ensure
   no EL2 caching of that line. Tied to #1.
3. Verify CNTVOFF/timebase identical so the guest's udelay calibration on cpu1 matches.

>>> START iteration 6 at stage2.c guest-RAM block attributes (SH + MemAttr). The
>>> "barriers help" symptom is the classic signature of a missing inner-shareable
>>> cacheable mapping for SMP guest memory.

## Iteration 6 — confirmed: timing-sensitive RACE; CPU1 stuck taking ~0 exits
Raw lock-free uart tracing (uart_putc, bypasses printk lock) in cpu1's vcpu_run loop:
- One run: cpu1 prints ONE 'R' (enters guest) and ZERO 'r' (never returns) → stuck on a
  single guest instruction, no exits at all (likely WFE waiting for a wakeup).
- Another run (with the extra uart delays): 'rRErrr' → cpu1 took several exits, timer
  ENABLED (E), no mask (M) → progressed further.
=> **NON-DETERMINISTIC**: outcome changes when raw-uart delays are added. This is a
   genuine concurrency RACE, not a deterministic logic bug.

Ruled OUT this iteration:
- PSCI EL2 online-spin deadlock: making CPU_ON return immediately did NOT fix it.
- Stage-2 attributes: guest RAM is already Normal-WB Inner-Shareable (stage2.c l1[1]
  = S2_MEMATTR_NORM | S2_SH_ISH) — correct for SMP coherency.
- spinlock: replaced WFE/SEV with __atomic busy-wait (still hangs, so not the cause,
  but the fix is correct and worth keeping).

Most likely remaining cause (iteration 7): CPU1's guest sits in WFE waiting for a
cross-core wakeup (SGI/IPI or a watched memory write) that races. Candidates:
1. The kick-SGI → drain → inject path has a window where the wakeup is lost if it
   races with cpu1 entering WFE (lost-wakeup: pending bit set + kick fires while target
   is between checking the flag and executing WFE). Classic lost-wakeup. FIX: ensure the
   target re-checks the pending bitmap after WFE, or that the physical kick stays pending
   so WFE returns. (A physical SGI made Pending before the target WFEs should wake it —
   verify the kick isn't being deactivated/dropped before delivery.)
2. cpu0's injected vSGI (LR2) may not actually be taken by cpu0's guest (priority/active
   state), so cpu0 never completes __cpu_up. Re-verify cpu0 consumes intid 0/1.

### iteration 7 START HERE
Add HCR_EL2.TWE=1 on BOTH vcpus to TRAP WFE to EL2 (EC=0x01). Then in handle_exit,
on a WFE trap from cpu1: log it (raw uart) and just return (re-enter guest). This both
(a) proves CPU1 is spinning in WFE, and (b) breaks a lost-wakeup by turning WFE into a
poll. If trapping WFE makes nproc=2 work, the bug is a lost cross-core wakeup and the
real fix is to make the kick-SGI delivery wakeup-safe.

### iteration 6b RESULT: WFE-trap did NOT fix it (reverted)
Set HCR_EL2.TWE (bit14) on both vcpus + no-op WFE trap (EC=0x01) in handle_exit. Boot
STILL hangs identically at "smp: Bringing up secondary CPUs" / "pCPU1 online, entering
guest". If CPU1 were spinning in WFE, trapping it would have produced heavy poll activity
and likely unblocked it — it did neither. So **CPU1 is NOT in a simple WFE lost-wakeup.**
Reverted (TWE on cpu0 also adds overhead). 

=> The stall is something else: either CPU1 hangs on a single non-WFE instruction
(tight poll on a memory flag with no exits), or CPU0 is the blocked party (e.g. cpu0's
guest stuck waiting in __cpu_up while cpu1 actually died/faulted silently). 

### iteration 7 (revised) START HERE — find WHERE each core's guest PC is
The decisive missing datum: the GUEST PC (ELR_EL2) of EACH vcpu while hung. Approach:
- Arm a periodic EL2 entry on cpu0 (its timer still works) and from there print BOTH
  vcpu[0].regs.elr_el2 and vcpu[1].regs.elr_el2 (the saved guest PCs) via raw uart.
  If vcpu1's PC is constant → cpu1 is spin/looping at that PC; disassemble the guest
  Image at that address to see what it waits on. If vcpu1's PC advances → cpu1 lives
  and cpu0 is the stuck one.
- Cross-check: is cpu1 maybe taking a SILENT fault? Add a raw-uart marker in the
  handle_exit default/park case AND in any abort path, per-cpu. Earlier the "unexpected
  exit EC=..." path parks in wfi — if cpu1 hit that, it would be silent without a flushed
  printk. Put a uart_putc in that park loop.
- Also verify cpu1's vcpu[1].regs were actually authored correctly (elr=entry) by the
  PSCI handler AND not clobbered before first run.

This is a genuine hard SMP race/hang. 3 real bugs already fixed; the cross-core
bring-up handshake completion is the lone remaining blocker for nproc=2.

## Iteration 7 — ROOT CAUSE FULLY CHARACTERIZED (cross-core IPI-wait deadlock)
Exhaustive raw-uart tracing (lock-free, non-perturbing) established the exact mechanism:

- **No silent fault:** added a raw '!cEE' marker in handle_exit's park/default path —
  NEVER fires. Neither core hits the EL2 fault handler. Both are live in the guest.
- **cpu0 returns from PSCI cleanly:** markers `P( S )` = enter CPU_ON, smc SUCCESS, saw
  percpu[1].online, returned. So cpu0 is NOT stuck in the hypervisor.
- **cpu1 is very much alive and gets FAR:** its sync-exit EC sequence is
  20×`<24>`(MMIO GIC init) + 9×`<18>`(ICC_SGI1R writes) + 11×`<24>`(MMIO) + 1×`<18>`,
  then stops. So cpu1 completes its redistributor/distributor init and sends IPIs.
- **The IPIs decode correctly:** raw SGI1R values from cpu1 end with `0x000001000001`
  = INTID 1, TargetList 0x1 (cpu0). My SGI path relays it: `k0` (kick sent to cpu0) →
  `K0` (cpu0's EL2 received the physical kick) → cpu0 drains + injects vINTID 1 into its
  own LR. The cross-core SGI plumbing WORKS end to end.
- **Then BOTH freeze.** cpu0 stops taking even its own vtimer (only 1 PC sample after
  online), meaning cpu0's guest is busy-waiting with IRQs DISABLED.

### THE DEADLOCK (definitive)
cpu1, late in secondary_start_kernel, issues an IPI (INTID 1) to cpu0 and **spins
waiting for cpu0 to handle it** (e.g. csd_lock_wait / a sync cross-call). cpu0 is
simultaneously busy-waiting in __cpu_up for cpu1 with **IRQs masked at EL1** (it stops
taking its own timer). The injected vINTID-1 sits Pending in cpu0's LR but the guest
never takes it because PSTATE.I is set. → cpu0 never services the IPI → cpu1 never
proceeds → cpu1 never sets cpu_online → cpu0 never exits __cpu_up. Mutual wait.

On real hardware this does not deadlock because the boot CPU's wait path and the
secondary's online sequence are ordered so the secondary sets cpu_online via COHERENT
MEMORY (not needing the boot CPU to take an IPI first). The fact that it deadlocks here
points at an ordering/visibility issue OR a genuinely Linux-version-specific bring-up
path that issues a synchronous cross-call during secondary bring-up.

### Candidate fixes (iteration 8 — needs design thought, not rapid trial)
1. **Verify cpu1 actually sets cpu_online in coherent memory and that cpu0 observes it.**
   If cpu1 is blocked BEFORE set_cpu_online (in the IPI-wait), the real question is why
   it does a synchronous IPI there. Inspect the guest kernel's secondary path
   (Linux 6.12 arch/arm64/kernel/smp.c secondary_start_kernel) for what runs before
   notify_cpu_starting/set_cpu_online and whether it cross-calls cpu0.
2. **Make cpu0's injected IPI deliverable despite EL1 IRQ-masking** — not possible by
   spec (PSTATE.I gates it). So the fix must be on the ordering side, not delivery.
3. **Re-examine whether the 9 earlier `<18>` IPIs (SGI1R 0x80, 0xf0, 0x0 — odd
   TargetLists) are being DROPPED wrongly**, causing the guest to retry/stall. These had
   INTID 0 and TargetList 0x80/0xf0 (cpus 7/4-7, nonexistent) — likely Linux probing or
   a misdecode. If they are real IPIs to cpu0 that we drop, cpu0 misses early signals.
   WORTH CHECKING: is INTID/TargetList extraction correct for ALL of cpu1's IPIs, or only
   the last? The 0x80/0xf0 values look wrong for a 2-cpu system — investigate SGI1R
   field extraction (Aff1 vs TargetList) for those.

>>> iteration 8 START: investigate lead #3 (are cpu1's early IPIs mis-decoded/dropped?)
>>> then lead #1 (guest secondary path ordering). This is the lone remaining blocker.

## DEBUG TRACES — all removed; tree builds clean (-Werror, entry 0x40080000)
(none remain)

## Uncommitted files (build clean; nproc=2 NOT yet reached — do not commit as "done")
DTS cpu@1 + GICR 0x40000; per-CPU vGICR + 64-bit TYPER fix; PSCI CPU_ON handler;
secondary.c guest entry; SGI path (vgic_sgi.c/.h); spinlock busy-wait; printk lock;
VENG1-gated vtimer + gic_ppi_set_enable. ADR work (task 17) still pending.
The 64-bit TYPER fix is independently correct and could be split out as its own commit.
- psci.c: "PSCI CPU_ON: vcpu%u entry=..." 
- mmio.c: "cpu%u MMIO ipa=..." (cpu!=0)
- irq_handler.c: "cpu%u IRQ intid=..." (cpu!=0)
- vgic_sgi.c: "SGI trap", "SGI drain", "drain cpu%u LR2=... ELRSR/VMCR/HCR"
All gated to cpu1 or SGI events; build is clean (-Werror, entry 0x40080000). NOT committed.

## Commits so far (NOT committed: this slice4+5 work is uncommitted, build clean)
- 34e7dcf slice1, 85d298c slice2, bfd3abc slice3
- ADR work (task 17) still pending; do it with the slice-4 commit once nproc=2 passes.
