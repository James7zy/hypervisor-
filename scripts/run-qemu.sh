#!/bin/sh
set -eu
: "${SVM_BIN:?SVM_BIN must point to the SVM binary, e.g.: SVM_BIN=/path/to/svm.bin make run}"
exec qemu-system-aarch64 \
  -machine virt,virtualization=on,gic-version=3 \
  -cpu cortex-a72 -smp 1 -m 1G \
  -nographic -serial mon:stdio \
  -kernel build/hypervisor.elf \
  -device loader,file="${SVM_BIN}",addr=0x40200000 \
  ${QEMU_EXTRA_ARGS:-}
