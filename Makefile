# SPDX-License-Identifier: TBD

ARCH          ?= arm64
BOARD         ?= qemu_virt
CROSS_COMPILE ?= aarch64-none-linux-gnu-

ifeq ($(shell which $(CROSS_COMPILE)gcc 2>/dev/null),)
    export PATH := /home/corsair/Downloads/toolchain/arm-gnu-toolchain-14.2.rel1-x86_64-aarch64-none-linux-gnu/bin:$(PATH)
endif

CC      := $(CROSS_COMPILE)gcc
LD      := $(CROSS_COMPILE)ld
OBJCOPY := $(CROSS_COMPILE)objcopy

BUILD_DIR ?= build
OBJ_DIR   := $(BUILD_DIR)/obj
ELF       := $(BUILD_DIR)/hypervisor.elf
BIN       := $(BUILD_DIR)/hypervisor.bin

HV_GUEST ?= linux

CFLAGS := \
    -ffreestanding -nostdlib -nostartfiles \
    -fno-pic -fno-stack-protector \
    -fno-strict-aliasing \
    -mgeneral-regs-only -mstrict-align \
    -Wall -Wextra -Werror -O2 -g

ifeq ($(HV_GUEST),svm)
CFLAGS += -DCONFIG_GUEST_SVM=1
else ifeq ($(HV_GUEST),svm_dual)
CFLAGS += -DCONFIG_GUEST_SVM=1 -DCONFIG_NR_VMS=2
else ifeq ($(HV_GUEST),linux)
CFLAGS += -DCONFIG_NR_VMS=2
else
$(error HV_GUEST must be 'linux', 'svm' or 'svm_dual')
endif

ASFLAGS := -g

include hypervisor/Makefile
include hypervisor/arch/$(ARCH)/Makefile

INCLUDES := $(hv-includes) $(arch-includes)

# .config required for all targets except defconfig/clean/help/menuconfig
ifeq ($(filter defconfig clean help menuconfig,$(MAKECMDGOALS)),)
ifeq ($(wildcard .config),)
$(error .config not found. Run 'make defconfig' first.)
endif
CONFIG_DEFS := $(shell \
    sed -n 's/^CONFIG_\([A-Za-z0-9_]*\)=y$$/-DCONFIG_\1=1/p' .config)
endif

CFLAGS  += $(CONFIG_DEFS) $(INCLUDES)
ASFLAGS += $(CONFIG_DEFS) $(INCLUDES)

ALL_OBJS  := $(addprefix $(OBJ_DIR)/, $(hv-objs) $(arch-objs))
LD_SCRIPT := $(arch-ldscript)

SVM_CFLAGS := -ffreestanding -nostdlib -nostartfiles -fno-pic -fno-pie \
              -Wall -Wextra -Werror -O2 -g
SVM_ELF    := $(BUILD_DIR)/svm/svm.elf
SVM_BIN    := $(BUILD_DIR)/svm/svm.bin
SVM2_ELF   := $(BUILD_DIR)/svm2/svm2.elf
SVM2_BIN   := $(BUILD_DIR)/svm2/svm2.bin
SVM3_ELF   := $(BUILD_DIR)/svm3/svm3.elf
SVM3_BIN   := $(BUILD_DIR)/svm3/svm3.bin
SVM4_ELF   := $(BUILD_DIR)/svm4/svm4.elf
SVM4_BIN   := $(BUILD_DIR)/svm4/svm4.bin

HOST_CC    := cc

.PHONY: all run clean defconfig menuconfig help svm svm2 svm3 svm4 check-offsets \
	test-svm-build test-svm-dual-build test-qemu test-qemu-svm2 test-qemu-svm3 \
	test-qemu-svm4 test guest

.NOTPARALLEL: test

all: $(ELF) $(BIN)

$(BIN): $(ELF)
	$(OBJCOPY) -O binary $< $@

$(ELF): $(ALL_OBJS) $(LD_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD) -T $(LD_SCRIPT) --no-warn-rwx-segments -o $@ $(ALL_OBJS)

$(OBJ_DIR)/%.o: hypervisor/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/%.o: hypervisor/%.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c -o $@ $<

$(SVM_ELF): tests/svm/svm_main.c tests/svm/svm.lds
	@mkdir -p $(dir $@)
	$(CC) $(SVM_CFLAGS) -T tests/svm/svm.lds -o $@ $<

$(SVM_BIN): $(SVM_ELF)
	$(OBJCOPY) -O binary $< $@

svm: $(SVM_BIN)

$(SVM2_ELF): tests/svm2/svm2_main.c tests/svm2/svm2_vectors.S tests/svm2/svm2.lds
	@mkdir -p $(dir $@)
	$(CC) $(SVM_CFLAGS) -T tests/svm2/svm2.lds -o $@ \
	      tests/svm2/svm2_main.c tests/svm2/svm2_vectors.S

