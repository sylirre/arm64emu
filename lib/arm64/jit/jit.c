/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* JIT runtime: a CPU's code cache and block tables, the dispatch loop, the
 * helpers generated code calls, and the coherence both modes share. What
 * each mode adds -- how a page's translations learn that its code changed --
 * is the program's (the jit_mode_* hooks, jit.h).
 *
 * Translation unit is a basic block (ends at any branch/system instruction,
 * capped, never crossing a 4 KB guest page). Anything not translated inline
 * is a call to jit_exec1 -> exec_a64, so semantics are the interpreter's by
 * construction.
 *
 * Coherence:
 *  - Block identity: the lookup key is (pc << 3 | ctx) -- EL0, the MMU, the
 *    SP bank, the FPEN trap (jit_ctx) -- AND the code page's host pointer as
 *    resolved at dispatch time through the fetch cache. A full system's TTBR
 *    switch or remap changes the resolution and misses naturally:
 *    correctness never depends on observing TLBI.
 *  - Invalidation goes by page key (JBlock.page): the physical page in a
 *    full system, where stores and DMA name memory physically, the virtual
 *    page in user mode, where munmap, mprotect and IC IVAU name it. The
 *    program drops a page's blocks when it learns the page changed
 *    (jit_drop_page); a block dropping itself from a helper runs its stale
 *    tail to the block's end (code memory is bump-allocated, never reused
 *    before a full flush) -- architecturally permitted without IC IVAU+ISB,
 *    and bounded at JIT_MAX_BLOCK_INSNS.
 *  - Chaining: in user mode across pages, since every change of what a VA
 *    holds reaches the JIT as an invalidation; in a full system only to a
 *    same-page, same-ctx successor, because the guest remaps without telling
 *    anyone and a chained jump bypasses the dispatcher's verification --
 *    within one page and one context, the entry check that validated the
 *    successor covers its predecessor's page too.
 *  - The jump cache (indirect branches) is VA-keyed: an entry goes with its
 *    block, and every entry with the CPU's code epoch (CpuEngine.code_epoch:
 *    a full system's TLB maintenance) and with a flush.
 *  - D-TLB: helper-path accesses go through the mem.h slow paths, so TLB
 *    maintenance is inherited from the interpreter. Generated code's inline
 *    probe skips what an interpreter access checks lazily (CpuEngine.gen_src);
 *    where another thread can change the mappings, the program raises
 *    `interrupt` and resyncs the D-TLB at the safepoint (jit_mode_resync).
 *  - IRQ/timer (a full system): the block-entry safepoint exits when
 *    c->icount reaches env->icount_deadline, so chained hot loops return to
 *    the run loop at its cadence; the dispatcher checks the CPU's attention
 *    and the interrupt lines between blocks.
 *
 * jit_flush_all reuses cache memory; it must only run from jit_run's C loop
 * or code outside generated code (never while a block that could be reached
 * again is still on the native call stack). */
#include <fcntl.h>
#include <fenv.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "arm64_config.h"
#include "esr.h"
#include "predecode.h"
#include "jit_priv.h"
#include "ir.h"

int g_jit;                          /* set by the program's command line */

/* The guest's flush-to-zero modes, as the IR spells them. What they cost the
 * inline FP surface is ir.h's vop_fpcr_blocked: nothing at all on a host that
 * can carry them in its own FPCR, the whole sensitive surface elsewhere.
 * FPCR.RMode is NOT here -- the classes that care about it carry their own
 * runtime gate. */
static inline u8 jit_fpmode(const CPU *c) {
    return (u8)(((c->fpcr >> 24) & 1u) * FPMODE_FZ |
                ((c->fpcr >> 19) & 1u) * FPMODE_FZ16);
}

/* ---- backend stubs for hosts without a code generator ---- */
#if !defined(__x86_64__) && !defined(__aarch64__) && !defined(__i386__) && \
    !defined(__arm__)
int  be_available(void) { return 0; }
void be_emit_thunks(Emit *e, JitEnv *env) { (void)e; (void)env; }
int  be_emit_block(Emit *e, JitEnv *env, JBlock *b, const struct IRBlock *ir) {
    (void)e; (void)env; (void)b; (void)ir;
    return -1;
}
void be_patch_chain(JitEnv *env, JBlock *b, int slot, const u8 *target_rx) {
    (void)env; (void)b; (void)slot; (void)target_rx;
}
void be_unpatch_chain(JitEnv *env, JBlock *b, int slot) {
    (void)env; (void)b; (void)slot;
}
void be_flush_icache(const u8 *rx, const u8 *rw, size_t len) {
    (void)rx; (void)rw; (void)len;
}
int be_vop_ok(unsigned vclass, u32 insn) {
    (void)vclass; (void)insn;
    return 0;
}
#endif

int jit_backend_available(void) { return be_available(); }

/* The CPU's JIT, as a signal handler or another thread may see it: published
 * once complete (jit_run), withdrawn before it is torn down. */
static inline JitEnv *cpu_jit(CPU *c) {
    return __atomic_load_n(&c->eng.jit, __ATOMIC_ACQUIRE);
}

void jit_interrupt(CPU *c) {
    JitEnv *env = cpu_jit(c);
    if (env) __atomic_store_n(&env->interrupt, 1, __ATOMIC_RELEASE);
}

/* ---- bus-fault fixups ---- */

void be_fixup_add(JitEnv *env, u32 fast_off, u32 slow_off) {
    if (!env->fixups || env->nfixups >= JIT_MAX_FIXUPS) return;   /* degrade */
    env->fixups[env->nfixups].fast = fast_off;
    env->fixups[env->nfixups].slow = slow_off;
    /* Publish the entry before the count: a signal handler may read this
     * table while translation is appending to it. */
    __atomic_store_n(&env->nfixups, env->nfixups + 1, __ATOMIC_RELEASE);
}

int jit_pc_in_generated(CPU *c, const void *pc) {
    JitEnv *env = cpu_jit(c);
    if (!env || !env->active || !env->cache_rx) return 0;
    const u8 *p = (const u8 *)pc;
    return p >= env->cache_rx && p < env->cache_rx + env->cache_size;
}

