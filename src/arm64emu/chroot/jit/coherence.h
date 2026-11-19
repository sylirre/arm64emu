/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* The JIT's coherence with the guest address space (coherence.c): what the
 * memory layer, the signal catcher and the process lifecycle tell the JIT.
 * The runtime itself is lib/arm64/jit's (jit.h). All of these are cheap
 * no-ops when the JIT is off. */
#ifndef ARM64EMU_JIT_COHERENCE_H
#define ARM64EMU_JIT_COHERENCE_H

#include "jit.h"

/* What the JIT runtime asks of the program (jit.h), for its CpuOps. */
extern const JitCoherence jit_va_coherence;

/* A guest range was unmapped/remapped/reprotected, or guest IC IVAU hit it:
 * drop the calling thread's translations for the affected pages and
 * interrupt other threads (they conservatively flush at their next
 * safepoint). */
void jit_invalidate_range(u64 addr, u64 len);

/* Any PTE mutation happened (mem.c as_gen_bump): make every thread leave
 * generated code at its next safepoint and resync its D-TLB. */
void jit_notify_mapping_change(void);

/* Called from the host signal catcher on this thread (async-signal-safe):
 * make generated code exit at the next block entry so the run loop can
 * deliver the guest signal promptly. */
void jit_signal_interrupt(void);

/* execve tears the address space down (as_destroy): drop everything. */
void jit_execve_flush(void);

/* fork()/vfork() child: parent threads don't exist here and dual-mapped code
 * caches alias the parent -- reset all JIT state (re-created lazily). */
void jit_fork_child(void);

#endif /* ARM64EMU_JIT_COHERENCE_H */
