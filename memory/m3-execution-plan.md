---
name: m3-execution-plan
description: How the M3.1–M3.4 milestones are being executed (subagents, tags, sequencing)
metadata:
  type: project
---

User asked (2026-06-18) to implement the remaining M3 plans via subagents, one tag per phase, respecting dependencies.

Decisions:
- M3.1 → M3.2 → M3.3 → M3.4 run STRICTLY SEQUENTIALLY (not parallel): each phase builds on the prior, and M3.2 deletes M3.1's MMIO scaffold. One subagent per phase.
- Lightweight git tags after each verified phase: `m3.1`, `m3.2`, `m3.3`, `m3.4`. The MAIN agent creates the tag (not the subagent) only after the subagent reports success — keeps tagging gated on verified completion.
- No prebuilt Linux `Image` in this env → each agent implements + statically verifies (build clean with -Werror, `readelf -h` entry point must stay `0x40080000`) and STOPS at the `make run` DoD step as an operator handoff, exactly as each plan specifies.

Plans live in `docs/superpowers/plans/2026-06-16-m3.{1,2,3,4}-*.md`. Toolchain: see [[toolchain-setup]].

Branch at start: m3.0-linux-alive. Existing tags: m0 (only).

STATUS (2026-06-18): ALL FOUR DONE and tagged — m3.1 (31a61fe), m3.2 (05d59da, removed M3.1 scaffold + real vGICv3), m3.3 (b82ffe1, virtio-console), m3.4 (74c0c90, initramfs + initrd DTB nodes). Each independently re-verified by the orchestrator (clean -Werror build, entry 0x40080000) before tagging. Two M3.4 subagents died mid-run (one user-killed, one API error) and were resumed from the last committed task — no rework, no broken commits.

REMAINING (operator handoff, blocked by env): no prebuilt arm64 Image, no initramfs, and `dtc` not installed. So the runtime boot-to-shell DoD was NOT observed here. To verify: `apt-get install device-tree-compiler`; `make guest` (regenerates guest.dtb with initrd nodes); supply LINUX_IMAGE + LINUX_INITRD (busybox cpio.gz per docs/guest-initramfs.md); `LINUX_IMAGE=… LINUX_INITRD=… make run`. Address map (uniform PA = IPA + 0x08000000): Image PA 0x48080000/IPA 0x40080000, DTB PA 0x4A000000/IPA 0x42000000, initrd PA 0x4C000000/IPA 0x44000000 (matches dts linux,initrd-start=0x44000000); guest RAM IPA 0x40000000 backed by PA 0x48000000, 256 MB.