const void *jit_fault_fixup(CPU *c, const void *pc) {
    JitEnv *env = cpu_jit(c);
    if (!env || !env->fixups || !jit_pc_in_generated(c, pc)) return NULL;
    u32 off = (u32)((const u8 *)pc - env->cache_rx);
    u32 lo = 0, hi = __atomic_load_n(&env->nfixups, __ATOMIC_ACQUIRE);
    while (lo < hi) {                       /* last entry with fast <= off */
        u32 mid = lo + (hi - lo) / 2;
        if (env->fixups[mid].fast <= off) lo = mid + 1; else hi = mid;
    }
    if (lo == 0) return NULL;
    const JFixup *f = &env->fixups[lo - 1];
    if (off < f->fast || off >= f->slow) return NULL;   /* between accesses */
    return env->cache_rx + f->slow;
}

/* ---- optional helper-call profiler (ARM64_JIT_ENV("STATS")=1 or =/path) ----
 * Ranks the exact instruction words still executed through the exec_a64
 * helper, so inlining work can be aimed at what a workload actually runs
 * (feed the words to a disassembler). Each CPU counts into an open-addressed
 * table of its own, merged into the process-wide one when the CPU is
 * released; the process dumps the top entries at exit, after merging the
 * CPU of the thread that exits. The icount ratio covers merged CPUs only --
 * fine for a profiler. */
#define JSTAT_SLOTS 4096
typedef struct JStat { u32 word; u32 pad_; u64 count; } JStat;
/* -1 until the first CPU's JIT setup decides; then 0 or 1. Shared by every
 * guest thread and settled (or cleared, on an allocation failure) by
 * whichever gets there first, so it moves through relaxed atomics for the
 * same reason g_jit does -- the answer is the same whoever computes it, but a
 * plain int written by one thread and read by another is still a data
 * race. */
static int g_jit_stats = -1;
static int g_jstat_fd = -1;                  /* parked dup of stderr */
static const char *g_jstat_path;             /* =/file: append */
static JStat g_jstat[JSTAT_SLOTS];
static u64 g_jstat_lost, g_jstat_icount, g_jstat_ntrans, g_jstat_ndisp;
static u32 g_jstat_flushes;
static pthread_mutex_t g_jstat_mu = PTHREAD_MUTEX_INITIALIZER;
/* The CPU this thread set a JIT up for: the one the exit-time dump merges.
 * Only the statistics read it, never a hot path. */
static __thread JitEnv *t_jstat_env;

/* A lock held by a sibling at fork is inherited locked and ownerless whether
 * or not it is on a hot path, and a wedged child under the one knob that
 * exists to diagnose the JIT would be a poor place to find that out. It is
 * the outermost of the program's locks; nothing is taken under it. */
void jit_locks_take(void)   { pthread_mutex_lock(&g_jstat_mu); }
void jit_locks_drop(void)   { pthread_mutex_unlock(&g_jstat_mu); }
void jit_locks_reinit(void) { pthread_mutex_init(&g_jstat_mu, NULL); }

static inline int jstat_on(void) {
    return __atomic_load_n(&g_jit_stats, __ATOMIC_RELAXED) > 0;
}

static void jstat_add(JStat *tab, u32 insn, u64 n, u64 *lost) {
    u32 h = (insn ^ (insn >> 13) ^ (insn >> 25)) & (JSTAT_SLOTS - 1);
    for (u32 i = 0; i < 8; i++) {
        JStat *s = &tab[(h + i) & (JSTAT_SLOTS - 1)];
        if (s->count == 0) s->word = insn;
        if (s->word == insn) { s->count += n; return; }
    }
    *lost += n;
}

/* Out of line: inlined into jit_exec1, its allocation would cost every
 * helper call a saved register. */
static __attribute__((noinline, cold)) void jstat_bump(JitEnv *env, u32 insn) {
    if (!env->jstat) {
        env->jstat = calloc(JSTAT_SLOTS, sizeof *env->jstat);
        if (!env->jstat) { __atomic_store_n(&g_jit_stats, 0, __ATOMIC_RELAXED); return; }
    }
    jstat_add(env->jstat, insn, 1, &env->jstat_lost);
}

/* Fold a CPU's counts into the process-wide ones, and start it over. */
static void jstat_merge(JitEnv *env) {
    pthread_mutex_lock(&g_jstat_mu);
    if (env->jstat)
        for (u32 i = 0; i < JSTAT_SLOTS; i++)
            if (env->jstat[i].count)
                jstat_add(g_jstat, env->jstat[i].word, env->jstat[i].count,
                          &g_jstat_lost);
    g_jstat_lost += env->jstat_lost;
    g_jstat_icount += env->c->icount;
    g_jstat_ntrans += env->ntrans;
    g_jstat_ndisp += env->ndisp;
    g_jstat_flushes += env->flush_count;
    pthread_mutex_unlock(&g_jstat_mu);
    free(env->jstat);
    env->jstat = NULL;
    env->jstat_lost = env->ntrans = env->ndisp = 0;
}

static const char *jstat_class(u32 w) {
    if ((w >> 24) == 0xD5) return "system";
    if ((w & 0x3F000000u) == 0x08000000u) return "excl";
    if ((w & 0x3F200C00u) == 0x38200000u) return "lse";   /* V=0 only */
    unsigned grp = (w >> 25) & 0xf;
    if (grp == 0xa || grp == 0xb) return "branch";
    if ((grp & 5) == 4) return "ldst";
    if ((grp & 7) == 7) return "fpsimd";
    return "other";
}

static int jstat_cmp(const void *a, const void *b) {
    u64 ca = ((const JStat *)a)->count, cb = ((const JStat *)b)->count;
    return (ca < cb) - (ca > cb);
}

