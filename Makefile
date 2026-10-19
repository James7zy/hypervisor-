# SPDX-License-Identifier: TBD

# The target architecture and board come from the config file, not from make
# variables: CONFIG_ARCH_<ARCH>=y and CONFIG_BOARD_<BOARD>=y name the
# directories hypervisor/arch/<arch>/ and hypervisor/arch/<arch>/board/<board>/.
# CONFIG defaults to .config (written by `make defconfig`); a sub-make may point
# it at a checked-in defconfig instead (see test-x86-stub).
CONFIG ?= .config

# Goals that run without a config (ARCH/BOARD then default for `help`/`clean`).
ifeq ($(filter defconfig clean help menuconfig,$(MAKECMDGOALS)),)
ifeq ($(wildcard $(CONFIG)),)
$(error $(CONFIG) not found. Run 'make defconfig' first.)
endif
cfg-arch  := $(shell sed -n 's/^CONFIG_ARCH_\([A-Z0-9_]*\)=y$$/\1/p' $(CONFIG) | tr 'A-Z' 'a-z')
cfg-board := $(shell sed -n 's/^CONFIG_BOARD_\([A-Z0-9_]*\)=y$$/\1/p' $(CONFIG) | tr 'A-Z' 'a-z')
ifneq ($(words $(cfg-arch)),1)
$(error $(CONFIG) must set exactly one CONFIG_ARCH_<arch>=y (found: $(cfg-arch)))
endif
ifneq ($(words $(cfg-board)),1)
$(error $(CONFIG) must set exactly one CONFIG_BOARD_<board>=y (found: $(cfg-board)))
endif
# A command-line ARCH=/BOARD= may only restate the config, never contradict it:
# a build whose flags and CONFIG_* macros disagree is worse than an error.
ifeq ($(origin ARCH),command line)
ifneq ($(ARCH),$(cfg-arch))
$(error ARCH=$(ARCH) contradicts $(CONFIG), which selects arch '$(cfg-arch)'. Pick the arch with CONFIG=<defconfig>)
endif
endif
ifeq ($(origin BOARD),command line)
ifneq ($(BOARD),$(cfg-board))
$(error BOARD=$(BOARD) contradicts $(CONFIG), which selects board '$(cfg-board)'. Pick the board with CONFIG=<defconfig>)
endif
endif
ARCH  := $(cfg-arch)
BOARD := $(cfg-board)
CONFIG_DEFS := $(shell \
    sed -n 's/^CONFIG_\([A-Za-z0-9_]*\)=y$$/-DCONFIG_\1=1/p' $(CONFIG))
else
ARCH  ?= arm64
BOARD ?= qemu_virt
endif

ifeq ($(ARCH),x86)
# Link-check stub only (ADR-0015): no runnable x86 board exists yet.
CROSS_COMPILE ?= x86_64-linux-gnu-
else
CROSS_COMPILE ?= aarch64-none-linux-gnu-

ifeq ($(shell which $(CROSS_COMPILE)gcc 2>/dev/null),)
    export PATH := /home/corsair/Downloads/toolchain/arm-gnu-toolchain-14.2.rel1-x86_64-aarch64-none-linux-gnu/bin:$(PATH)
endif
endif

CC      := $(CROSS_COMPILE)gcc
CPP     := $(CROSS_COMPILE)cpp
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

CFLAGS  += $(arch-cflags) $(CONFIG_DEFS) $(board-cflags) $(INCLUDES)
ASFLAGS += $(CONFIG_DEFS) $(board-cflags) $(INCLUDES)

# scripts/run-qemu.sh sizes -smp from this, so QEMU's pCPU count can never
# drift from the NR_CPUS the hypervisor was built with.
export HV_NR_CPUS := $(board-nr-cpus)

ALL_OBJS  := $(addprefix $(OBJ_DIR)/, $(hv-objs) $(arch-objs) $(board-drivers))
LD_SCRIPT := $(arch-ldscript)

SVM_CFLAGS := -ffreestanding -nostdlib -nostartfiles -fno-pic -fno-pie \
              -Wall -Wextra -Werror -O2 -g
