# Guest initramfs (user-supplied) — busybox rootfs for both VMs

Both Linux VMs use the same small root filesystem. Like the kernel `Image`,
the initramfs archive is **supplied by you, not built in this repo**. The
maintained `/init` source is `guest/initramfs-init.sh`.
This doc is a reference recipe for producing a `cpio.gz` initramfs containing a
static busybox and an `/init` that drops to an interactive shell on the guest
console.

## What the hypervisor expects

- A gzip-compressed cpio (newc format) archive — a standard Linux initramfs.
- Set `LINUX_INITRD`; `make run` loads the same archive at PA `0x84000000`
  for VM0 and `0xC4000000` for VM1.
- Both guest DTBs advertise `linux,initrd-start = <0x44000000>` and
  `linux,initrd-end = <0x45000000>` in `/chosen` (guest IPA). Keep the payload
  under 16 MB.
- The kernel unpacks it and runs `/init`, which reads `hv.vm=` into `PS1`.

## Address sanity

| Artifact | Guest IPA (both VMs) | VM0 PA | VM1 PA |
|---|---|---|---|
| RAM base | `0x40000000` | `0x80000000` | `0xC0000000` |
| Image | `0x40080000` | `0x80080000` | `0xC0080000` |
| DTB | `0x42000000` | `0x82000000` | `0xC2000000` |
| initramfs | `0x44000000` | `0x84000000` | `0xC4000000` |

The initramfs is 64 MB above RAM base and 32 MB above the DTB. The Image must
fit below the DTB (~31 MB budget). `guest/linux-6.12-qemu_virt.config` is a
known-good full Linux `.config`, not a savedefconfig.

## Recipe (host tools — needs a static busybox binary)

You need a statically-linked arm64 `busybox`. Either download a prebuilt
static aarch64 busybox, or build it from the upstream source:

```sh
# build a static arm64 busybox from the upstream git
git clone --depth=1 https://git.busybox.net/busybox
cd busybox
make ARCH=arm64 CROSS_COMPILE=aarch64-none-linux-gnu- defconfig
# Enable static linking non-interactively (equivalent to ticking
# "Build static binary (no shared libs)" in `make menuconfig`):
sed -i 's/# CONFIG_STATIC is not set/CONFIG_STATIC=y/' .config
make ARCH=arm64 CROSS_COMPILE=aarch64-none-linux-gnu- -j"$(nproc)"
file busybox   # must be: ELF ... ARM aarch64 ... statically linked
cd ..
```

> **实测(2026-06-19):** 用上游 `https://git.busybox.net/busybox` 的 master
> (`git clone --depth=1`,当时是 **busybox 1.39.0**)交叉编译,用的是和内核同一
> 套工具链 `aarch64-none-linux-gnu-` 14.2。产物 `./busybox` 约 2.1 MB,打成的
> initramfs 约 1.2 MB。**必须静态链接**(`CONFIG_STATIC=y`):initramfs 里没有
> 动态链接器(`ld-linux-aarch64.so`)和共享库,动态版的 busybox 一进去就因找不到
> 解释器而失败,到不了 shell。注意 host 自带的 `/usr/bin/busybox` 通常是 x86-64
> 的,guest(arm64)用不了——必须自己编一个 aarch64 的。

Assemble the initramfs tree:

```sh
mkdir -p initramfs/{bin,sbin,proc,sys,dev,etc}
cp busybox/busybox initramfs/bin/busybox
( cd initramfs && ln -sf busybox bin/sh )

# Use the repository's maintained init, including per-VM prompts.
cp /path/to/hypervisor-repo/guest/initramfs-init.sh initramfs/init
chmod +x initramfs/init

# Pre-create the common applet symlinks:
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

Expected: both kernels unpack the archive and run `/init`. The banner keeps
its historical `M3.4` label; the prompt identifies the VM, for example:

```
=== M3.4: busybox rootfs up — vm0 interactive shell ===
vm0:~ #
```

Use `Ctrl-T`, then `vm_console 1`, to select VM1 input and run `ls /` or
`cat /proc/cpuinfo` there too. The per-VM DTBs differ only in `hv.vm=0/1`;
this changes the prompt, not the hostname.

## Console note (must match the DTB bootargs)

`console=ttyAMA0` and `earlycon=pl011,0x9000000` address the **emulated PL011**.
EL2 owns the physical UART; guest MMIO traps into its per-VM vuart. Focus
controls RX only, so output from either VM remains visible.

There is no current EL2 virtio-console backend. Switching bootargs to `hvc0`
will not create one; virtio backends belong to the planned M14 userspace DM.
Run through `make run`, which exports the board's CPU count to the QEMU script.