static void jstat_dump(void) {
    static int done;
    if (done) return;
    done = 1;
    if (t_jstat_env && t_jstat_env->active) jstat_merge(t_jstat_env);
    pthread_mutex_lock(&g_jstat_mu);
    u64 total = g_jstat_lost;
    for (u32 i = 0; i < JSTAT_SLOTS; i++) total += g_jstat[i].count;
    /* The guest may have closed stderr (in user mode fd 2 is shared with
     * it): write to the fd parked at enable time, or append to =<path>. */
    int fd = g_jstat_path
                 ? open(g_jstat_path, O_WRONLY | O_CREAT | O_APPEND, 0644)
                 : g_jstat_fd;
    if (total && fd >= 0) {
        qsort(g_jstat, JSTAT_SLOTS, sizeof *g_jstat, jstat_cmp);
        dprintf(fd, "[jit-stats pid %d] %llu helper insns",
                (int)getpid(), (unsigned long long)total);
        if (g_jstat_icount) {
            /* The percentage math is the one host-FP use outside guest FP:
             * fence it so the sticky FPSR flag accumulation never sees it. */
            fexcept_t saved;
            fegetexceptflag(&saved, FE_ALL_EXCEPT);
            dprintf(fd, " / %llu executed (%.2f%%)",
                    (unsigned long long)g_jstat_icount,
                    100.0 * (double)total / (double)g_jstat_icount);
            fesetexceptflag(&saved, FE_ALL_EXCEPT);
        }
        dprintf(fd, "\n");
        dprintf(fd, "[jit-stats] %u flushes, %llu translations, %llu dispatches\n",
                (unsigned)g_jstat_flushes, (unsigned long long)g_jstat_ntrans,
                (unsigned long long)g_jstat_ndisp);
        for (u32 i = 0; i < 32 && g_jstat[i].count; i++)
            dprintf(fd, "[jit-stats]  %12llu  %08x  %s\n",
                    (unsigned long long)g_jstat[i].count,
                    (unsigned)g_jstat[i].word, jstat_class(g_jstat[i].word));
        if (g_jstat_lost)
            dprintf(fd, "[jit-stats]  %12llu  (table overflow)\n",
                    (unsigned long long)g_jstat_lost);
    }
    if (g_jstat_path && fd >= 0) close(fd);
    pthread_mutex_unlock(&g_jstat_mu);
}

void jit_stats_flush(void) {
    if (jstat_on()) jstat_dump();
}

/* Decide, once per process, whether the statistics and the block dump are
 * on. */
static void jstat_settle(void) {
    pthread_mutex_lock(&g_jstat_mu);
    if (__atomic_load_n(&g_jit_stats, __ATOMIC_RELAXED) < 0) {
        const char *s = getenv(ARM64_JIT_ENV("STATS"));
        __atomic_store_n(&g_jit_stats, s != NULL, __ATOMIC_RELAXED);
        if (s) {
            if (s[0] == '/') g_jstat_path = s;
            else {
                g_jstat_fd = fcntl(2, F_DUPFD_CLOEXEC, 900);
                if (g_jstat_fd < 0) g_jstat_fd = 2;
            }
            atexit(jstat_dump);
        }
    }
    pthread_mutex_unlock(&g_jstat_mu);
}

/* ---- optional per-block code dump (ARM64_JIT_ENV("DUMP")=<prefix> or =1) ----
 * Every translated block's host bytes are pwritten into
 * <prefix>.<pid>.<tid>.code at the block's code-cache offset -- the file is
 * a sparse image of the cache, so chain/thunk jump targets in a disassembly
 * line up with file offsets -- plus one text line per block in
 * <prefix>.<pid>.<tid>.map (pc, offset, length, the guest words).
 * Disassemble a block with:
 *   objdump -D -b binary -m i386:x86-64 (or aarch64) \
 *     --start-address=0x<off> --stop-address=0x<off+len> <...>.code
 * After a cache flush offsets are reused; correlate .map lines in order. */
static const char *jdump_prefix(void) {
    static const char *prefix;
    static int read;
    if (!__atomic_load_n(&read, __ATOMIC_ACQUIRE)) {
        const char *d = getenv(ARM64_JIT_ENV("DUMP"));
        prefix = d ? ((d[0] && strcmp(d, "1") != 0) ? d : "jit-dump") : NULL;
        __atomic_store_n(&read, 1, __ATOMIC_RELEASE);
    }
    return prefix;
}

static void jdump_open(JitEnv *env, const char *prefix) {
    char name[256];
    unsigned long tid = (unsigned long)syscall(SYS_gettid);
    snprintf(name, sizeof name, "%s.%d.%lu.code", prefix, (int)getpid(), tid);
    env->jdump_code_fd = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    snprintf(name, sizeof name, "%s.%d.%lu.map", prefix, (int)getpid(), tid);
    env->jdump_map_fd = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (env->jdump_code_fd >= 0)             /* thunks live at offset 0 */
        (void)!pwrite(env->jdump_code_fd, env->cache_rw,
                      (size_t)(env->blocks_start_rw - env->cache_rw), 0);
}

static void jdump_block(JitEnv *env, CPU *c, JBlock *b, const u8 *rw_end,
                        const char *prefix) {
    if (env->jdump_code_fd < 0 && env->jdump_map_fd < 0) jdump_open(env, prefix);
    size_t off = (size_t)(b->code - env->cache_rx);
    size_t len = (size_t)(rw_end - (env->cache_rw + off));
    if (env->jdump_code_fd >= 0)
        (void)!pwrite(env->jdump_code_fd, env->cache_rw + off, len, (off_t)off);
    if (env->jdump_map_fd < 0) return;
    char line[JIT_MAX_BLOCK_INSNS * 9 + 96];
    int k = snprintf(line, sizeof line,
                     "pc=0x%llx off=0x%zx len=%zu ninsns=%u guest:",
                     (unsigned long long)b->pc, off, len, b->ninsns);
    for (u32 i = 0; i < b->ninsns && k < (int)sizeof line - 10; i++) {
        u32 w;
        if (!mem_ifetch(c, b->pc + 4 * i, &w)) break;
        k += snprintf(line + k, sizeof line - (size_t)k, " %08x", w);
    }
    line[k++] = '\n';
    (void)!write(env->jdump_map_fd, line, (size_t)k);
}

/* ---- code cache ---- */

static size_t jit_cache_size(void) {
    const char *s = getenv(ARM64_JIT_ENV("MB"));
    long mb = s ? atol(s) : 32;
    if (mb < 1) mb = 1;
    /* A backend's direct branch must span the whole cache (block -> epilogue,
     * and one block -> another when chained). */
    if (mb > JIT_CACHE_MAX_MB) mb = JIT_CACHE_MAX_MB;
    return (size_t)mb << 20;
}

