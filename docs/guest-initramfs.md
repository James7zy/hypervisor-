# Guest initramfs (user-supplied) — busybox rootfs for M3.4

The M3.4 "boot to shell" milestone needs a small root filesystem. Like the
kernel `Image`, the initramfs is **supplied by you, not built in this repo**.
This doc is a reference recipe for producing a `cpio.gz` initramfs containing a
static busybox and an `/init` that drops to an interactive shell on the guest
console.

## What the hypervisor expects

- A gzip-compressed cpio (newc format) archive — a standard Linux initramfs.
- Loaded by QEMU at **physical** address `0x4C000000` via
  `-device loader,file=<initramfs>,addr=0x4C000000` (set `LINUX_INITRD`).
- Advertised to the kernel by the guest DTB `/chosen`:
  `linux,initrd-start = <0x44000000>` and `linux,initrd-end = <0x45000000>`
  (guest IPA; PA = IPA + 0x08000000). Keep the payload under 16 MB.
- The kernel unpacks it as the rootfs and runs `/init`.

## Address sanity

| Artifact | Guest IPA | Physical (QEMU `addr=`) |
|---|---|---|
| initramfs | `0x44000000` | `0x4C000000` |

64 MB above guest RAM base (`0x40000000` / PA `0x48000000`); 32 MB above the
DTB; clear of the kernel at IPA `0x40080000`.

## Recipe (host tools — needs a static busybox binary)

You need a statically-linked arm64 `busybox`. Either download a prebuilt
static aarch64 busybox, or build it:

```sh
# (optional) build a static arm64 busybox
git clone --depth=1 https://git.busybox.net/busybox
cd busybox
make ARCH=arm64 CROSS_COMPILE=aarch64-none-linux-gnu- defconfig
# enable "Build static binary (no shared libs)" in: make menuconfig
make ARCH=arm64 CROSS_COMPILE=aarch64-none-linux-gnu- -j"$(nproc)"
# -> ./busybox  (static aarch64 ELF)
cd ..
```

Assemble the initramfs tree:

```sh
mkdir -p initramfs/{bin,sbin,proc,sys,dev,etc}
cp busybox/busybox initramfs/bin/busybox
( cd initramfs && ln -sf busybox bin/sh )

cat > initramfs/init <<'EOF'
#!/bin/sh
mount -t proc     none /proc
mount -t sysfs    none /sys
mount -t devtmpfs none /dev 2>/dev/null || \
  /bin/busybox mdev -s
echo
echo "=== M3.4: busybox rootfs up — interactive shell ==="
# console=ttyAMA0 makes the PL011 the controlling /dev/console.
exec /bin/sh
EOF
chmod +x initramfs/init

# install the busybox applet symlinks at boot via the init above (mdev/ln),
# or pre-create the common ones:
for a in mount umount ls cat echo sh mdev cpio; do
  ln -sf busybox initramfs/bin/$a
done
```

Pack it into a `cpio.gz`:

```sh
( cd initramfs && find . | cpio -o -H newc | gzip -9 ) > initramfs.cpio.gz
ls -l initramfs.cpio.gz   # should be well under 16 MB
```

## Run

```sh
LINUX_IMAGE=/path/to/Image \
LINUX_INITRD=/path/to/initramfs.cpio.gz \
make run
```

Expected: after the M3.0–M3.3 boot, the kernel logs
`Unpacking initramfs...`, runs `/init`, prints the banner, and an interactive
busybox prompt appears:

```
=== M3.4: busybox rootfs up — interactive shell ===
/ #
```

Type `ls /` or `cat /proc/cpuinfo` and see output — that is the M3.4 DoD.

## Console note (must match the DTB bootargs)

This repo's `guest/qemu_virt.dts` sets `console=ttyAMA0` (the passed-through
PL011), so the shell runs on `ttyAMA0` and `/init`'s `exec /bin/sh` lands on
`/dev/console` = the PL011. If you switch the guest to the M3.3 virtio-console
(`console=hvc0` in `bootargs`), make `/init` target `/dev/hvc0` instead, e.g.:

    exec setsid sh -c 'exec sh </dev/hvc0 >/dev/hvc0 2>&1'

Keep `earlycon=pl011` either way — virtio-console is not available during early
boot.
