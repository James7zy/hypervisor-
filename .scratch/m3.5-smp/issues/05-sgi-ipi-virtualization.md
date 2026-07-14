# SGI / IPI virtualization (concurrency lands last)

Status: ready-for-agent

## Parent

Plan: `docs/superpowers/plans/2026-06-23-m3.5-smp.md` — Slice 5.
Design: `docs/superpowers/specs/2026-06-23-m3.5-smp-design.md`.

## What to build

Let the guest send inter-processor interrupts between its two vCPUs by trapping
its SGI generation, relaying it as a physical kick-SGI to the target pCPU, and
injecting the virtual SGI on arrival. This is the only slice that introduces
real cross-core concurrency, which is why it lands last.

- Trap `ICC_SGI1R_EL1`: set the trap-control bit per vCPU (`ICH_HCR_EL2.TC`) and
  add the sysreg-write decode in the EL2 trap path, extracting the target
  affinity list and the virtual INTID.
- Maintain a per-vCPU pending-SGI bitmap (16 SGIs), with all writes guarded by a
  dedicated SGI spinlock.
- On the `ICC_SGI1R` trap: set the target vCPU's pending bits, then issue a
  **physical** kick-SGI (`BOARD_KICK_SGI`) to the target pCPU via the real
  `ICC_SGI1R_EL1`.
- On the target core's EL2 IRQ handler: add the kick-SGI branch that drains the
  pending bitmap under the SGI spinlock, injects each pending virtual INTID into
  this core's LR, then priority-drops and **deactivates** the kick-SGI. Also
  switch the existing hard-coded `&g_vm.vcpu` injection sites to `current_vcpu()`.
- Guard `printk`/UART output with the spinlock to stop cross-core log
  interleaving.

ADR work: document the SGI kick path (trap → physical SGI → drain/inject) in the
superseding per-CPU/SMP ADR introduced in slice 04.

## Acceptance criteria

- [ ] `ICC_SGI1R_EL1` writes trap to EL2 and are decoded (target list + vINTID).
- [ ] Per-vCPU pending-SGI bitmap exists; all access is under the SGI spinlock.
- [ ] Trap path relays a physical kick-SGI to the target pCPU.
- [ ] Target core drains the bitmap, injects each vINTID, and deactivates the kick-SGI.
- [ ] Existing injection sites use `current_vcpu()` rather than `&g_vm.vcpu`.
- [ ] `printk`/UART output is spinlock-guarded.
- [ ] Strong (non-blocking) verification: `/proc/interrupts` IPI counts grow; two `yes > /dev/null &` loads advance both pCPUs' EL2 tick counters.
- [ ] ADR updated to document the SGI kick path.
- [ ] `make` zero warnings; `nproc` still 2 and the guest still boots.

## Blocked by

- `.scratch/m3.5-smp/issues/04-guest-driven-cpu-on.md`
