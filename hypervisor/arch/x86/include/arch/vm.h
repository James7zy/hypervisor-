/* SPDX-License-Identifier: TBD */
#ifndef HV_ARCH_VM_H
#define HV_ARCH_VM_H

/*
 * x86 part of the vCPU -- link-check stub (ADR-0015). The fields are
 * placeholders that only give the generic struct vcpu (<vm.h>) something to
 * embed; a real VMX port will reshape them around the VMCS. No asm offset
 * macros: the stub has no assembly.
 */

#ifndef __ASSEMBLER__
#include <types.h>

struct arch_regs {
    u64 gpr[16];    /* rax..r15 */
    u64 rip;
    u64 rflags;
};

struct vcpu_arch {
    u64 vmcs_pa;    /* placeholder */
};
#endif /* !__ASSEMBLER__ */

#endif /* HV_ARCH_VM_H */
