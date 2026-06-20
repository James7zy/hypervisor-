---
name: toolchain-setup
description: How to get the aarch64 cross-toolchain on PATH to build this hypervisor
metadata:
  type: project
---

The aarch64 cross-toolchain is NOT on the default PATH. Before any `make`, source the setup script:

    source /home/corsair/Downloads/toolchain.sh

It exports `CROSS_COMPILE=aarch64-none-linux-gnu-`, `ARCH=arm64`, and prepends
`/home/corsair/Downloads/toolchain/arm-gnu-toolchain-14.2.rel1-x86_64-aarch64-none-linux-gnu/bin`
to PATH (Arm GNU Toolchain 14.2.Rel1, gcc 14.2.1).

Bash-tool shell state does NOT persist between calls, so source it inline in the
same command as the build, e.g.:
`source /home/corsair/Downloads/toolchain.sh && make`

Caveats:
- `dtc` (device-tree-compiler) is NOT installed. M3.1–M3.3 don't need it; M3.4 (DTB initrd) might — install `device-tree-compiler` via apt if a DTB rebuild is required.
- `qemu-system-aarch64` is at /usr/local/bin (present). No prebuilt Linux `Image` is available in this env, so `make run` / boot-to-shell DoD steps are operator handoffs. See [[m3-execution-plan]].
