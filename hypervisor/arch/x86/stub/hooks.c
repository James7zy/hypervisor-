/* SPDX-License-Identifier: TBD */
/*
 * x86 link-check stub (ADR-0015): one do-nothing implementation of every
 * *_arch_* hook the generic code calls. Hooks have no weak defaults, so a
 * hook added to a generic header without a line here fails `make
 * test-x86-stub` at link time -- that failure is the point of this file.
 * Nothing here is ever executed.
 */
#include <types.h>
#include <cpu.h>
#include <percpu.h>
#include <vm.h>

struct percpu *x86_this_percpu;

void cpu_arch_init(void)
{
}

void cpu_arch_halt(void)
{
    for (;;)
        __asm__ volatile("cli; hlt");
}

s64 cpu_arch_power_on(u32 pcpu)
{
    (void)pcpu;
    return -1;
}

void vm_arch_init(struct vm *m)
{
    (void)m;
}

void vm_arch_devices_init(void)
{
}

void vcpu_arch_reset(struct vcpu *v, const struct vm_config *cfg)
{
    (void)v;
    (void)cfg;
}

void vcpu_arch_load(struct vcpu *v)
{
    (void)v;
}

void vcpu_arch_inject_irq(struct vcpu *v, u32 irq)
{
    (void)v;
    (void)irq;
}

void vcpu_arch_run(struct vcpu *v)
{
    (void)v;
}
