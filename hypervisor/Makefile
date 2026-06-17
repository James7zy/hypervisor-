# SPDX-License-Identifier: TBD
hv-objs := \
    boot/main.o \
    debug/uart_pl011.o \
    lib/string.o \
    lib/print.o \
    common/vm/vm.o \
    common/psci/psci.o \
    dm/gpa.o \
    dm/virtqueue.o \
    dm/virtio_mmio.o \
    dm/virtio_console.o

hv-includes := \
    -Ihypervisor/include \
    -Ihypervisor/common/vm \
    -Ihypervisor/common/psci \
    -Ihypervisor/dm