/* RWX if the host allows it; else a dual-mapped memfd (RW + RX views of the
 * same pages) for W^X hosts. memfd_create may itself be unavailable or
 * blocked -- then the JIT degrades to the interpreter. */
static int cache_alloc(JitEnv *env) {
    size_t size = jit_cache_size();
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p != MAP_FAILED) {
        env->cache_rw = env->cache_rx = p;
        env->cache_size = size;
        return 0;
    }
#ifdef SYS_memfd_create
    /* The descriptor lives only until both views are mapped: the mappings
     * keep the pages. Its whole life is one jit_mode_private_fd bracket (in
     * user mode the guest shares the descriptor table, and must neither see
     * it nor have a sibling's fork hand its child a copy). */
    jit_mode_private_fd(1);
    int fd = (int)syscall(SYS_memfd_create, "a64-jit", 1 /*MFD_CLOEXEC*/);
    void *rw = MAP_FAILED, *rx = MAP_FAILED;
    if (fd >= 0 && ftruncate(fd, (off_t)size) == 0) {
        rw = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        rx = mmap(NULL, size, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
    }
    if (fd >= 0) close(fd);
    jit_mode_private_fd(0);
    if (rw != MAP_FAILED && rx != MAP_FAILED) {
        env->cache_rw = rw;
        env->cache_rx = rx;
        env->cache_size = size;
        return 0;
    }
    if (rw != MAP_FAILED) munmap(rw, size);
    if (rx != MAP_FAILED) munmap(rx, size);
#endif
    return -1;
}

static const u8 *rx_of(JitEnv *env, const u8 *rw) {
    return env->cache_rx + (rw - env->cache_rw);
}

/* ---- jump cache ---- */

/* The empty pattern (see JitEnv.jcache). */
#define JC_EMPTY_TAG (~0ULL)

static inline void jcache_empty(JitEnv *env, u32 i) {
    env->jcache[i].tag = JC_EMPTY_TAG;
    env->jcache[i].code = env->jc_miss_rx;
}

static void jcache_reset(JitEnv *env) {
    for (u32 i = 0; i < JIT_JC_SIZE; i++) jcache_empty(env, i);
    env->jc_ndirty = 0;
}

/* Empty what was filled since the last purge. */
static void jcache_purge(JitEnv *env) {
    if (env->jc_ndirty <= JIT_JC_DIRTY_MAX) {
        for (u32 i = 0; i < env->jc_ndirty; i++)
            jcache_empty(env, env->jc_dirty[i]);
        env->jc_ndirty = 0;
    } else {
        jcache_reset(env);
    }
}

static inline u32 jcache_index(u64 pc) {
    return (u32)(pc >> 2) & (JIT_JC_SIZE - 1);
}

/* Must only run from the dispatcher's C loop, or outside generated code,
 * never from a helper while a block is still on the native call stack (its
 * memory would be reused). */
static void jit_flush_all(JitEnv *env) {
    memset(env->hash, 0, JIT_HASH_SIZE * sizeof *env->hash);
    memset(env->pages, 0, JIT_PAGE_TBL * sizeof *env->pages);
    jcache_reset(env);
    env->nblocks = 0;
    env->nedges = 0;
    env->nfixups = 0;
    env->ptr = env->blocks_start_rw;
    env->flush_count++;
}

static void jit_env_destroy(JitEnv *env) {
    if (env->cache_rw) munmap(env->cache_rw, env->cache_size);
    if (env->cache_rx && env->cache_rx != env->cache_rw)
        munmap((void *)env->cache_rx, env->cache_size);
    if (env->jdump_code_fd >= 0) close(env->jdump_code_fd);
    if (env->jdump_map_fd >= 0) close(env->jdump_map_fd);
    free(env->hash);
    free(env->pages);
    free(env->arena);
    free(env->edges);
    free(env->fixups);
    free(env->ir);
    free(env->jstat);
    if (t_jstat_env == env) t_jstat_env = NULL;
    free(env);
}

/* What SCTLR_EL1 and CNTKCTL_EL1 open to EL0 (sysreg.c el0_allowed): UMA,
 * DZE, UCT, UCI; EL0PCTEN, EL0VCTEN, EL0VTEN, EL0PTEN. */
static inline u64 jit_el0cfg(const CPU *c) {
    return (c->sctlr[1] & ((1ULL << 9) | (1ULL << 14) | (1ULL << 15) | (1ULL << 26))) |
           ((c->cntkctl_el1 & 0x303) << 32);
}

static JitEnv *jit_env_new(CPU *c) {
    if (!be_available()) return NULL;
    JitEnv *env = calloc(1, sizeof *env);
    if (!env) return NULL;
    env->c = c;
    env->jdump_code_fd = env->jdump_map_fd = -1;
    env->helper_exec1 = (void *)jit_exec1;
    env->helper_exec1_ic = (void *)jit_exec1_ic;
    env->helper_ld = (void *)jit_ld;
    env->helper_st = (void *)jit_st;
    env->helper_ldv = (void *)jit_ldv;
    env->helper_stv = (void *)jit_stv;
    env->helper_spchk = (void *)jit_sp_check;
    env->dtlb = c->eng.dtlb;
    env->slowmem = getenv(ARM64_JIT_ENV("SLOWMEM")) != NULL;
    env->jc_epoch = c->eng.code_epoch;
    env->el0cfg = jit_el0cfg(c);
    jstat_settle();
    if (cache_alloc(env) < 0) { free(env); return NULL; }
    env->hash = calloc(JIT_HASH_SIZE, sizeof *env->hash);
    env->pages = calloc(JIT_PAGE_TBL, sizeof *env->pages);
    env->arena = malloc(JIT_MAX_BLOCKS * sizeof *env->arena);
    env->edges = malloc(2 * JIT_MAX_BLOCKS * sizeof *env->edges);
    /* A full system's guest memory is its own RAM, which never faults on
     * the host: only user mode records the inline accesses. */
    if (!c->eng.sys) env->fixups = malloc(JIT_MAX_FIXUPS * sizeof *env->fixups);
    if (!env->hash || !env->pages || !env->arena || !env->edges ||
        (!c->eng.sys && !env->fixups)) {
        jit_env_destroy(env);
        return NULL;
    }
    Emit e = { env->cache_rw, env->cache_rx,
               env->cache_rw + 4096, 0 };
    be_emit_thunks(&e, env);
    if (e.overflow) { jit_env_destroy(env); return NULL; }
    be_flush_icache(env->cache_rx, env->cache_rw,
                    (size_t)(e.rw - env->cache_rw));
    env->blocks_start_rw =
        env->cache_rw + (((e.rw - env->cache_rw) + 15) & ~15L);
    env->ptr = env->blocks_start_rw;
    env->end = env->cache_rw + env->cache_size;
    jcache_reset(env);                  /* needs jc_miss_rx, from the thunks */
    if (jit_mode_attach(c, env) != 0) { jit_env_destroy(env); return NULL; }
    env->active = 1;
    t_jstat_env = env;
    return env;
}

