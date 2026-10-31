/* SPDX-License-Identifier: TBD */
#ifndef HV_VM_H
#define HV_VM_H

#ifndef __ASSEMBLER__
#include <types.h>
#include <spinlock.h>
#include <vcpu_state.h>
#include <percpu.h>   /* NR_CPUS (percpu.h forward-declares struct vcpu) */
#include <arch/vm.h>  /* struct arch_regs, struct vcpu_arch */

/* Compile-time VM count. 1 until M5 slice 3; the Makefile overrides it
 * per guest profile (-DCONFIG_NR_VMS=N). */
#ifndef CONFIG_NR_VMS
#define CONFIG_NR_VMS 1
#endif
#define NR_VMS       CONFIG_NR_VMS
#define VCPUS_PER_VM 2   /* static 2-vCPU VMs; affinity must fit NR_CPUS
                            (validated per entry at boot in vm_init — the
                            compile-time NR_VMS*VCPUS_PER_VM assert was
                            replaced by that check, M11 slice A D6) */

struct vm;

struct vcpu {
    struct arch_regs regs;   /* MUST be first: guest GPR frame (asm offset 0) */
    struct vcpu_arch arch;   /* arch-private vCPU state; asm-visible fields are
                                laid out in <arch/vm.h> */
    /* Generic fields below are NOT read by the exception-entry asm. */
    struct vm *owner;        /* back-pointer: trap handlers navigate via
                                current_vcpu()->owner instead of globals */
    u32        vcpu_idx;     /* VM-local vCPU index (the guest's CPU number) */
    enum vcpu_state state;   /* per-vCPU run state (M11 slice A); appended
                                after the asm-visible prefix — existing
                                VCPU_* offsets unchanged (pinned in
                                tests/check_offsets_target.c) */
};

struct vm_config;

struct vm {
    struct vcpu            vcpu[VCPUS_PER_VM];
    u32                    id;        /* index into vm[]; keys the per-VM
                                         file-static state in arch code
                                         and in dm/vuart.c */
    const struct vm_config *config;
};

extern struct vm vm[NR_VMS];

void vm_init(void);
void vm_run(void);

/*
 * The vCPU statically affined to pCPU `pcpu` (vm_config.vcpu_pcpu[]), or NULL
 * if none. With 1:1 tables this is the pCPU's only vCPU; with shared
 * affinity (legal data from slice A) it is the LOWEST (vmi, idx) — slice B's
 * scheduler replaces this pick. M11 slice A.
 */
struct vcpu *vcpu_for_pcpu(u32 pcpu);

/*
 * VM/vCPU hooks every arch implements (ADR-0015); no weak defaults.
 */
/* Per-VM arch setup (second-stage translation tables and their identifier). */
void vm_arch_init(struct vm *m);
/* Register the arch's emulated platform devices (e.g. the interrupt
 * controller) on the MMIO bus, once for all VMs. */
void vm_arch_devices_init(void);
/* Put a VM's boot vCPU in its architectural reset/boot state for `cfg`
 * (boot-protocol registers, virtualization controls, virtual interrupt
 * interface). */
void vcpu_arch_reset(struct vcpu *v, const struct vm_config *cfg);
/* Load `v` onto this pCPU's virtualization hardware (CPU identity,
 * second-stage tables, virtual interrupt state) and reset this pCPU's
 * live EL1 CPU state to the vCPU's fresh-boot state (warm reset: EL1
 * sysregs are not saved across CPU_OFF, so a re-authorized CPU_ON must
 * not inherit the parked guest's MMU-on state). */
void vcpu_arch_load(struct vcpu *v);
/* Raise virtual interrupt `irq` for vCPU `v`; callable from any pCPU
 * (kicking v's pCPU if remote). Only shared peripheral interrupts are
 * supported (the vuart's) -- the one caller today. */
void vcpu_arch_inject_irq(struct vcpu *v, u32 irq);
/* Enter the guest on this pCPU; returns on an exit that needs the caller. */
void vcpu_arch_run(struct vcpu *v);
#endif /* !__ASSEMBLER__ */

#ifdef __ASSEMBLER__
#include <arch/vm.h>   /* asm offset macros into struct vcpu / struct hv_ctx */
#endif

#endif /* HV_VM_H */
