/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* Operand pre-decode: a per-PC, per-thread cache of the decoded form of an
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
 * the corresponding decode.c cases; the differential test suite checks their
 * fidelity against qemu.
 *
 * Deliberately NOT a function-pointer table: the sibling emulator measured a
 * handler-pointer cache ~5-6% slower (indirect-call mispredicts + a large
 * table thrashing the data cache). Dense u8 opcode ids instead, dispatched by
 * direct-threaded computed goto (each handler fetches and dispatches its
 * successor, so every handler owns a branch-predictor slot). */
#ifndef ARM64CHROOT_PDRUN_H
#define ARM64CHROOT_PDRUN_H

#include "predecode.h"

/* Direct-mapped by (pc>>2), 16 B entries. Sized by measured miss rate on a
 * busybox-sh workload: 8 K entries missed 12.9%, 16 K 4.6%, 32 K 2.2%, 64 K
 * no better than 32 K. 16 K (256 KB/thread) is the size/benefit knee. */
#define PD_BITS 14
#define PD_SIZE (1u << PD_BITS)
#define PD_MASK (PD_SIZE - 1)

extern __thread PDEnt g_pdcache[PD_SIZE];
extern int g_predecode;            /* cleared by -nopd (diagnostic mode) */

/* Run instructions back-to-back through the decode cache (direct-threaded
 * dispatch). Returns when the loop must intervene: recorded exception,
 * pending signal, stop/halt, or an instruction-fetch fault. */
void pd_run(CPU *c);

#endif /* ARM64CHROOT_PDRUN_H */
