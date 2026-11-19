/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* System registers: sysreg.c. */
#ifndef A64_SYSREG_H
#define A64_SYSREG_H

#include "cpu.h"

/* Execute a System-instruction-group encoding (MSR/MRS/SYS/MSR-imm). */
void sysreg_exec(CPU *c, u32 insn);

/* May EL0 execute this System-instruction-group encoding, under the EL1
 * configuration now in SCTLR_EL1 and CNTKCTL_EL1? Anything else is UNDEFINED
 * there (sysreg_exec raises it). The JIT asks it, in a full system, before
 * inlining one into an EL0 block. */
bool sysreg_el0_ok(CPU *c, u32 insn);

/* Initialise ID/feature registers at reset. */
void sysreg_init(CPU *c);

/* The ID register op0 3, op1 0, CRn 0, CRm, op2 as EL1 reads it: what this CPU
 * implements, before a kernel sanitizes EL0's view of it (in user mode the
 * program, playing that kernel, builds EL0's view from it). */
u64 sysreg_id_read(CPU *c, unsigned CRm, unsigned op2);

#endif /* A64_SYSREG_H */
