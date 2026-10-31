# SPDX-License-Identifier: TBD
hv-objs := \
    boot/main.o \
    lib/string.o \
    lib/print.o \
    common/vm/vm.o \
    common/vm/vm_configs.o \
    common/vm/vcpu_state.o \
    common/percpu.o \
    common/io/mmio.o \
    dm/vuart.o \
    dm/console.o \
    dm/hv_shell.o

# One public header directory. Module-private headers must be included with
# quotes from their own directory, so nothing outside a module (arch/ included)
# can reach into it by search path.
hv-includes := \
    -Ihypervisor/include