# Bare-metal SVM test guests: one shared runtime, one binary per case
# (tests/svm/cases/<case>.c -> $(BUILD_DIR)/svm/svm-<case>.bin).
SVM_CASES  := basic vtimer vm1
SVM_BINS   := $(SVM_CASES:%=$(BUILD_DIR)/svm/svm-%.bin)
SVM_COMMON := tests/svm/svm_lib.c tests/svm/svm_vectors.S

HOST_CC    := cc

.PHONY: all run clean defconfig menuconfig help svm check-offsets check-arch-boundary \
	test-svm-build test-svm-dual-build test-qemu test-qemu-vtimer \
	test-qemu-dual test-qemu-shell test-x86-stub test guest

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

.SECONDARY: $(SVM_BINS:.bin=.elf)
svm: $(SVM_BINS)

$(BUILD_DIR)/svm/svm-%.elf: tests/svm/cases/%.c $(SVM_COMMON) tests/svm/svm_lib.h tests/svm/svm.lds
	@mkdir -p $(dir $@)
	$(CC) $(SVM_CFLAGS) -Wl,--no-warn-rwx-segments -T tests/svm/svm.lds \
	      -o $@ $< $(SVM_COMMON)

$(BUILD_DIR)/svm/svm-%.bin: $(BUILD_DIR)/svm/svm-%.elf
	$(OBJCOPY) -O binary $< $@

.PHONY: vspi test-qemu-vspi
VSPI_BINS := $(BUILD_DIR)/vspi/vspi-vm0.bin $(BUILD_DIR)/vspi/vspi-vm1.bin
.SECONDARY: $(VSPI_BINS:.bin=.elf)
vspi: $(VSPI_BINS)

$(BUILD_DIR)/vspi/vspi-vm%.elf: tests/vspi/vspi_main.c tests/vspi/vspi_entry.S tests/vspi/vspi_vectors.S tests/vspi/vspi.lds
	@mkdir -p $(dir $@)
	$(CC) $(SVM_CFLAGS) -mgeneral-regs-only -mstrict-align -fno-stack-protector \
	      -DVSPI_VM_ID=$* -T tests/vspi/vspi.lds -o $@ \
	      tests/vspi/vspi_main.c tests/vspi/vspi_entry.S tests/vspi/vspi_vectors.S

$(BUILD_DIR)/vspi/vspi-vm%.bin: $(BUILD_DIR)/vspi/vspi-vm%.elf
	$(OBJCOPY) -O binary $< $@

VSPI_BUILD_DIR ?= build/test-vspi
test-qemu-vspi:
	$(MAKE) BUILD_DIR=$(VSPI_BUILD_DIR) HV_GUEST=svm_dual all vspi
	HYPERVISOR_ELF=$(VSPI_BUILD_DIR)/hypervisor.elf \
	SVM_BIN=$(VSPI_BUILD_DIR)/vspi/vspi-vm0.bin \
	SVM_BIN2=$(VSPI_BUILD_DIR)/vspi/vspi-vm1.bin sh tests/run_vspi_test.sh

DTC        ?= dtc
GUEST_DTS  := guest/qemu_virt.dts
# One DTS, one DTB per VM: they differ ONLY in the hv.vm= bootargs token, which
# the initramfs init turns into a distinct prompt (guest/initramfs-init.sh).
# Without it both guests render an identical `~ #` on the shared console and
# there is no way to tell which VM the EL2 shell attached you to.
GUEST_DTB  := $(BUILD_DIR)/guest/guest.dtb
GUEST_DTB1 := $(BUILD_DIR)/guest/guest-vm1.dtb

# The DTS takes its whole bootargs value as one cpp string (dtc will not
# concatenate adjacent literals), so preprocess before dtc.
# $(1) = output DTB, $(2) = VM id baked into that VM's hv.vm= token.
GUEST_BOOTARGS_BASE := earlycon=pl011,0x9000000 console=ttyAMA0 nokaslr
define build_guest_dtb
	@command -v $(DTC) >/dev/null 2>&1 || { \
	    echo "ERROR: '$(DTC)' not found. Install it:"; \
	    echo "  Debian/Ubuntu: sudo apt-get install device-tree-compiler"; \
	    exit 1; }
	@mkdir -p $(dir $(1))
	$(CPP) -nostdinc -undef -x assembler-with-cpp \
	    -DHV_BOOTARGS='"$(GUEST_BOOTARGS_BASE) hv.vm=$(2)"' $(GUEST_DTS) \
	    | $(DTC) -I dts -O dtb -o $(1)
