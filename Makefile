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

BUILD_DIR := build
OBJ_DIR   := $(BUILD_DIR)/obj
ELF       := $(BUILD_DIR)/hypervisor.elf
BIN       := $(BUILD_DIR)/hypervisor.bin

CFLAGS := \
    -ffreestanding -nostdlib -nostartfiles \
    -fno-pic -fno-stack-protector \
    -mgeneral-regs-only -mstrict-align \
    -Wall -Wextra -Werror -O2 -g

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

SVM_CFLAGS := -ffreestanding -nostdlib -nostartfiles -Wall -Wextra -Werror -O2 -g
SVM_ELF    := $(BUILD_DIR)/svm/svm.elf
SVM_BIN    := $(BUILD_DIR)/svm/svm.bin

HOST_CC    := cc

.PHONY: all run clean defconfig menuconfig help svm check-offsets test-qemu test

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

$(BUILD_DIR)/check_offsets: tests/check_offsets.c
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -o $@ $<

check-offsets: $(BUILD_DIR)/check_offsets
	$(BUILD_DIR)/check_offsets

test-qemu: all svm
	SVM_BIN=$(SVM_BIN) sh tests/run_svm_test.sh

test: check-offsets test-qemu

run: $(ELF)
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