/* ---- block table ---- */

static inline u32 hash_tag(u64 tag) { return (u32)(tag >> 5) & (JIT_HASH_SIZE - 1); }

static JBlock *jit_lookup(JitEnv *env, u64 tag, const u8 *host_page) {
    JBlock *b = env->hash[hash_tag(tag)];
    while (b && (b->tag != tag || b->host_page != host_page))
        b = b->hash_next;
    return b;
}

/* ---- SMC thrash guard (by page key) ---- */
static void thrash_bump(JitEnv *env, u64 page) {
    unsigned s = (unsigned)(page >> 12) & (JIT_THRASH_SLOTS - 1);
    if (env->thrash[s].page != page) {
        env->thrash[s].page = page;
        env->thrash[s].count = 1;
    } else if (env->thrash[s].count < 0xffffffu) {
        env->thrash[s].count++;
    }
}
static int thrash_hot(JitEnv *env, u64 page) {
    unsigned s = (unsigned)(page >> 12) & (JIT_THRASH_SLOTS - 1);
    return env->thrash[s].page == page &&
           env->thrash[s].count >= JIT_THRASH_LIMIT;
}

/* Remove b from the hash table, unpatch every chained direct jump INTO it
 * back to its dispatcher stub, and empty its jump-cache entry: the only slot
 * a block is ever filled into, and only it holds b's code. The caller
 * removes b from its page list. b's code memory stays allocated, so a block
 * invalidating itself from a store helper can still run its own tail. */
static void jit_unlink_block(JitEnv *env, JBlock *b) {
    JBlock **hp = &env->hash[hash_tag(b->tag)];
    while (*hp && *hp != b) hp = &(*hp)->hash_next;
    if (*hp) *hp = b->hash_next;
    for (u32 ei = b->in_head; ei != ~0u; ) {
        JEdge *ed = &env->edges[ei];
        JBlock *from = &env->arena[ed->from];
        if (from->patched[ed->slot]) {
            be_unpatch_chain(env, from, ed->slot);
            be_flush_icache(from->code + from->exit_off[ed->slot],
                            env->cache_rw + (from->code - env->cache_rx) +
                                from->exit_off[ed->slot], 16);
        }
        ei = ed->next;
    }
    b->in_head = ~0u;
    u32 jci = jcache_index(b->pc);
    if (env->jcache[jci].code == b->code) jcache_empty(env, jci);
}

static void drop_page(JitEnv *env, u64 page) {
    JBlock **pp = &env->pages[(u32)(page >> 12) & (JIT_PAGE_TBL - 1)];
    while (*pp) {
        JBlock *b = *pp;
        if (b->page == page) {
            *pp = b->page_next;
            jit_unlink_block(env, b);
        } else {
            pp = &b->page_next;
        }
    }
}

void jit_drop_page(CPU *c, u64 page) {
    JitEnv *env = c->eng.jit;
    if (!env || !env->active) return;
    drop_page(env, page);
    thrash_bump(env, page);             /* count rewrites for the guard */
}

/* By the bounded block arena: a range too wide to visit page by page (a huge
 * munmap) must not flush unrelated hot code. Re-processing an already-
 * dropped block is idempotent (off the hash, no incoming edges). */
void jit_drop_range(CPU *c, u64 first, u64 npages) {
    JitEnv *env = c->eng.jit;
    if (!env || !env->active) return;
    u64 end = first + (npages << 12);
    for (u32 bi = 0; bi < env->nblocks; bi++) {
        JBlock *b = &env->arena[bi];
        if (b->page < first || b->page >= end) continue;
        JBlock **pp = &env->pages[(u32)(b->page >> 12) & (JIT_PAGE_TBL - 1)];
        while (*pp && *pp != b) pp = &(*pp)->page_next;
        if (*pp) *pp = b->page_next;
        jit_unlink_block(env, b);
    }
}

void jit_flush(CPU *c) {
    JitEnv *env = c->eng.jit;
    if (!env || !env->active) return;
    jit_flush_all(env);
    memset(env->thrash, 0, sizeof env->thrash);
}

void jit_cpu_release(CPU *c) {
    JitEnv *env = c->eng.jit;
    if (!env) return;
    if (jstat_on()) jstat_merge(env);
    jit_mode_detach(c, env);
    /* Withdrawn before it goes: a signal handler on this thread that loads
     * the pointer after this sees none, one that loaded it before has done
     * its store by the time this thread runs on. */
    __atomic_store_n(&c->eng.jit, NULL, __ATOMIC_RELEASE);
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    jit_env_destroy(env);
}

void jit_cpu_forked(CPU *c) {
    JitEnv *env = c->eng.jit;
    if (env) {
        __atomic_store_n(&c->eng.jit, NULL, __ATOMIC_RELEASE);
        __atomic_signal_fence(__ATOMIC_SEQ_CST);
        jit_env_destroy(env);
    }
    /* Read the way every other site reads it: a fork child is single-
     * threaded, but the thread the flag was cleared on, in the parent,
     * might have been clearing it at that very moment. */
    if (jstat_on()) {
        /* Report per process; the mutex may be held by a thread that did
         * not come across. */
        pthread_mutex_init(&g_jstat_mu, NULL);
        memset(g_jstat, 0, sizeof g_jstat);
        g_jstat_lost = g_jstat_icount = g_jstat_ntrans = g_jstat_ndisp = 0;
        g_jstat_flushes = 0;
    }
}

/* ---- helpers called from generated code ---- */

