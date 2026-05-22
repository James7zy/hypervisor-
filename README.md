# hypervisor

A learning/research ARM hypervisor inspired by ACRN, Xvisor, and Xen.

## Status

**M0 — Hello EL2** (current milestone). The hypervisor enters EL2 on QEMU
virt and prints a banner. No guest support yet.

See [the M0 design](docs/superpowers/specs/2026-05-21-hypervisor-m0-design.md).

## Quickstart

```sh
make defconfig
make
make run
```

Expected output:

```
[hv] Hello from EL2 on qemu_virt, CurrentEL=0x8
```

Exit QEMU with `Ctrl-A x`.

## Requirements

- `aarch64-linux-gnu-gcc` >= 10
- `aarch64-linux-gnu-binutils`
- `qemu-system-aarch64` >= 6.0

## Acceptance checklist (M0)

- [ ] `make` builds cleanly with zero warnings.
- [ ] `aarch64-linux-gnu-objdump -h build/hypervisor.elf` shows `.text` at `0x40080000`.
- [ ] `aarch64-linux-gnu-readelf -h build/hypervisor.elf` shows entry == `0x40080000`.
- [ ] `make run` prints the banner within 3 seconds.
- [ ] Banner's `CurrentEL` reads `0x8`.
- [ ] Temporarily flipping the EL assertion in `head.S` to demand EL3
      triggers the `!EL` early-panic path. Revert after verifying.
- [ ] `Ctrl-A x` exits QEMU cleanly.

## License

TBD — see `LICENSE`.
