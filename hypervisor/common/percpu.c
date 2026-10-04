/* SPDX-License-Identifier: TBD */
#include <percpu.h>

/*
 * Static per-CPU array, one slot per physical core (NR_CPUS, board-provided).
 * Each pCPU's slot is pointed to by its TPIDR_EL2; the exception-entry asm
 * reaches the current vCPU via that pointer (see percpu.h / ADR-0003).
 */
struct percpu percpu[NR_CPUS];
