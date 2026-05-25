/* SPDX-License-Identifier: TBD */
#ifndef ARCH_STAGE2_H
#define ARCH_STAGE2_H

#include <types.h>
#include <vm.h>

void stage2_init(struct vcpu *vcpu, u32 vmid);
void stage2_activate(const struct vcpu *vcpu);

#endif /* ARCH_STAGE2_H */
