# M3 boot verification — first real QEMU run (2026-06-19)

M3.0–M3.4 were marked **done on static verification only**; the live `make run`
boot was deferred as an "operator handoff" (see
`docs/superpowers/plans/2026-06-16-m3.0-linux-alive.md` line 573) and **never
actually executed end-to-end**. This is the first real run. It exposed bugs that
static inspection could not.

## Environment assembled for the run

- Toolchain: Arm GNU `aarch64-none-linux-gnu-` 14.2 (`source /home/corsair/Downloads/toolchain.sh`).
- Guest kernel: `~/Music/virtual/linux-6.12.93`, arm64 `defconfig` **trimmed**
  (disabled NET/PCI/USB/DRM/DEBUG_INFO/… kept PL011, virtio-mmio/console,
  devtmpfs, initramfs) so `Image` is **28 MB** — see "address budget" below.
- initramfs: static aarch64 busybox (built from git, `CONFIG_STATIC=y`) packed
  per `docs/reference/guest-initramfs.md` → `~/Music/virtual/initramfs.cpio.gz` (1.2 MB).

## Bug 1 — Stage-2 L1 1 GB block mis-aligned (FIXED)

`hypervisor/arch/arm64/mmu/stage2.c` mapped guest RAM with a single **L1 1 GB
block**, whose output PA must be 1 GB-aligned:

```c
l1_table[1] = (ram_pa & 0xFFFFC0000000UL) | ...;
```

`ram_pa` was `BOARD_LINUX_RAM_PA = 0x48000000` — only 128 MB-aligned. The mask
silently rounded it **down to `0x40000000`**, the hypervisor's own image. The
guest at IPA `0x40080000` therefore executed the hv's `head.S`, hit
`panic_early`, and emitted `!EL`. Instruction trace confirmed the guest's first
op was `mrs x9, mpidr_el1` (hv `_start`), not the Linux Image header.

ADR-0004 even encoded the false premise ("`ram_pa` (`0x4800_0000`) is [1 GB-aligned]").

**Fix (chosen: keep the single 1 GB block, move RAM to a 1 GB-aligned PA):**
`board.h` `BOARD_LINUX_RAM_PA 0x48000000 → 0x80000000` (and IMAGE_PA, DTB_PA,
initrd PA shifted to the `0x80000000` base); `scripts/run-qemu.sh` loader
addresses updated to `0x80080000 / 0x82000000 / 0x84000000`. Guest **IPA** layout
(RAM `0x40000000`, DTB `0x42000000`, initrd `0x44000000`) is unchanged, so the
guest DTS needs no edit. After the fix the `!EL` is gone and the guest runs real
Linux head.S.

## RESULT: boots to interactive busybox shell ✅

After bugs 1 and 2 below were fixed, the committed `scripts/run-qemu.sh` boots an
unmodified arm64 Linux 6.12.93 all the way to the busybox shell — the headline M3
goal. Serial evidence:

```
[    0.000000] Booting Linux on physical CPU 0x0000000000 [0x410fd083]
[    0.000000] earlycon: pl011 at MMIO 0x0000000009000000
[    1.702448] Run /init as init process
=== M3.4: busybox rootfs up — interactive shell ===
~ #
```

The prompt `~ #` is reached and is interactive. **Known follow-up bug 3** (below):
typing into the shell currently crashes the hypervisor.

## Bug 2 — guest Image fetch external-abort: RAM moved past end of DRAM (FIXED)

Fixing bug 1 moved `BOARD_LINUX_RAM_PA` to `0x80000000` for 1 GB-alignment, but
QEMU `virt` DRAM starts at `0x40000000`, so `-m 1G` makes RAM end *exactly* at
`0x80000000` — the new RAM base is the first byte **past** physical memory. The
guest's first instruction fetch at IPA `0x40080000` → PA `0x80080000` hit
unbacked memory and took a synchronous external abort (`ESR 0x86000010`, IFSC
`0x10`, FAR `0x40080000`); with the guest's VBAR_EL1 still 0 it double-faulted to
`0x200` and trapped to EL2 (the `EC=0x20 ELR=0x200` exit originally observed).

**Fix:** `scripts/run-qemu.sh` now uses **`-m 2G`** (RAM `0x40000000`–`0xBFFFFFFF`),
so PA `0x80080000` is backed. Documented inline in the script.

Trade-off note: bug 1's chosen fix (1 GB-aligned RAM base, single L1 block)
*forces* `>1 GB` of guest DRAM. The alternative fix (descend Stage-2 to L2 2 MB
blocks) would have allowed keeping RAM at `0x48000000` inside 1 GB. Revisit if a
1 GB guest is ever required.

## Bug 3 — IRQ taken at EL2 in the `daifclr`→`eret` window → panic_vector (FIXED)

At the live shell, typing crashed the hv (`!VEC`). Root cause, from `-d int`:

```
Taking exception 5 [IRQ]  ...from EL2 to EL2   ← IRQ taken while AT EL2
...with ELR 0x40082fb0                          ← in el1_irq_handler_asm tail
...to EL2 PC 0x40080a80                          ← hv_vectors + 0x280 (Cur-EL IRQ)
```