/* Execute one instruction with interpreter semantics; mirrors cpu_step's
 * ordering exactly (icount++ runs even when exec_a64 raised an abort).
 * Returns nonzero when the block must stop: control transfer (including a
 * synchronous exception vectoring), or the CPU asking for attention (stop,
 * WFI/WFE halt, an interrupt it unmasked, a recorded exception, a pending
 * signal: bounds delivery latency mid-block). */
u32 jit_exec1(CPU *c, u64 pc, u32 insn) {
    c->cur_insn_pc = pc;
    c->pc = pc + 4;
    exec_a64(c, insn);
    c->icount++;
    /* c->eng.jit is read on the rare paths only: held across the calls, it
     * would cost every helper call a saved register too. */
    if (UNLIKELY(jstat_on())) jstat_bump(c->eng.jit, insn);
    if (UNLIKELY(cpu_attn_pending(c))) return 1;
    /* MSR FPCR lands here (the system group is never inlined). Where the host
     * carries the mode, follow it immediately -- generated code chained after
     * this one must flush as the guest now asks. Where it does not, a mode
     * that newly matters invalidates the inline FP in every block already
     * translated, including the rest of this one, so end the block and let
     * jit_run's loop drop the cache before anything else runs. */
    fpcr_host_sync(c);
    if (UNLIKELY((jit_fpmode(c) & JIT_FPMODE_XLATE_MASK) &&
                 !(c->eng.jit->fpmode_seen & JIT_FPMODE_XLATE_MASK)))
        return 1;
    return c->pc != pc + 4;
}

/* IC IVAU, Xt: executes the instruction (a no-op in sysreg.c, kept for
 * fidelity), then lets the program drop what the line's page holds. Always
 * ends the block: the page may be the running block's own, and everything
 * after this point in it was fetched before the rewrite. */
u32 jit_exec1_ic(CPU *c, u64 pc, u32 insn) {
    u64 va = reg_x(c, insn & 31);
    jit_exec1(c, pc, insn);
    jit_mode_ic_ivau(c, va);
    return 1;
}

/* ---- memory slow paths (see jit_priv.h MDESC_*) ----
 * On a fault the exception has already been taken (vectored, or recorded
 * for the run loop); count the faulting instruction here -- it is excluded
 * from the block's batched icount, and the interpreter counts aborted
 * instructions (cpu_step runs icount++ unconditionally after exec_a64).
 *
 * Generated code calls these once its own inline D-TLB probe has missed, so
 * they go straight to mem.h's slow paths instead of probing the same entry
 * again; the slow paths are whole accesses, hits included, so this also
 * holds under ARM64_JIT_ENV("SLOWMEM"), which skips the inline probe. */

/* SP-alignment check (SCTLR_EL1.SA/SA0). Generated code calls this only when the
 * SP base is already known-misaligned (low 4 bits set). Raises an SP-alignment
 * fault and returns 1 (exit the block) when the check is enabled for the current
 * EL; returns 0 when it is off (the native access then proceeds). Counts the
 * faulting instruction like the mem helpers. */
u32 jit_sp_check(CPU *c, u64 pc) {
    c->cur_insn_pc = pc;
    unsigned bit = (c->el == 0) ? 4 : 3;      /* SCTLR_EL1.SA0 : .SA */
    if ((c->sctlr[1] >> bit) & 1) {
        cpu_raise_sync(c, esr_make(EC_SP_ALIGN, 0), 0);
        c->icount++;
        return 1;
    }
    return 0;
}

u32 jit_ld(CPU *c, u64 va, u64 pc, u32 desc) {
    c->cur_insn_pc = pc;
    unsigned rt = MDESC_RT(desc), sz = 1u << MDESC_SZLOG(desc);
    u64 v;
    if (!mem_read_slow(c, va, sz, &v)) { c->icount++; return 1; }
    if (MDESC_SIGN(desc)) {
        unsigned b = sz * 8;
        v = (u64)((s64)(v << (64 - b)) >> (64 - b));
    }
    if (!MDESC_IS64(desc)) v = (u32)v;
    if (MDESC_TMP(desc)) c->eng.jit->tmp_spill[rt & 3] = v;   /* IR temp home */
    else if (rt != 31) c->x[rt] = v;
    return 0;
}

u32 jit_st(CPU *c, u64 va, u64 val, u64 pc, u32 desc) {
    c->cur_insn_pc = pc;
    if (!mem_write_slow(c, va, 1u << MDESC_SZLOG(desc), val)) {
        c->icount++;
        return 1;
    }
    return 0;
}

u32 jit_ldv(CPU *c, u64 va, u64 pc, u32 desc) {
    c->cur_insn_pc = pc;
    unsigned rt = MDESC_RT(desc), bytes = 1u << MDESC_VSZL(desc);
    if (bytes == 16) {
        V128 v;
        if (!mem_read128_slow(c, va, &v)) { c->icount++; return 1; }
        c->v[rt] = v;
    } else {
        u64 t;
        if (!mem_read_slow(c, va, bytes, &t)) { c->icount++; return 1; }  /* zero-extended */
        c->v[rt].d[0] = t;
        c->v[rt].d[1] = 0;
    }
    return 0;
}

u32 jit_stv(CPU *c, u64 va, u64 pc, u32 desc) {
    c->cur_insn_pc = pc;
    unsigned rt = MDESC_RT(desc), bytes = 1u << MDESC_VSZL(desc);
    if (bytes == 16) {
        if (!mem_write128_slow(c, va, &c->v[rt])) { c->icount++; return 1; }
        return 0;
    }
    if (!mem_write_slow(c, va, bytes, c->v[rt].d[0])) { c->icount++; return 1; }
    return 0;
}

/* ---- translation ---- */

