#/usr/bin/bash
set -x

LINUX_IMAGE=/home/corsair/Music/virtual/linux-6.12.93/arch/arm64/boot/Image \
LINUX_INITRD=/home/corsair/Music/virtual/initramfs.cpio.gz make run 
