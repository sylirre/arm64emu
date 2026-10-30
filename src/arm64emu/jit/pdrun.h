/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* The predecoded tier: a decode cache of PDEnts (lib/arm64 predecode.h,
 * classified by pd_fill) executed by direct-threaded dispatch (pdrun.c). */
#ifndef ARM64EMU_PDRUN_H
#define ARM64EMU_PDRUN_H

#include "predecode.h"

/* pd_step's direct-mapped decode cache, indexed by (pc>>2)&PD_MASK. 16 K entries
 * (256 KB) is arm64chroot's measured size/benefit knee. Single global: this is a
 * single-CPU system emulator (the donor's __thread was for its user threads). */
#define PD_BITS 14
#define PD_SIZE (1u << PD_BITS)
#define PD_MASK (PD_SIZE - 1)
extern PDEnt g_pdcache[PD_SIZE];

/* Default predecoded interpreter tier: a direct-threaded executor over the
 * decode cache. pd_step runs a slice (mirrors jit_step's contract) and returns
 * when the driver must intervene (deadline, IRQ line, halt/stop, fetch abort). */
extern int g_pd;                   /* on by default; --no-pd off (main.c); --jit wins */
StepResult pd_step(CPU *c, u64 slice, u64 max_insn);

#endif /* ARM64EMU_PDRUN_H */
