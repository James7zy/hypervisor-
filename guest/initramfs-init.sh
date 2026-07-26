#!/bin/sh
# Guest initramfs /init.
#
# Kept in-tree so the initramfs is reproducible: the .cpio.gz itself lives
# outside the repo (it is a build artifact of a busybox rootfs), but this is
# the only file in it that is ours. To rebuild:
#
#   mkdir ir && cd ir
#   zcat /path/to/initramfs.cpio.gz | cpio -idm
#   cp /path/to/guest/initramfs-init.sh init && chmod +x init
#   find . | cpio -o -H newc | gzip -9 > /path/to/initramfs-vmid.cpio.gz
#
mount -t proc     none /proc
mount -t sysfs    none /sys
mount -t devtmpfs none /dev 2>/dev/null || /bin/busybox mdev -s

# Identify which VM this is from the hv.vm= token the hypervisor's per-VM DTB
# puts in bootargs (see guest/qemu_virt.dts). Both VMs run the SAME Image and
# initramfs over one shared EL2 console, so without this they render an
# identical `~ #` prompt and there is no way to see which one the EL2 shell's
# `vm_console <n>` just attached you to.
#
# The prompt carries the identity rather than the hostname: the busybox this
# was built against ships only 11 applets and has no `hostname`.
VM=0
for arg in $(cat /proc/cmdline); do
    case "$arg" in
        hv.vm=*) VM="${arg#hv.vm=}" ;;
    esac
done

echo
echo "=== M3.4: busybox rootfs up — vm${VM} interactive shell ==="
export PS1="vm${VM}:\w # "
exec /bin/sh