Both `vcpu_run` and `el1_irq_handler_asm` did `msr daifclr, #2` **one
instruction before `eret`**. A physical IRQ pending in that window is taken **at
EL2**, where `vectors.S` routes the Current-EL IRQ entry to `panic_vector`
(EL2 has no current-EL IRQ handler). The vtimer fires constantly, so the window
was hit as soon as a second IRQ (PL011 RX from a keystroke) raised contention.
(The original `!VEC ESR=0x5a000000` was a stale ESR_EL2 printed by panic_vector;
the true ESR was `0x56000000`, EC=IRQ.)

The `daifclr` was unnecessary: with `HCR_EL2.IMO=1`, a physical IRQ taken while
the guest runs at EL1 goes to EL2 regardless of the guest's `PSTATE.I` (it is
gated only by EL2's own `PSTATE.I`, which does not apply at EL1). So the timer
PPI is still delivered via the Lower-EL IRQ vector (+0x480) after the `eret`.

**Fix:** remove `msr daifclr, #2` before `eret` in both `vmexit_asm.S`
(`vcpu_run`) and `irq_handler_asm.S`. Verified: 40-keystroke and multi-command
stress runs, zero crashes.

## Interactive input (ttyAMA0 passthrough) — made to work alongside the bug-3 fix

With the crash gone, typing still produced no echo: (a) `vm_run` called
`virtio_console_rx_poll()` → `uart_getc()`, draining the PL011 RX FIFO the guest
needs (the guest console is `ttyAMA0`, not `hvc0`); and (b) the PL011 SPI (INTID
33) was never enabled at the physical GICD nor injected into the guest, so the
guest's UART ISR never ran.

Fixes:
- `vm.c`: drop the `virtio_console_rx_poll()` call from the run loop (it is only
  for a `console=hvc0` guest and otherwise steals PL011 input).
- `gic_v3.c` + `board.h` + `gic_v3.h`: enable PL011 SPI 33 in the physical GICD
  (Group 1, prio 0xA0, routed to CPU0).
- `irq_handler.c`: on INTID 33, inject into the guest vGIC and priority-drop only
  (leave Active — the LR.HW linkage gates re-pend).
- `vgic.c`: `vgic_inject_spi` now **HW-forwards** into **LR1** (LR0 is owned by
  the constantly-re-injected vtimer; HW=1 lets the guest's deactivate release the
  physical line so a level-sensitive RX line does not stay Active after one byte).

Verified — full interactive session:

```
~ # ls /
bin   dev   etc   init  proc  root  sbin  sys
~ # echo HELLO_FROM_GUEST
HELLO_FROM_GUEST
~ # uname -m
aarch64
~ # cat /proc/uptime
14.58 12.21
```

## Bug 2 (original symptom, superseded by the analysis above) — instruction abort to PC=0x200

After bug 1, the guest runs ~174 blocks of real Linux head.S then traps once:

```
[hv] unexpected exit EC=0x20 ESR=0x8200000d ELR=0x200
```

- EC=0x20 = instruction abort from lower EL; IFSC `0x0d` = translation fault L1.
- Guest PC = `0x200` — branched into the low (unmapped-as-RAM) IPA region; `0x200`
  is the sync/SP_ELx vector offset, so the guest most likely took an exception and
  vectored through a VBAR that does not resolve to mapped RAM.
- **No earlycon output** appeared before the fault, i.e. it dies earlier than the
  M3.0 DoD ever claimed. The hv's `default` exit handler does not re-enter on an
  instruction abort, so the guest then hangs.

Not yet root-caused. Likely suspects to chase next:
1. Image-vs-vmlinux offset: guest IPA `0x40080000` is the **Image** load addr; to
   correlate trace PCs, disassemble the raw `Image` (or account for the 64-byte
   header + EFI `nop` sled), not `vmlinux` `_text` directly.
2. The guest enabling its Stage-1 MMU and branching to a kernel VA whose
   Stage-2-translated IPA is outside the single RAM block.
3. CNTVOFF / timer or an HCR_EL2 trap re-entry corrupting early state.

## Makefile gap noted in passing

`make` does **not** track header dependencies (no `.d` files). Editing `board.h`
did not trigger recompilation of the `.o` files that include it — a stale relink
kept the old `ram_pa` and printed `ram_pa=0x48000000` until `make clean`. Any
header-only change currently requires `make clean`.

## Repro (single QEMU invocation, non-interactive)

```sh
source /home/corsair/Downloads/toolchain.sh
make clean && make && make guest
timeout 80 qemu-system-aarch64 \
  -machine virt,virtualization=on,gic-version=3 -cpu cortex-a72 -smp 1 -m 1G \
  -nographic -serial mon:stdio -kernel build/hypervisor.elf \
  -device loader,file=$HOME/Music/virtual/linux-6.12.93/arch/arm64/boot/Image,addr=0x80080000 \
  -device loader,file=build/guest/guest.dtb,addr=0x82000000 \
  -device loader,file=$HOME/Music/virtual/initramfs.cpio.gz,addr=0x84000000 \
  </dev/null
# add `-d in_asm,cpu -D /tmp/trace.log` for an instruction trace.
```
