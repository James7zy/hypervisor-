# Drive the platform from compile-time board constants, not a parsed FDT

> **Status:** Accepted. **Milestone:** M3.0. (Will be revisited at M4.)

QEMU passes the DTB physical address in `x0` at entry, exactly as a real boot
chain would on ARM. A general hypervisor discovers its platform — RAM size,
device base addresses, interrupt routing — by parsing that flattened device tree
(or ACPI) at runtime. But the milestones up to M3.4 target exactly one fixed
machine, QEMU `virt`, whose memory map and device addresses are known at build
time. A full FDT parser is real work (libfdt-equivalent, property lookup,
`#address-cells`/`#size-cells` handling) with no consumer that needs it yet.

**Decision:** Treat compile-time `BOARD_*` constants as the single source of
truth for the platform layout, and deliberately **discard the boot-time DTB
pointer**. `head.S` still forwards `x0` to `hypervisor_main(uintptr_t dtb_phys)`,
but `hypervisor/boot/main.c` drops it with `(void)dtb_phys;`. Everything the
hypervisor needs comes from `hypervisor/arch/arm64/board/qemu_virt/board.h`
(`BOARD_UART_BASE`, `BOARD_GIC_DIST_BASE`, `BOARD_GIC_RDIST_BASE`,
`BOARD_LINUX_RAM_IPA`/`_PA`/`_SIZE`, `BOARD_LINUX_IMAGE_PA`,
`BOARD_LINUX_DTB_IPA`/`_PA`, …) and the derived `struct vm_config` in
`hypervisor/common/vm/vm_config.h`.

Note this is the hypervisor's *own* config; the *guest* still gets a DTB
(`BOARD_LINUX_DTB_IPA`) that QEMU loads and the boot protocol points the guest at.

## Considered Options

- **Compile-time `BOARD_*` constants (chosen)** — zero runtime code, no parser to
  debug, and the entire platform map is one readable header. Cost: the binary is
  welded to one machine; changing the target means editing and rebuilding, and
  the genuine `x0` DTB is thrown away.
- **Parse the FDT at boot** — rejected for now: the production answer and what M4
  will need, but it is substantial infrastructure that buys nothing on a single
  fixed QEMU target. Building it now would be speculative generality.

## Consequences

- **This is acknowledged technical debt, not a finished design.** The discarded
  `dtb_phys` is the visible marker: a reader seeing `(void)dtb_phys;` should know
  runtime discovery was a conscious omission, not an oversight.
- The board constants encode QEMU `virt` specifics (PL011 at `0x0900_0000`, GIC
  at `0x0800_0000`/`0x080A_0000`, RAM windows). They are the same constants the
  device passthrough/emulation split in [[0005-device-passthrough-vs-emulation]]
  and the Stage-2 map in [[0004-stage2-static-1gb-block-mapping]] depend on, so a
  retarget touches all three.
- **M4 (RK3588) will supersede this.** Real hardware needs runtime DT (or ACPI)
  discovery: RAM size and device addresses are not knowable at build time across
  boards, and the boot firmware genuinely hands over a DTB to honour. When that
  lands it should introduce a superseding ADR and begin actually consuming
  `dtb_phys`.
