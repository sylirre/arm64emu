/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* The JIT's coherence with the guest address space: the runtime is
 * lib/arm64/jit's; this is what it asks of the program (jit.h's
 * JitCoherence: jit_va_coherence), and the program's own entry points into
 * it (coherence.h).
 *
 * Each guest thread runs its own CPU, so each has its own translations; the
 * per-CPU caches make all of this lock-free:
 *  - Mapping changes: pte_set_range/pte_prot_range (mem.c) call
 *    jit_invalidate_range -> the calling thread drops its blocks on the
 *    affected pages; if any affected page ever held translated code (the
 *    global sticky code-page map), the global invalidation generation is
 *    bumped and every registered thread's `interrupt` flag is set -- they
 *    conservatively flush their own cache at the next safepoint.
 *  - Self-modifying code: the guest's mandatory IC IVAU (CTR_EL0 advertises
 *    DIC=0/IDC=0) reaches va_ic_ivau, which invalidates the written
 *    line's page. Plain stores are not instrumented: skipping IC on this CPU
 *    is architecturally undefined, exactly as on hardware.
 *  - D-TLB: generated code probes this thread's D-TLB (mem.h's layout, the
 *    CPU's c->eng) without the per-access generation check translate()
 *    does, so every PTE mutation also sets `interrupt` (via as_gen_bump) and
 *    the safepoint resync empties this thread's D-TLB. Stale hits between
 *    safepoints are benign by the same retired-backing quarantine the
 *    interpreter relies on.
 *  - Signals: the host catcher sets `interrupt`; blocks re-check it at every
 *    entry, so delivery latency is bounded by one block.
 *
 * Translations are keyed by guest virtual page (FetchCache.pa_page is the VA
 * page here: there is no physical memory to name). */
#include <stdlib.h>
#include <string.h>

#include "machine.h"
#include "coherence.h"
#include "jit_priv.h"

/* ---- thread registry (cross-thread interrupt only) ----
 * Slots are claimed/released with atomics under as_lock; the fork child
 * clears the table outright (a fork must never inherit a lock or a pointer
 * owned by a thread that does not exist in the child). Notifiers run under
 * as_lock too, so a slot they loaded cannot be unregistered-and-freed
 * mid-dereference. */
#define JIT_ENVS_MAX 256
static JitEnv *g_jit_envs[JIT_ENVS_MAX];
static unsigned long g_jit_inval_gen = 1;

/* Refuses the JIT (nonzero) when every slot is taken. An unregistered thread
 * never has its interrupt flag raised by jit_notify_mapping_change, so it
 * would keep running translations of code another thread has since unmapped,
 * reprotected or rewritten -- silently executing stale native code. Falling
 * back to the interpreter is slow; running the wrong instructions is not a
 * trade we get to make. */
static int va_attach(CPU *c, JitEnv *env) {
    (void)c;
    int ok = -1;
    env->mode_gen = __atomic_load_n(&g_jit_inval_gen, __ATOMIC_ACQUIRE);
    as_lock();
    for (int i = 0; i < JIT_ENVS_MAX; i++) {
        if (!g_jit_envs[i]) { g_jit_envs[i] = env; ok = 0; break; }
    }
    as_unlock();
    return ok;
}

static void va_detach(CPU *c, JitEnv *env) {
    (void)c;
    as_lock();
    for (int i = 0; i < JIT_ENVS_MAX; i++) {
        if (g_jit_envs[i] == env) { g_jit_envs[i] = NULL; break; }
    }
    as_unlock();
}

void jit_notify_mapping_change(void) {
    if (!jit_on()) return;
    as_lock();
    for (int i = 0; i < JIT_ENVS_MAX; i++) {
        JitEnv *e = g_jit_envs[i];
        if (e) __atomic_store_n(&e->interrupt, 1, __ATOMIC_RELEASE);
    }
    as_unlock();
}

void jit_signal_interrupt(void) {
    CPU *c = as_cpu_current();      /* own-thread TLS: prewarmed */
    if (c) jit_interrupt(c);
}

/* The safepoint: resync this thread's D-TLB/fetch cache (generated fast
 * paths skip the per-access generation check translate() performs, so empty
 * the D-TLB eagerly rather than lazily), and start over if another thread
 * invalidated code since. */
static int va_resync(CPU *c, JitEnv *env) {
    (void)c;
    jit_dtlb_reset();
    unsigned long g = __atomic_load_n(&g_jit_inval_gen, __ATOMIC_ACQUIRE);
    if (g == env->mode_gen) return 0;
    env->mode_gen = g;
    return 1;
}

/* The guest shares the descriptor table: the runtime's own memfd lives in
 * one fd window (machine.h, "the emulator's own descriptors"), so the guest
 * never sees its number and a sibling's fork cannot hand its child a copy. */
static void va_private_fd(int enter) {
    if (enter) fdwin_enter(); else fdwin_leave();
}

/* ---- global sticky code-page map ----
 * One bit per guest 4 KB page that ever held a translation in any thread.
 * Monotonic (never cleared), so it is race-free with plain atomic OR and a
 * fork child can keep the inherited copy. Used only to decide whether a PTE
 * mutation must interrupt other threads; false positives merely cost an
 * unnecessary flush. 64 MiB of guest VA per lazily-allocated chunk. */
