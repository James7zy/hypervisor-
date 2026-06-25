/* SPDX-License-Identifier: TBD */
#include <percpu.h>

/*
 * Static per-CPU array (M3.5: NR_CPUS == 2, static 1:1 pinning, no scheduler).
 * Each pCPU's slot is pointed to by its TPIDR_EL2; the exception-entry asm
 * reaches the current vCPU via that pointer (see percpu.h / ADR-0003).
 */
struct percpu percpu[NR_CPUS];
