/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* Optional JIT (off by default): translates guest basic blocks to host
 * native code. Interpreter semantics stay the source of truth: anything the
 * translator does not handle natively is executed by calling exec_a64, and
 * every phase of the JIT must keep the differential and consistency suites
 * green.
 *
 * Per CPU throughout (c->eng.jit: code cache, block tables, jump cache),
 * like the decode cache and the D-TLB: there are no locks in the JIT runtime
 * and never any cross-thread writes to generated code. What each mode must
 * add to stay coherent with the memory it models -- learning that code
 * changed under a translation -- is the program's, behind the jit_mode_*
 * hooks below; another thread asks for it through the CPU's interrupt flag,
 * checked at every block entry (safepoint). */
#ifndef A64_JIT_H
#define A64_JIT_H

#include "cpu.h"

/* The JIT is on (set by the program from its command line, defined in
 * jit.c). Cleared at startup when no backend exists for this host or a
 * per-instruction debug facility is on, and at runtime if a CPU's code cache
 * cannot be set up (W^X denial) or a block cannot be emitted at all.
 *
 * The program sets it before there is a second thread and may touch it
 * directly. Everything after that goes through the two accessors below: the
 * runtime clears it from whichever guest thread hit the problem, while every
 * other thread reads it once per run-loop iteration, and a plain int shared
 * that way is a C11 data race. Relaxed ordering is all it needs -- the flag
 * only ever goes one way, and a thread that reads a stale 1 merely
 * translates one more block before it notices. A relaxed load is the same
 * instruction a plain one compiled to, so the run loop pays nothing for it. */
extern int g_jit;
static inline int jit_on(void) {
    return __atomic_load_n(&g_jit, __ATOMIC_RELAXED);
}
static inline void jit_disable(void) {
    __atomic_store_n(&g_jit, 0, __ATOMIC_RELAXED);
}

/* True if this build carries a code generator for the host architecture
 * (x86-64, AArch64, i686, ARM32; other hosts run the interpreter). */
int jit_backend_available(void);

/* The longest block: translated code checks the deadline at block entries,
 * so it can retire up to this many instructions past it. */
#define JIT_MAX_BLOCK_INSNS 128

/* Run translated code until the CPU asks for attention, icount reaches
 * `deadline` (cpu.h, "Run control") -- up to a block late -- or an
 * instruction fetch faults: the same return contract as pd_run. The first
 * call on a CPU sets its JIT up; where that fails the JIT turns itself off
 * (jit_on) and the call returns having run nothing. */
void jit_run(CPU *c, u64 deadline);

/* ---- the program's use of a CPU's JIT ----
 * All of these are no-ops for a CPU whose JIT was never set up. */

/* Make generated code leave at its next block entry, the dispatcher then
 * asking the program to resync (jit_mode_resync). Async-signal-safe, and
 * callable from any thread. */
void jit_interrupt(CPU *c);
/* Drop the CPU's translations on one page (by the key JBlock.page goes by:
 * physical in a full system, virtual in user mode), counting the rewrite for
 * the thrash guard; or on npages pages from `first` without counting, by a
 * walk of the block arena that is bounded however wide the range. Safe from
 * a helper inside generated code: a dropped block's code stays allocated
 * until the next flush, so a block dropping itself still runs its tail. */
void jit_drop_page(CPU *c, u64 page);
void jit_drop_range(CPU *c, u64 first, u64 npages);
/* Drop every translation and what the thrash guard has learned. Never from
 * inside generated code: the code memory is reused at once. */
void jit_flush(CPU *c);
/* The host thread running the CPU is done with it (a thread exits):
 * release its JIT, its share of the statistics merged first. */
void jit_cpu_release(CPU *c);
/* fork(2) child: the CPU's JIT belongs to the parent -- a dual-mapped code
 * cache aliases the parent's pages -- so drop it unmerged, with the
 * process-wide statistics, which describe the parent too. */
void jit_cpu_forked(CPU *c);
/* Is `pc` a host address inside the CPU's translated-code cache? A host
 * fault there cannot be unwound: guest registers may live only in host
 * registers, with no way to write them back. */
int jit_pc_in_generated(CPU *c, const void *pc);
/* Where to resume a host bus fault taken at `pc` inside generated code: the
 * slow-path entry of the inline memory access it belongs to, or NULL if `pc`
 * is not in one. Resuming there re-runs the access through the memory helper
 * with every cached guest register intact. Recorded only where guest memory
 * can fault on the host at all (user mode). Async-signal-safe. */
const void *jit_fault_fixup(CPU *c, const void *pc);

/* ARM64_JIT_ENV("STATS"): write the report now -- the process is about to
 * leave through _exit, which skips the atexit handler that otherwise does.
 * No-op when the knob is off. */
void jit_stats_flush(void);
/* The statistics' lock, for the program's pthread_atfork handlers (a lock a
 * sibling held at fork is inherited locked and ownerless). */
void jit_locks_take(void);
void jit_locks_drop(void);
void jit_locks_reinit(void);

/* ---- the program's part of the runtime ----
 * Defined by the program, for the memory it models. */
struct JitEnv;
struct JBlock;
/* A CPU's JIT has just been set up (its env, not yet in use): set up the
 * program's share. Nonzero refuses it -- the JIT then turns itself off. */
int  jit_mode_attach(CPU *c, struct JitEnv *env);
/* ...and is about to be released (jit_cpu_release). */
void jit_mode_detach(CPU *c, struct JitEnv *env);
/* A block was translated: from now on a write to its page must reach
 * jit_drop_page. */
void jit_mode_translated(CPU *c, const struct JBlock *b);
/* The guest ran IC IVAU for the line at `va` (Xt, as the guest gave it). */
void jit_mode_ic_ivau(CPU *c, u64 va);
/* env->interrupt was raised: resync what generated code reads behind the
 * interpreter's back before another block runs. Nonzero: drop every
 * translation first. */
int  jit_mode_resync(CPU *c, struct JitEnv *env);
/* Brackets the life of a descriptor the runtime opens for itself (the W^X
 * fallback's memfd, closed again once mapped): enter 1, then 0. */
void jit_mode_private_fd(int enter);

#endif /* A64_JIT_H */