static JBlock *jit_translate(JitEnv *env, CPU *c, u64 pc, u64 tag,
                             const u8 *host_page, u64 page) {
    if (!env->ir) {
        env->ir = malloc(sizeof *env->ir);
        if (!env->ir) { jit_disable(); return NULL; }
    }
    u32 max_insns = JIT_MAX_BLOCK_INSNS;
retry:
    if (env->nblocks >= JIT_MAX_BLOCKS ||
        env->nedges + 2 > 2 * JIT_MAX_BLOCKS ||
        (size_t)(env->end - env->ptr) < JIT_BLOCK_MAX_BYTES)
        jit_flush_all(env);

    /* The entry fetch can legitimately fault (a jump to an unmapped or
     * non-executable page): the exception is taken like the interpreter's. */
    env->ir->fpmode = env->fpmode_seen;
    u32 n = jit_fe_block(c, pc, env->ir, max_insns);
    if (n == 0) return NULL;

    Emit e = { env->ptr, rx_of(env, env->ptr),
               env->ptr + JIT_BLOCK_MAX_BYTES, 0 };
    JBlock *b = &env->arena[env->nblocks];
    b->pc = pc;
    b->tag = tag;
    b->host_page = host_page;
    b->page = page;
    b->ninsns = n;
    b->code = e.rx;

    u32 fixups_before = env->nfixups;
    if (be_emit_block(&e, env, b, env->ir) < 0) {
        /* Discard the abandoned attempt's fixups along with its code, or the
         * table would stop being sorted by offset and the binary search in
         * jit_fault_fixup would go wrong. */
        env->nfixups = fixups_before;
        if (max_insns > 1 && n > 1) {   /* pathological block: shrink, retry */
            max_insns = n / 2;
            goto retry;
        }
        fprintf(stderr, "jit: emitter overflow, disabling the JIT\n");
        jit_disable();
        return NULL;
    }

    env->nblocks++;
    b->hash_next = env->hash[hash_tag(tag)];
    env->hash[hash_tag(tag)] = b;
    JBlock **ph = &env->pages[(u32)(page >> 12) & (JIT_PAGE_TBL - 1)];
    b->page_next = *ph;
    *ph = b;
    jit_mode_translated(c, b);

    be_flush_icache(b->code, env->ptr, (size_t)(e.rw - env->ptr));
    const char *dump = jdump_prefix();
    if (UNLIKELY(dump != NULL)) jdump_block(env, c, b, e.rw, dump);
    env->ptr = env->cache_rw + (((e.rw - env->cache_rw) + 15) & ~15L);
    env->ntrans++;
    return b;
}

/* ---- dispatch ---- */

/* Block context: EL0 flag, MMU enable, the active SP bank (sp_el[] is
 * banked by SPSel/EL -- the backends bake the bank's offset into generated
 * code), and the CPACR.FPEN trap state (bit 4: the frontend compiles FP/SIMD
 * as a trapping CALL1 under it, see fe_fptrap). All of these can only change
 * via CALL1-ended blocks (exceptions, ERET, MSR SPSel/CPACR), so they are
 * per-block constants; in user mode they never change at all. */
static inline u64 jit_ctx(const CPU *c) {
    unsigned spx = c->sp_sel ? c->el : 0;
    return (u64)((c->el == 0 ? 1u : 0u) | ((c->sctlr[1] & 1) ? 2u : 0u) |
                 ((u64)spx << 2) | (c->fp_trapped ? 16u : 0u));
}

static inline u64 jit_tag(u64 pc, u64 ctx) {
    /* pc bits 1:0 are zero -> 5 ctx bits fit under pc << 3; the 3 dropped
     * pc top bits are redundant sign-extension for canonical VAs. */
    return (pc << 3) | ctx;
}

/* Fetch-cache-based resolution of pc's code page. Returns 1 with host/page
 * set, 0 if the fetch faulted (exception taken) or the page is MMIO
 * (host = NULL). Mirrors mem_ifetch's tag discipline. The page key is the
 * physical page where the program models physical memory (a full system,
 * FetchCache.pa_page) and the virtual page where it does not. */
static int resolve_code_page(CPU *c, u64 pc, const u8 **host, u64 *page) {
    u64 vpage = pc & ~0xfffULL;
    FetchCache *f = &c->eng.fetch[0];   /* the current EL's */
    if (!(f->host && f->page == vpage)) {
        u32 w0;
        c->cur_insn_pc = pc;             /* precise IABORT ELR */
        if (!mem_ifetch_slow(c, pc, &w0)) return 0;   /* abort taken */
        if (!(f->host && f->page == vpage)) {
            *host = NULL;                /* executing from MMIO */
            return 1;
        }
    }
    *host = f->host;
    *page = c->eng.sys ? f->pa_page : vpage;
    return 1;
}