#define CM_L2_BITS   14                          /* guest pages per L2 chunk (log2) */
/* L1 spans the rest of the guest page-number space. Derived from
 * GUEST_VA_BITS (as mem.c's page table is) so widening the guest VA can never
 * leave this table too small: a stale constant here indexes g_codemap[] out of
 * bounds for high guest addresses (Go's arena hints live near the top of the
 * VA), which reads a garbage pointer and faults the host in codemap_test. */
#define CM_L1_BITS   (GUEST_VA_BITS - 12 - CM_L2_BITS)
#define CM_L2_PAGES  (1u << CM_L2_BITS)
static unsigned char *g_codemap[1u << CM_L1_BITS];

static void codemap_mark(u64 pc) {
    u64 pageno = pc >> 12;
    unsigned l1 = (unsigned)(pageno >> CM_L2_BITS);
    unsigned char *map = __atomic_load_n(&g_codemap[l1], __ATOMIC_ACQUIRE);
    if (!map) {
        unsigned char *fresh = calloc(1, CM_L2_PAGES / 8);
        if (!fresh) return;                      /* degrade: over-notify below */
        unsigned char *expect = NULL;
        if (__atomic_compare_exchange_n(&g_codemap[l1], &expect, fresh, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            map = fresh;
        else { free(fresh); map = expect; }
    }
    unsigned bit = (unsigned)(pageno & (CM_L2_PAGES - 1));
    __atomic_fetch_or(&map[bit >> 3], (unsigned char)(1u << (bit & 7)),
                      __ATOMIC_RELEASE);
}

static int codemap_test(u64 pc) {
    u64 pageno = pc >> 12;
    unsigned char *map =
        __atomic_load_n(&g_codemap[pageno >> CM_L2_BITS], __ATOMIC_ACQUIRE);
    if (!map) return 0;
    unsigned bit = (unsigned)(pageno & (CM_L2_PAGES - 1));
    return (__atomic_load_n(&map[bit >> 3], __ATOMIC_ACQUIRE) >> (bit & 7)) & 1;
}

static void va_translated(CPU *c, const JBlock *b) {
    (void)c;
    codemap_mark(b->pc);
}

/* IC IVAU, Xt. TBI0: cache maintenance by VA ignores the address's top byte,
 * exactly as a data access does (mem.c's translate()), so a tagged code
 * pointer still names the page it really lives on. */
static void va_ic_ivau(CPU *c, u64 va) {
    (void)c;
    va &= A64_TBI_MASK;
    jit_invalidate_range(va & ~63ULL, 64);
}

const JitCoherence jit_va_coherence = {
    .attach      = va_attach,
    .detach      = va_detach,
    .translated  = va_translated,
    .ic_ivau     = va_ic_ivau,
    .resync      = va_resync,
    .private_fd  = va_private_fd,
};

/* ---- coherence entry points ---- */

void jit_invalidate_range(u64 addr, u64 len) {
    if (!jit_on() || !len) return;
    /* IC IVAU hands this an arbitrary guest register value, so the range is
     * not known to be a valid guest VA. Anything at or above GUEST_TASK_SIZE
     * is unmappable by construction and can hold no translation, while the
     * codemap is sized for that space alone -- indexing it with such an
     * address reads past g_codemap[] and faults the host. Clamp. */
    if (addr >= GUEST_TASK_SIZE) return;
    if (len > GUEST_TASK_SIZE - addr) len = GUEST_TASK_SIZE - addr;
    CPU *c = as_cpu_current();
    u64 first = addr & ~(GUEST_PAGE_SIZE - 1);
    u64 npages = ((addr + len - 1 - first) >> 12) + 1;
    int marked = 0;
    if (npages > JIT_PAGE_TBL) {
        /* Range too large to walk page-by-page (e.g. a huge munmap). The
         * codemap can't be probed cheaply over this many pages, so keep the
         * conservative cross-thread notify (marked = 1), but drop only our
         * own in-range blocks via the bounded arena instead of flushing the
         * whole cache -- a large data-region unmap must not evict hot code. */
        marked = 1;
        if (c) jit_drop_range(c, first, npages);
    } else {
        for (u64 i = 0; i < npages; i++) {
            u64 pg = first + (i << 12);
            if (codemap_test(pg)) {
                marked = 1;
                if (c) jit_drop_page(c, pg);
            }
        }
    }
    if (marked) {
        /* Some thread (maybe only ever this one) translated code here: make
         * everyone re-validate. Own precise drop is done; skip the own full
         * flush only if no other invalidation raced in between. */
        unsigned long g =
            __atomic_add_fetch(&g_jit_inval_gen, 1, __ATOMIC_RELEASE);
        JitEnv *env = c ? c->eng.jit : NULL;
        if (env && env->mode_gen == g - 1) env->mode_gen = g;
        jit_notify_mapping_change();
    }
}

void jit_execve_flush(void) {
    if (!jit_on()) return;
    CPU *c = as_cpu_current();
    if (c) jit_flush(c);
    unsigned long g = __atomic_add_fetch(&g_jit_inval_gen, 1, __ATOMIC_RELEASE);
    JitEnv *env = c ? c->eng.jit : NULL;
    if (env) env->mode_gen = g;
    jit_notify_mapping_change();
}

void jit_fork_child(void) {
    /* Single-threaded now; parent threads' registrations are meaningless and
     * a dual-mapped cache aliases the parent's future writes. Reset all. */
    memset(g_jit_envs, 0, sizeof g_jit_envs);
    CPU *c = as_cpu_current();
    if (c) jit_cpu_forked(c);
}
