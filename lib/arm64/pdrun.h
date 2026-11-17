/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* Operand pre-decode: a per-PC, per-CPU cache of the decoded form of an
 * instruction — a dense opcode id plus pre-extracted operand fields — that
 * lets the run loop skip exec_a64's classification tree and field extraction
 * for hot instruction forms. Entries are a pure function of the instruction
 * word (PC-relative forms add c->cur_insn_pc at execute time), so comparing
 * the cached word against the freshly fetched one fully validates a hit: no
 * flush is needed for mmap/munmap/execve/self-modifying code.
 *
 * Anything the classifier (lib/arm64 predecode.c) does not recognize with
 * certainty dispatches as PD_GENERIC straight into exec_a64, which stays the
 * source of truth. The specialized handlers (pdrun.c) are transcriptions of
 * the corresponding decode.c cases; the differential test suites check their
 * fidelity against qemu and against the plain interpreter.
 *
 * Deliberately NOT a function-pointer table: a handler-pointer cache measured
 * ~5-6% slower (indirect-call mispredicts + a large table thrashing the data
 * cache). Dense u8 opcode ids instead, dispatched by
 * direct-threaded computed goto (each handler fetches and dispatches its
 * successor, so every handler owns a branch-predictor slot). */
#ifndef A64_PDRUN_H
#define A64_PDRUN_H

#include "predecode.h"

/* Direct-mapped by (pc>>2), 16 B entries. Sized by measured miss rate on a
 * busybox-sh workload: 8 K entries missed 12.9%, 16 K 4.6%, 32 K 2.2%, 64 K
 * no better than 32 K. 16 K (256 KB per CPU) is the size/benefit knee. */
#define PD_BITS 14
#define PD_SIZE (1u << PD_BITS)
#define PD_MASK (PD_SIZE - 1)

extern int g_pd;                   /* the tier is on (default); the program
                                    * turns it off on request, and for the JIT
                                    * and the per-instruction diagnostics */

/* Run instructions back-to-back through the CPU's decode cache
 * (direct-threaded dispatch), the cache allocated on the first call (c->eng.pd).
 * Returns when the CPU asks for attention, when icount reaches `deadline`, or
 * on an instruction-fetch fault; at least one instruction runs. */
void pd_run(CPU *c, u64 deadline);
/* Release the CPU's decode cache: the program calls this when the host thread
 * that ran the CPU is done with it. */
void pd_cache_free(CPU *c);

#endif /* A64_PDRUN_H */