void jit_run(CPU *c, u64 deadline) {
    JitEnv *env = c->eng.jit;
    if (UNLIKELY(!env)) {
        env = jit_env_new(c);
        if (!env) {
            /* No code cache (W^X denial), or the program refused it. Either
             * way this CPU cannot run translated code safely, and the run
             * loop has no per-CPU fallback -- disable the JIT for the
             * process, as the emitter-overflow path does. */
            fprintf(stderr, "jit: cannot set the JIT up, using the "
                            "interpreter\n");
            jit_disable();
            return;
        }
        __atomic_store_n(&c->eng.jit, env, __ATOMIC_RELEASE);
    }

    env->icount_deadline = deadline;
    u32 flush_seen = env->flush_count;
    JBlock *prev = NULL;                /* chainable exit awaiting a target */
    int prev_slot = 0;

    /* Before every block, in this order: the JIT's own `interrupt` -- the
     * flag generated code checks at every block entry -- is cleared and what
     * it asked for done; then the run loop's attention and the deadline are
     * looked at (cpu.h, "Run control"). Whatever raised the flag with
     * something for the loop to answer raised the attention too, which only
     * the loop drops, so a request is seen here or, raised after this look,
     * raises the flag again and ends the block it lands in. The other order
     * left a window: a signal caught between the look and the clear lost its
     * flag and kept its attention unseen, and a block chained to itself then
     * spun on, deaf to the same signals arriving again, which are queued
     * once (tests/arm64chroot/fixtures/sigtramp.c). The interrupt lines are
     * asked directly as well: generated code unmasks inline (MSR DAIF,
     * DAIFClr: frontend.c), without the poll the interpreter's unmask
     * makes. */
    for (;;) {
        if (UNLIKELY(env->interrupt)) {
            __atomic_store_n(&env->interrupt, 0, __ATOMIC_RELAXED);
            __atomic_signal_fence(__ATOMIC_SEQ_CST);   /* clear, then look */
            if (jit_mode_resync(c, env)) jit_flush_all(env);
        }
        if (UNLIKELY(cpu_attn_pending(c) || cpu_irq_pending(c) ||
                     c->icount >= deadline))
            return;
        if (UNLIKELY(env->flush_count != flush_seen)) {
            flush_seen = env->flush_count;
            prev = NULL;                /* arena reset: pointer is stale */
        }
        /* Keep the host's own FZ/FZ16 on the guest's (a no-op where the host
         * has none): this covers what jit_exec1 cannot -- a CPU that came up
         * with a non-default FPCR, and an exception or signal return that
         * restored one. */
        fpcr_host_sync(c);
        /* First sight of a mode that changes what may be emitted: blocks
         * translated before it inlined FP that cannot honor it. Drop them
         * all; from here the frontend withdraws whatever this host cannot
         * do (ir.h's vop_fpcr_blocked). */
        if (UNLIKELY((jit_fpmode(c) & JIT_FPMODE_XLATE_MASK) &&
                     !(env->fpmode_seen & JIT_FPMODE_XLATE_MASK))) {
            env->fpmode_seen |= jit_fpmode(c);
            jit_flush_all(env);
            flush_seen = env->flush_count;
            prev = NULL;
        } else {
            env->fpmode_seen |= jit_fpmode(c);
        }
        /* The EL1 configuration that decides what EL0 may execute changed:
         * EL0 blocks were judged under the old one (frontend.c,
         * fe_sys_el0_refused). A guest kernel sets it once per CPU, so this
         * flushes at boot and hardly ever after; user mode never changes it.
         * The MSR that changes it ends its block, so the check always runs
         * before the next EL0 block. */
        if (UNLIKELY(jit_el0cfg(c) != env->el0cfg)) {
            env->el0cfg = jit_el0cfg(c);
            jit_flush_all(env);
            flush_seen = env->flush_count;
            prev = NULL;
        }
        if (UNLIKELY(env->jc_epoch != c->eng.code_epoch)) {
            /* TLB maintenance: VA->code bindings may be stale, and the
             * jcache is VA-keyed. Every epoch-changing path ends its block,
             * so the purge always runs before the next generated-code jcache
             * probe can hit. */
            env->jc_epoch = c->eng.code_epoch;
            jcache_purge(env);
        }

        u64 pc = c->pc;
        env->ndisp++;
        const u8 *hp;
        u64 page = 0;
        if (UNLIKELY(!resolve_code_page(c, pc, &hp, &page))) {
            prev = NULL;                /* fetch abort taken */
            continue;
        }
        if (UNLIKELY(!hp)) {            /* MMIO execution: interpret */
            prev = NULL;
            cpu_step(c);
            continue;
        }
        if (UNLIKELY(thrash_hot(env, page))) {
            /* Self-modifying hot page: interpret in place until control
             * leaves the page, so a rewrite loop can't thrash the
             * translator. cpu_step re-fetches live, exactly like the
             * interpreter. */
            prev = NULL;
            do {
                cpu_step(c);
            } while (!cpu_attn_pending(c) && !cpu_irq_pending(c) &&
                     c->icount < deadline &&
                     (c->pc & ~0xfffULL) == (pc & ~0xfffULL));
            continue;
        }

        u64 ctx = jit_ctx(c);
        env->ctx = ctx;                 /* jcache probes OR this into pc<<3 */
        /* Inline D-TLB probes OR these low tag bits into the VA page. Gen,
         * EL and MMU state can only change through block-exiting paths, so
         * refreshing here keeps every probe's compare current. */
        env->dtlb_ctxgen = c->eng.genbits | (ctx & 3);
        u64 tag = jit_tag(pc, ctx);
        JBlock *b = jit_lookup(env, tag, hp);
        if (!b) {
            b = jit_translate(env, c, pc, tag, hp, page);
            if (UNLIKELY(env->flush_count != flush_seen)) {
                flush_seen = env->flush_count;
                prev = NULL;
            }
            if (!b) {
                if (!jit_on()) return;  /* the JIT disabled itself */
                prev = NULL;            /* mid-block fetch fault taken */
                continue;
            }
        }

        /* Chain the exit that brought us here directly to b (see the header
         * for where that may cross a page). prev ran THIS iteration round,
         * so no guest state changed in between; skip if prev was invalidated
         * meanwhile. */
        if (prev && prev->exit_pc[prev_slot] == b->pc &&
            !prev->patched[prev_slot] &&
            ((prev->tag ^ b->tag) & 0x1f) == 0 &&    /* all 5 ctx bits */
            (!c->eng.sys || ((prev->pc ^ b->pc) & ~0xfffULL) == 0) &&
            jit_lookup(env, prev->tag, prev->host_page) == prev &&
            env->nedges < 2 * JIT_MAX_BLOCKS) {
            be_patch_chain(env, prev, prev_slot, b->code);
            be_flush_icache(prev->code + prev->exit_off[prev_slot],
                            env->cache_rw + (prev->code - env->cache_rx) +
                                prev->exit_off[prev_slot], 16);
            JEdge *ed = &env->edges[env->nedges];
            ed->from = (u32)(prev - env->arena);
            ed->slot = (u8)prev_slot;
            ed->next = b->in_head;
            b->in_head = env->nedges++;
        }

        /* Refill the indirect-branch cache: b was just verified (tag + host
         * page) for this dispatch, so a generated JMPIND probe may take the
         * same key straight to b->code until the entry is emptied. The key
         * is the one the backends compute (IRBlock.sys): a full system's
         * block tag, never zero-extended pc alone, and the pc in user
         * mode. */
        u32 jci = jcache_index(pc);
        env->jcache[jci].tag = c->eng.sys ? tag : pc;
        env->jcache[jci].code = b->code;
        if (env->jc_ndirty < JIT_JC_DIRTY_MAX)
            env->jc_dirty[env->jc_ndirty] = (u16)jci;
        if (env->jc_ndirty <= JIT_JC_DIRTY_MAX)   /* saturate, don't wrap */
            env->jc_ndirty++;

        u32 eid = env->enter(env, b->code);
        prev = NULL;
        if (eid != JIT_EXIT_NONE && (eid >> 1) < env->nblocks) {
            prev = &env->arena[eid >> 1];
            prev_slot = (int)(eid & 1);
        }
    }
}
