---
name: m3-never-booted-in-qemu
description: M3 was marked done on static checks only; first real QEMU run (2026-06-19) needed 3 fixes — now boots to an interactive busybox shell that accepts input
metadata:
  type: project
---

CLAUDE.md and the milestone table claim **M3 (boot UP Linux to a busybox shell)
done**, but M3.0–M3.4 were only **statically verified** (build clean + readelf).
The live `make run` boot was deferred as an "operator handoff" (M3.0 plan line
573) and **never actually run end-to-end** until 2026-06-19.

First real QEMU run found two bugs (see `docs/debug/m3-boot-verification.md`):
1. **FIXED** — Stage-2 L1 1 GB block mis-aligned: `ram_pa=0x48000000` is not
   1 GB-aligned, the `& 0xFFFFC0000000` mask rounded it to `0x40000000` (the hv
   image), so the guest executed hv code and printed `!EL`. Fix: moved
   `BOARD_LINUX_RAM_PA` to `0x80000000` (1 GB-aligned) + matching loader addrs.
   ADR-0004 had encoded the false "0x48000000 is 1 GB-aligned" premise — needs
   updating.
2. **FIXED** — bug 1's fix put RAM at PA 0x80000000, but QEMU virt `-m 1G` ends
   DRAM exactly there, so the guest Image fetch external-aborted. Fix: run script
   now uses `-m 2G` so PA 0x80080000 is backed.
3. **FIXED** — typing crashed the hv: a physical IRQ taken in the `msr
   daifclr,#2`→`eret` window runs AT EL2, whose current-EL IRQ vector is wired
   to `panic_vector`. The `daifclr` was unnecessary (HCR_EL2.IMO delivers the
   IRQ from EL1 anyway). Removed it in `vcpu_run` + `el1_irq_handler_asm`.
   Plus made input actually work: stopped `vm_run` stealing PL011 RX via
   `uart_getc`, enabled PL011 SPI 33 in the physical GICD, and HW-forward-inject
   it into the guest via vGIC **LR1** (LR0 is the vtimer's).

**Result:** the committed `scripts/run-qemu.sh` boots unmodified Linux 6.12.93 to
an **interactive** busybox shell — `ls /`, `echo`, `uname -m`, `cat /proc/uptime`
all run, zero crashes under keystroke stress. The headline M3 goal is now fully
observed.

**How to build/run:** trimmed arm64 defconfig kernel (28 MB Image, fits the
0x40080000→DTB budget) + static busybox initramfs:
`LINUX_IMAGE=~/Music/virtual/linux-6.12.93/arch/arm64/boot/Image LINUX_INITRD=~/Music/virtual/initramfs.cpio.gz make run`.
**Why:** the milestone "done" was static-only; don't trust un-run "done" flags.
See [[toolchain-setup]], [[toolchain-no-header-deps]].
