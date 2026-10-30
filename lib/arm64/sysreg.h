/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* System register / SMCCC hooks. All but smccc_conduit are sysreg.c's. */
#ifndef A64_SYSREG_H
#define A64_SYSREG_H

#include "cpu.h"

/* Execute a System-instruction-group encoding (MSR/MRS/SYS/MSR-imm). */
void sysreg_exec(CPU *c, u32 insn);

/* May EL0 execute this System-instruction-group encoding, under the EL1
 * configuration now in SCTLR_EL1 and CNTKCTL_EL1? Anything else is UNDEFINED
 * there (sysreg_exec raises it). arm64emu's JIT asks it before inlining one
 * into an EL0 block. */
bool sysreg_el0_ok(CPU *c, u32 insn);

/* Initialise ID/feature registers at reset. */
void sysreg_init(CPU *c);

/* The ID register op0 3, op1 0, CRn 0, CRm, op2 as EL1 reads it: what this CPU
 * implements, before a kernel sanitizes EL0's view of it (arm64chroot's
 * loop.c, playing that kernel, builds EL0's view from it). */
u64 sysreg_id_read(CPU *c, unsigned CRm, unsigned op2);

/* Handle an SMC/HVC conduit call (PSCI / SMCCC). Returns true if handled.
 * arm64emu's (devices/psci.c); arm64chroot has none, so it is weak and called
 * only under a NULL check. */
bool smccc_conduit(CPU *c, bool is_hvc) __attribute__((weak));

#endif /* A64_SYSREG_H */