endef

$(GUEST_DTB): $(GUEST_DTS)
	$(call build_guest_dtb,$@,0)

$(GUEST_DTB1): $(GUEST_DTS)
	$(call build_guest_dtb,$@,1)

guest: $(GUEST_DTB) $(GUEST_DTB1)

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
	$(MAKE) BUILD_DIR=$(TEST_BUILD_DIR) HV_GUEST=svm all svm

TEST_DUAL_BUILD_DIR := build/test-svm-dual

test-svm-dual-build:
	$(MAKE) BUILD_DIR=$(TEST_DUAL_BUILD_DIR) HV_GUEST=svm_dual all svm

test-qemu: test-svm-build
	LINUX_IMAGE= HYPERVISOR_ELF=$(TEST_BUILD_DIR)/hypervisor.elf \
	SVM_BIN=$(TEST_BUILD_DIR)/svm/svm-basic.bin \
	sh tests/run_svm_test.sh tests/svm/expect/basic.txt

test-qemu-vtimer: test-svm-build
	LINUX_IMAGE= HYPERVISOR_ELF=$(TEST_BUILD_DIR)/hypervisor.elf \
	SVM_BIN=$(TEST_BUILD_DIR)/svm/svm-vtimer.bin \
	sh tests/run_svm_test.sh tests/svm/expect/vtimer.txt

test-qemu-dual: test-svm-dual-build
	LINUX_IMAGE= HYPERVISOR_ELF=$(TEST_DUAL_BUILD_DIR)/hypervisor.elf \
	SVM_BIN=$(TEST_DUAL_BUILD_DIR)/svm/svm-basic.bin \
	SVM_BIN2=$(TEST_DUAL_BUILD_DIR)/svm/svm-vm1.bin \
	sh tests/run_svm_test.sh tests/svm/expect/dual.txt

# Like vSPI, the shell scenario writes QEMU serial stdin; it reuses the
# dual-SVM build because the EL2 shell needs NR_VMS=2 but no guest OS.
test-qemu-shell: test-svm-dual-build
	HYPERVISOR_ELF=$(TEST_DUAL_BUILD_DIR)/hypervisor.elf \
	SVM_BIN=$(TEST_DUAL_BUILD_DIR)/svm/svm-basic.bin \
	SVM_BIN2=$(TEST_DUAL_BUILD_DIR)/svm/svm-vm1.bin sh tests/run_shell_test.sh

# Code outside hypervisor/arch/ may reach the architecture only via *_arch_*
# hooks and <arch/xxx.h> (ADR-0015); zero tolerance, no allowlist.
check-arch-boundary:
	CC="$(CC)" CPPFLAGS="$(filter -D% -I%,$(CFLAGS))" sh scripts/check-arch-boundary.sh

# Second-arch check (ADR-0015): the generic code must compile for x86_64 and
# link against arch/x86's stub hooks. No weak defaults, so a hook the stub
# lacks fails the link. Build/link only -- the result is never run.
X86_BUILD_DIR := build/test-x86

test-x86-stub:
	$(MAKE) CONFIG=configs/x86_stub_defconfig BUILD_DIR=$(X86_BUILD_DIR) all

test: check-arch-boundary test-x86-stub check-offsets check-offsets-target test-qemu test-qemu-vtimer test-qemu-dual test-qemu-shell test-qemu-vspi

run: $(ELF) $(GUEST_DTB) $(GUEST_DTB1)
	./scripts/run-qemu.sh

clean:
	rm -rf $(BUILD_DIR)

# `make defconfig DEFCONFIG=x86_stub` selects configs/x86_stub_defconfig.
DEFCONFIG ?= qemu_virt

defconfig:
	cp configs/$(DEFCONFIG)_defconfig .config
	@echo "Wrote .config from configs/$(DEFCONFIG)_defconfig"

menuconfig:
	@echo "menuconfig reserved for a later milestone; edit .config by hand."
	@false

help:
	@echo "Targets: all run clean defconfig"
	@echo "Config:  CONFIG=$(CONFIG) (ARCH/BOARD come from it: ARCH=$(ARCH) BOARD=$(BOARD))"
	@echo "Vars:    CROSS_COMPILE=$(CROSS_COMPILE) DEFCONFIG=$(DEFCONFIG)"
