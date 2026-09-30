/* SPDX-License-Identifier: TBD */
#ifndef HV_VM_H
#define HV_VM_H

#ifndef __ASSEMBLER__
#include <types.h>
#include <spinlock.h>
#include <percpu.h>   /* NR_CPUS, for the NR_VMS/VCPUS_PER_VM static_assert
                         below (percpu.h forward-declares struct vcpu) */
#include <vuart.h>    /* struct vuart, embedded by value in struct vm below */
#include <arch/vm.h>  /* struct arch_regs, struct vcpu_arch */

/* Compile-time VM count. 1 until M5 slice 3; the Makefile overrides it
 * per guest profile (-DCONFIG_NR_VMS=N). */
#ifndef CONFIG_NR_VMS
#define CONFIG_NR_VMS 1
#endif
#define NR_VMS       CONFIG_NR_VMS
#define VCPUS_PER_VM 2   /* static 2-vCPU VMs; NR_CPUS = NR_VMS * VCPUS_PER_VM */

/*
 * secondary_main derives a woken pCPU's (VM, vCPU) purely from its pCPU id
 * (id / VCPUS_PER_VM, id % VCPUS_PER_VM); vm_configs[]/pcpu_base must agree
 * with that structural mapping (see vm_init's boot-time check in vm.c). This
 * only makes sense if every VM's static pCPU slot fits within the physical
 * machine, i.e. NR_VMS * VCPUS_PER_VM <= NR_CPUS -- catch a mismatch (e.g. a
 * future CONFIG_NR_VMS bump without a matching NR_CPUS bump) at compile time
 * instead of silently indexing percpu[]/sgi_pending[] out of bounds.
 *
 * NR_CPUS (percpu.h) is a fixed PLATFORM constant sized for the largest build
 * profile (4, since M5 slice 3: -smp 4 in run-qemu.sh, 2 VMs x 2 vCPUs). It is
 * intentionally NOT required to equal NR_VMS * VCPUS_PER_VM: single-VM build
 * profiles (HV_GUEST=svm, NR_VMS=1, the pre-slice-3 SVM regression
 * tests) only ever PSCI CPU_ON pCPUs 0/1 -- pCPUs 2/3 are simply never woken
 * under those profiles, so under-using the physical core budget is safe; the
 * dangerous direction (a VM's slot overrunning NR_CPUS) is what this guards.
 */
_Static_assert(NR_VMS * VCPUS_PER_VM <= NR_CPUS,
               "NR_VMS * VCPUS_PER_VM must fit within NR_CPUS");

struct vm;

struct vcpu {
    struct arch_regs regs;   /* MUST be first: guest GPR frame (asm offset 0) */
    struct vcpu_arch arch;   /* arch-private vCPU state; asm-visible fields are
                                laid out in <arch/vm.h> */
    /* Generic fields below are NOT read by the exception-entry asm. */
    struct vm *owner;        /* back-pointer: trap handlers navigate via
                                current_vcpu()->owner instead of globals */
    u32        vcpu_idx;     /* VM-local vCPU index (the guest's CPU number) */
};

struct vm_config;

struct vm {
    struct vcpu            vcpu[VCPUS_PER_VM];
    u32                    id;        /* index into vm[]; keys the per-VM
                                         file-static state in stage2.c /
                                         vgic_v3_mmio.c */
    const struct vm_config *config;
    struct vuart            vuart;    /* emulated PL011; see hypervisor/dm/vuart.c */
    /* M5 slice 3: set by psci_power_down() when this VM calls CPU_OFF/
     * SYSTEM_OFF/SYSTEM_RESET. Not read by any asm path (struct vm has no
     * __ASSEMBLER__ offset macros, unlike struct vcpu/struct hv_ctx) —
     * only the kick-SGI branch of el2_irq_handler polls it, to park this VM's
     * other pCPU(s) instead of re-entering their guest. */
    volatile u32            off;
};

extern struct vm vm[NR_VMS];

void vm_init(void);
void vm_run(void);

/*
 * VM/vCPU hooks every arch implements (ADR-0015); no weak defaults.
 */
/* Per-VM arch setup (arm64: build the VM's Stage-2 table, VMID, VTTBR). */
void vm_arch_init(struct vm *m);
/* Register the arch's emulated platform devices (arm64: the vGIC
 * distributor/redistributors) on the MMIO bus, once for all VMs. */
void vm_arch_devices_init(void);
/* Put a VM's boot vCPU in its architectural reset/boot state for `cfg`
 * (arm64: Linux boot protocol registers, HCR_EL2, virtual CPU interface). */
void vcpu_arch_reset(struct vcpu *v, const struct vm_config *cfg);
/* Load `v` onto this pCPU's virtualization hardware (arm64: VMPIDR_EL2,
 * Stage-2, vGIC list registers). */
void vcpu_arch_load(struct vcpu *v);
/* Enter the guest on this pCPU; returns on an exit that needs the caller. */
void vcpu_arch_run(struct vcpu *v);
#endif /* !__ASSEMBLER__ */

#ifdef __ASSEMBLER__
#include <arch/vm.h>   /* asm offset macros into struct vcpu / struct hv_ctx */
#endif


#endif /* HV_VM_H */
