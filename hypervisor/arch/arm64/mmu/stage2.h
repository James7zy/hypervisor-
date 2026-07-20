/* SPDX-License-Identifier: TBD */
#ifndef ARCH_STAGE2_H
#define ARCH_STAGE2_H

#include <types.h>
#include <vm.h>

void stage2_init(struct vm *m);
void stage2_activate(const struct vcpu *vcpu);

#endif /* ARCH_STAGE2_H */