$(SVM2_BIN): $(SVM2_ELF)
	$(OBJCOPY) -O binary $< $@

svm2: $(SVM2_BIN)

$(SVM3_ELF): tests/svm3/svm3_main.c tests/svm3/svm3_vectors.S tests/svm3/svm3.lds
	@mkdir -p $(dir $@)
	$(CC) $(SVM_CFLAGS) -T tests/svm3/svm3.lds -o $@ \
	      tests/svm3/svm3_main.c tests/svm3/svm3_vectors.S

$(SVM3_BIN): $(SVM3_ELF)
	$(OBJCOPY) -O binary $< $@

svm3: $(SVM3_BIN)

$(SVM4_ELF): tests/svm4/svm4_main.c tests/svm4/svm4.lds
	@mkdir -p $(dir $@)
	$(CC) $(SVM_CFLAGS) -T tests/svm4/svm4.lds -o $@ tests/svm4/svm4_main.c

$(SVM4_BIN): $(SVM4_ELF)
	$(OBJCOPY) -O binary $< $@

svm4: $(SVM4_BIN)

DTC        ?= dtc
GUEST_DTS  := guest/qemu_virt.dts
GUEST_DTB  := $(BUILD_DIR)/guest/guest.dtb

$(GUEST_DTB): $(GUEST_DTS)
	@command -v $(DTC) >/dev/null 2>&1 || { \
	    echo "ERROR: '$(DTC)' not found. Install it:"; \
	    echo "  Debian/Ubuntu: sudo apt-get install device-tree-compiler"; \
	    exit 1; }
	@mkdir -p $(dir $@)
	$(DTC) -I dts -O dtb -o $@ $<

guest: $(GUEST_DTB)

$(BUILD_DIR)/check_offsets: tests/check_offsets.c
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -o $@ $<

check-offsets: $(BUILD_DIR)/check_offsets
	$(BUILD_DIR)/check_offsets

$(BUILD_DIR)/check_offsets_target.o: tests/check_offsets_target.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

check-offsets-target: $(BUILD_DIR)/check_offsets_target.o
	@echo "PASS: cross-compiled struct offsets match assembly macros"

TEST_BUILD_DIR := build/test-svm

test-svm-build:
	$(MAKE) BUILD_DIR=$(TEST_BUILD_DIR) HV_GUEST=svm all svm svm2 svm3

TEST_DUAL_BUILD_DIR := build/test-svm-dual

test-svm-dual-build:
	$(MAKE) BUILD_DIR=$(TEST_DUAL_BUILD_DIR) HV_GUEST=svm_dual all svm svm4

test-qemu-svm4: test-svm-dual-build
	HYPERVISOR_ELF=$(TEST_DUAL_BUILD_DIR)/hypervisor.elf \
	SVM_BIN=$(TEST_DUAL_BUILD_DIR)/svm/svm.bin \
	SVM_BIN2=$(TEST_DUAL_BUILD_DIR)/svm4/svm4.bin sh tests/run_svm4_test.sh

test-qemu: test-svm-build
	LINUX_IMAGE= HYPERVISOR_ELF=$(TEST_BUILD_DIR)/hypervisor.elf \
	SVM_BIN=$(TEST_BUILD_DIR)/svm/svm.bin sh tests/run_svm_test.sh

test-qemu-svm2: test-svm-build
	LINUX_IMAGE= HYPERVISOR_ELF=$(TEST_BUILD_DIR)/hypervisor.elf \
	SVM_BIN=$(TEST_BUILD_DIR)/svm2/svm2.bin sh tests/run_svm2_test.sh

test-qemu-svm3: test-svm-build
	LINUX_IMAGE= HYPERVISOR_ELF=$(TEST_BUILD_DIR)/hypervisor.elf \
	SVM_BIN=$(TEST_BUILD_DIR)/svm3/svm3.bin sh tests/run_svm3_test.sh

test: check-offsets check-offsets-target test-qemu test-qemu-svm2 test-qemu-svm3 test-qemu-svm4

run: $(ELF) $(GUEST_DTB)
	./scripts/run-qemu.sh

clean:
	rm -rf $(BUILD_DIR)

defconfig:
	cp configs/$(BOARD)_defconfig .config
	@echo "Wrote .config from configs/$(BOARD)_defconfig"

menuconfig:
	@echo "menuconfig reserved for a later milestone; edit .config by hand."
	@false

help:
	@echo "Targets: all run clean defconfig"
	@echo "Vars:    ARCH=$(ARCH) BOARD=$(BOARD) CROSS_COMPILE=$(CROSS_COMPILE)"
