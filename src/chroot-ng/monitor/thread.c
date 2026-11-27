/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* Guest threads as the host's own.
 *
 * A guest thread used to be the guest's clone, made by the kernel with nothing
 * of the host's libc around it: no thread pointer of the libc's, no stack of
 * ours, nothing for the libc's thread list to know. That is fine for a monitor
 * that calls nothing of the libc, and not for one that does -- a libc call
 * made from such a thread runs on the main thread's TCB. So the clone that
 * makes a thread is trapped (seccomp.c) and made here, as a pthread of the
 * host's on a stack of ours, which then becomes the guest's thread:
 *
 *  - the parent, in the SIGSYS handler, builds a start record -- the guest's
 *    registers at the trap, its FP/SIMD registers, its signal mask and thread
 *    pointer, the call's flags and arguments -- and calls pthread_create with
 *    every signal but SIGSYS blocked, which the new thread inherits;
 *  - the new thread registers its host context here (its TCB, its stack),
 *    gives back what its libc gave it that a kernel's new thread does not
 *    have -- an rseq area, a robust list, a clear_child_tid -- and takes the
 *    call's own: CLONE_CHILD_CLEARTID's address for clear_child_tid,
 *    CLONE_PARENT_SETTID's and CLONE_CHILD_SETTID's writes, a private fs or
 *    descriptor table without CLONE_FS or CLONE_FILES;
 *  - it reports its tid to the parent, which returns it as the call's result,
 *    and enters the guest by rt_sigreturn on a frame it built: the parent's
 *    registers with x0 = 0 and the new stack, the parent's FP/SIMD registers
 *    (a syscall leaves nothing of SVE beyond them), the parent's mask, and an
 *    alternate signal stack of ours (see below), with CLONE_SETTLS's thread
 *    pointer -- or the parent's -- in the register.
 *
 * The guest's robust list is the kernel's from there on, as the guest sets
 * it: it is not trapped, so the kernel processes it when the thread dies,
 * however it dies. So is its clear_child_tid where the host's libc leaves the
 * kernel's to it -- glibc's does, and the kernel's clear then comes where a
 * kernel's does, after the robust list. The kernel keeps one a thread, and
 * the other libcs want it for themselves: bionic's pthread_exit sets none
 * for a detached thread (ours are) before it ends it, and musl's names the
 * lock of its thread list, which the kernel's clear at the exit releases.
 * There the guest's is kept here -- CLONE_CHILD_CLEARTID's, then whatever
 * set_tid_address (trapped) asks for -- and cleared, with the kernel's wake,
 * as the thread leaves, before its libc ends it.
 *
 * A thread that exits (a trapped exit, or a sibling's exec killing it) does
 * not leave by the svc: it unwinds to the start routine, which returns, and
 * the libc ends the thread as it ends any of its own -- unlinks it, frees what
 * it allocated for it -- before the kernel's exit, which is where the guest's
 * robust list and clear_child_tid are processed. The leader (tid == pid) does
 * leave by the svc: its exit code is the group's, which the libc's last-thread
 * exit(0) would replace.
 *
 * The stack is reclaimed once the thread is gone (tgkill answers ESRCH): the
 * libc runs on it until the thread's last instruction.
 *
 * The alternate signal stack. A trapped exit needs somewhere for the SIGSYS
 * frame, and musl's __unmapself and bionic's thread teardown make that exit
 * on a stack they have just unmapped. So a guest thread of ours always has a
 * kernel alternate stack: the guest's own while it has one, ours otherwise
 * (sigsys.c, sigsys_altstack, which answers the guest with what it set).
 *
 * What the libc made for a thread of ours is the host's, not the guest's,
 * and an exec's sweep has to leave it (execve.c, exec_sweep) while the
 * thread lives -- a fork child's one thread, made by a thread of ours, goes
 * on as the process. glibc and musl make nothing: their thread record and
 * TLS are carved from the stack we give them. bionic carves its record
 * there too, and maps the rest: the TLS (since Android 10, guard pages
 * either side), a shadow call stack in a guard region of 16 MiB, and an
 * alternate signal stack, all made before the start routine and unmapped by
 * its pthread_exit. So each thread carries a mark inside each, an address in
 * no mapping of the guest's -- its thread pointer, its x18, the alternate
 * stack bionic set -- for the sweep to keep the mapping each lies in and the
 * guards either side of it.
 *
 * And a fork child of bionic's keeps every thread's record on its list of
 * threads, where its next pthread_create links the new one in: the other
 * threads' stacks stay mapped in the child for good. glibc's and musl's fork
 * child forget the other threads, and those stacks are given back. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <unistd.h>

#include "cng/loader.h"
#include "cng/monitor.h"
#include "cng/ownmap.h"
#include "cng/ptrace.h"
#include "cng/rt.h"
#include "cng/syscall.h"
#include "cng/uapi.h"
#include "cng/ucontext.h"

#ifdef __GLIBC__
#define THR_CTID_KERNEL 1 /* the guest's clear_child_tid is the kernel's */
#else
#define THR_CTID_KERNEL 0 /* ...or ours (see the top) */
#endif

#define THR_N      1024
#define THR_STACK  (256UL * 1024) /* the pthread's: its TCB, the start routine */
#define THR_ALT    (64UL * 1024)  /* the alternate signal stack, below it */

/* What the handler's entry switches to (hentry.S reads the two words). */
struct cng_hctx {
    unsigned long tp, x18;
};

struct cng_ctx {
    unsigned long w[22];
};
extern long cng_ctx_save(struct cng_ctx *c);
extern void cng_ctx_jump(const struct cng_ctx *c) __attribute__((noreturn));

struct cng_thr {
    struct cng_hctx h;   /* first: what cng_hentry_ctx hands the entry */
    long tid;            /* 0: never used, -1: given back */
    unsigned long map, maplen; /* our mapping, guards included (0: none) */
    unsigned long alt;   /* its alternate signal stack's base, THR_ALT long */
    struct cng_ctx back; /* where an exit unwinds to */
    cng_stack_t gss;     /* the guest's alternate stack, as it set it */
    int pthread;         /* a guest thread of ours, which unwinds to exit */
    unsigned long ctid;  /* the guest's clear_child_tid, kept here where it is
                            not the kernel's (THR_CTID_KERNEL) */
    unsigned long host[3]; /* inside what its libc mapped for it: the TLS,
                              the shadow call stack, the alternate stack (the
                              top); 0: none, or inside `map` */
};
static struct cng_thr g_thr[THR_N];

/* Every other thread's: the main thread's, as main() found it (rt.c), or in
 * a fork child the one thread's. */
static struct cng_hctx g_main_hctx;

void cng_thread_main(unsigned long tp, unsigned long x18) {
    g_main_hctx.tp = tp;
    g_main_hctx.x18 = x18;
}

/* Stacks of threads that have left their start routine, until they are gone. */
#define DEAD_N 256
static struct {
    long tid;
    unsigned long map, len;
} g_dead[DEAD_N];

static unsigned thr_hash(long tid) {
    return (unsigned)((unsigned long)tid * 2654435761u) % THR_N;
}

static struct cng_thr *thr_find(long tid) {
    unsigned h = thr_hash(tid);
    for (unsigned k = 0; k < THR_N; k++) {
        struct cng_thr *t = &g_thr[(h + k) % THR_N];
        long v = __atomic_load_n(&t->tid, __ATOMIC_ACQUIRE);
        if (v == tid)
            return t;
        if (v == 0)
            return 0;
    }
    return 0;
}

/* A record of its own for `tid`. A free slot is one never used (the probe
 * above ends at it) or one given back, which is marked -1 so the probe goes on
 * past it, and taken again here. */
static struct cng_thr *thr_claim(long tid) {
    unsigned h = thr_hash(tid);
    for (int pass = 0; pass < 2; pass++)
        for (unsigned k = 0; k < THR_N; k++) {
            struct cng_thr *t = &g_thr[(h + k) % THR_N];
            long v = __atomic_load_n(&t->tid, __ATOMIC_ACQUIRE);
            if (v == tid)
                return t;
            if ((v == 0 || (pass && v == -1)) &&
                __atomic_compare_exchange_n(&t->tid, &v, tid, 0,
                                            __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
                return t;
        }
    return 0;
}

static int thr_in_map(const struct cng_thr *t, unsigned long a) {
    return a - t->map < t->maplen;
}

int cng_thread_host_mark(unsigned long lo, unsigned long hi) {
    for (int i = 0; i < THR_N; i++) {
        if (__atomic_load_n(&g_thr[i].tid, __ATOMIC_ACQUIRE) <= 0)
            continue;
        for (int k = 0; k < 3; k++) {
            unsigned long a = g_thr[i].host[k];
            if (a && a >= lo && a < hi)
                return 1;
        }
    }
    return 0;
}

static void thr_free(struct cng_thr *t) {
    t->h.tp = t->h.x18 = t->map = t->maplen = t->alt = 0;
    t->host[0] = t->host[1] = t->host[2] = 0;
    t->ctid = 0;
    t->pthread = 0;
    __atomic_store_n(&t->tid, -1, __ATOMIC_RELEASE);
}

/* Called by the entry on the interrupted context: nothing here may lean on
 * the thread pointer, which is the guest's. */
const struct cng_hctx *cng_hentry_ctx(void) {
    struct cng_thr *t = thr_find(sys_gettid());
    return t && t->h.tp ? &t->h : &g_main_hctx;
}

unsigned long cng_thread_alt(unsigned long *size) {
    struct cng_thr *t = thr_find(sys_gettid());
    if (!t || !t->alt)
        return 0;
    *size = THR_ALT;
    return t->alt;
}

/* The stacks of threads that are gone, given back. */
static void thr_reclaim(void) {
    long pid = sys_getpid();
    for (int i = 0; i < DEAD_N; i++) {
        long tid = __atomic_load_n(&g_dead[i].tid, __ATOMIC_ACQUIRE);
        if (tid <= 0 || CNG_SYS(__NR_tgkill, pid, tid, 0, 0, 0, 0) != -ESRCH)
            continue;
        if (!__atomic_compare_exchange_n(&g_dead[i].tid, &tid, -1, 0,
                                         __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
            continue;
        cng_own_drop((void *)g_dead[i].map, g_dead[i].len);
        sys_munmap((void *)g_dead[i].map, g_dead[i].len);
        __atomic_store_n(&g_dead[i].tid, 0, __ATOMIC_RELEASE);
    }
}

static void thr_bury(long tid, unsigned long map, unsigned long len) {
    for (int i = 0; i < DEAD_N; i++) {
        long z = 0;
        if (__atomic_compare_exchange_n(&g_dead[i].tid, &z, -1, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            g_dead[i].map = map;
            g_dead[i].len = len;
            __atomic_store_n(&g_dead[i].tid, tid, __ATOMIC_RELEASE);
            return;
        }
    }
    /* Full: this one stays mapped, which is all a full table costs. */
}

void cng_thread_reclaim(void) { thr_reclaim(); }

/* Threads of ours on their way in: counted from before the pthread_create
 * (cng_thread_clone) until the new thread has told its parent how it went
 * and is on its way into the guest (thr_start), with SIGSYS held off the
 * whole way, in the parent and in the child -- which the libcs' own
 * pthread_create does for every signal over the clone anyway, in the parent
 * and in the child until its start, and a thread caught there by an exec's
 * de_thread looked like one that can never take the request, which is given
 * up on after a few milliseconds of it -- not long under arm64emu, whose new
 * thread has its translations to make first. Held off by us so that a
 * request sent meanwhile is delivered once neither thread needs the other:
 *
 *  - the parent must not die of one inside the pthread_create, holding the
 *    libc's locks there (its stack list, the malloc arena) for every later
 *    thread to wait on, or between the count and the call, which would leave
 *    the count up for good; nor while it waits for the child's word, since
 *    what the child starts from (struct thr_start) is on the parent's
 *    scratch stack, which a death there unmaps (cng_scratch_release) while
 *    the child is still reading it: a SIGSEGV in thr_start, the whole
 *    process with it (Android 8's M6 chain shape, about one run in two,
 *    where a slow core leaves the new thread waiting to run that long);
 *  - and the child must not die of one before it has given that word, or
 *    the parent, holding SIGSYS off for it, would wait for good.
 *
 * The child's comes back with the guest's mask, by the rt_sigreturn into the
 * guest (cng_thread_enter), and a request sent meanwhile is taken there, at
 * the guest's first instruction, and unwinds the thread to its start. */
static int g_thr_starting;

/* Is `tid` possibly a thread of ours on its way in or out, which holds SIGSYS
 * off for a moment and is to be waited for rather than given up on (execve.c,
 * de_thread)? On its way out: buried, and not gone yet. On its way in: any
 * thread at all while one is starting, as a starting one has no record yet
 * and the parent making it is blocked too. */
int cng_thread_unsettled(long tid) {
    if (__atomic_load_n(&g_thr_starting, __ATOMIC_SEQ_CST) > 0)
        return 1;
    for (int i = 0; i < DEAD_N; i++)
        if (__atomic_load_n(&g_dead[i].tid, __ATOMIC_ACQUIRE) == tid)
            return 1;
    return 0;
}

/* The flags our alternate stack is armed with: SS_AUTODISARM where the kernel
 * has it (4.7). A handler of ours leaves the alternate stack for its scratch
 * stack, and has it disarmed while it is away, or a signal delivered to it
 * there would be put at the alternate stack's top, over the frames of the
 * handler it interrupted (sigsys.c, scr_run). Disarmed for the handler's
 * length by the kernel itself, the stack needs nothing more from it. Before
 * 4.7 the flag is refused -- and a frame carrying it would fail rt_sigreturn
 * -- and the handler disarms ours as it does a guest's own. */
static int g_alt_flags, g_alt_probed;

/* Asked once, at startup (cng_thread_warmup), before anything runs on an
 * alternate stack. */
static void thr_alt_probe(void) {
    static char probe[16384] __attribute__((aligned(16)));
    cng_stack_t old, try = {probe, CNG_SS_AUTODISARM, sizeof probe};
    if (CNG_SYS(__NR_sigaltstack, (long)&try, (long)&old, 0, 0, 0, 0) == 0) {
        g_alt_flags = CNG_SS_AUTODISARM;
        CNG_SYS(__NR_sigaltstack, (long)&old, 0, 0, 0, 0, 0);
    }
    g_alt_probed = 1;
}

static int thr_alt_flags(void) {
    if (!g_alt_probed)
        thr_alt_probe();
    return g_alt_flags;
}

/* ---- the new thread ------------------------------------------------------ */

struct thr_start {
    struct cng_uregs r;         /* the guest's registers at the trap */
    unsigned char fp[528];      /* its FP/SIMD registers: user_fpsimd_state */
    unsigned long mask;         /* its signal mask */
    unsigned long gtp;          /* its thread pointer */
    unsigned long flags;
    unsigned long map, maplen;  /* the mapping made for the thread */
    int state;                  /* 0 while starting, then the tid or -errno */
};

/* The frame rt_sigreturn takes (struct rt_sigframe): a siginfo, then the
 * ucontext, whose record list needs an FPSIMD record and a terminator. */
struct thr_frame {
    cng_siginfo_t info;
    struct cng_ucontext uc;
};

extern void cng_thread_enter(struct thr_frame *f, unsigned long tp)
    __attribute__((noreturn));

static void thr_report(struct thr_start *s, int v) {
    __atomic_store_n(&s->state, v, __ATOMIC_RELEASE);
    CNG_SYS(__NR_futex, &s->state, 1 /* FUTEX_WAKE */, 1, 0, 0, 0);
}

static void *thr_start(void *arg) {
    struct thr_start *s = arg;
    long tid = sys_gettid();
    struct cng_thr *t = thr_claim(tid);
    if (!t) {
        thr_bury(tid, s->map, s->maplen);
        thr_report(s, -EAGAIN);
        __atomic_sub_fetch(&g_thr_starting, 1, __ATOMIC_SEQ_CST);
        return 0;
    }
    __asm__ volatile("mrs %0, tpidr_el0\n\tmov %1, x18"
                     : "=r"(t->h.tp), "=r"(t->h.x18));
    t->map = s->map;
    t->maplen = s->maplen;
    /* The marks (see the top): a kernel's new thread has no alternate stack,
     * so one here is the libc's; x18 is the shadow call stack's only where
     * the libc keeps one there. */
    cng_stack_t hss;
    unsigned long m[3] = {t->h.tp, 0, 0};
#ifdef __BIONIC__
    m[1] = t->h.x18;
#endif
    if (CNG_SYS(__NR_sigaltstack, 0, (long)&hss, 0, 0, 0, 0) == 0 &&
        !(hss.ss_flags & CNG_SS_DISABLE) && hss.ss_size)
        m[2] = (unsigned long)hss.ss_sp + hss.ss_size - 1;
    for (int k = 0; k < 3; k++)
        t->host[k] = m[k] && !thr_in_map(t, m[k]) ? m[k] : 0;
    t->alt = s->map + cng_page_size;
    t->gss.ss_sp = 0;
    t->gss.ss_flags = CNG_SS_DISABLE;
    t->gss.ss_size = 0;
    t->pthread = 1;
    if (cng_ctx_save(&t->back)) {
        /* An exit, unwound to here (cng_thread_exit): off the scratch stack
         * now, which goes back, and this one with the thread.
         *
         * SIGSYS is held off from here to the end, and the thread buried
         * first, so that an exec's de_thread waits for it (it is unsettled
         * now: cng_thread_unsettled) instead of telling it again. Told, it
         * died where it stood -- with no record left to unwind by, the raw
         * exit -- and where it stood was its libc's exit, which frees the
         * thread's malloc cache under the arena lock before it blocks the
         * signals itself: the lock stayed held, and every malloc and free
         * after it waited for good, a pthread_create of ours and the next
         * exiting thread's among them (m6's chain shape on the x86 runner,
         * threads left behind by the exec one run in four). */
        unsigned long sys = 1UL << (CNG_SIGSYS - 1);
        CNG_SYS(__NR_rt_sigprocmask, 0 /*SIG_BLOCK*/, (long)&sys, 0, sizeof sys,
                0, 0);
        long me = sys_gettid();
        struct cng_thr *u = thr_find(me);
        if (u)
            thr_bury(me, u->map, u->maplen);
        cng_scratch_release(me);
        if (u) {
            if (u->ctid) { /* the kernel's mm_release, for the guest's */
                u32 z = 0;
                if (cng_user_copyout((void *)u->ctid, &z, sizeof z) == 0)
                    CNG_SYS(__NR_futex, u->ctid, 1 /* FUTEX_WAKE */, 1, 0, 0, 0);
            }
            thr_free(u);
        }
        return 0;
    }
    /* Started: an exit has somewhere to unwind to now. SIGSYS stays held off,
     * as it has been since the parent's pthread_create (g_thr_starting),
     * until the parent has been told how it went and the guest's mask comes
     * back with the guest (see the top): a de_thread request sent meanwhile
     * is taken there. */

    /* What its libc set up that a kernel's new thread has none of. The
     * robust list is given to the kernel as the call asked, none (where the
     * host has the call at all: Android refuses it), and clear_child_tid is
     * CLONE_CHILD_CLEARTID's address -- the kernel's, or ours (see the top). */
    unsigned long flags = s->flags;
    struct cng_uregs r = s->r;
    unsigned long ctid = (flags & CNG_CLONE_CHILD_CLEARTID) ? r.x[4] : 0;
    cng_rseq_drop_host();
    if (!cng_blocked[__NR_set_robust_list])
        CNG_SYS(__NR_set_robust_list, 0, 24, 0, 0, 0, 0);
#if THR_CTID_KERNEL
    CNG_SYS(__NR_set_tid_address, ctid, 0, 0, 0, 0, 0);
#else
    t->ctid = ctid;
#endif
    long e = 0;
    if (!(flags & CNG_CLONE_FS))
        e = CNG_SYS(__NR_unshare, CNG_CLONE_FS, 0, 0, 0, 0, 0);
    if (!e && !(flags & CNG_CLONE_FILES))
        e = CNG_SYS(__NR_unshare, CNG_CLONE_FILES, 0, 0, 0, 0, 0);
    if (e) {
        thr_bury(tid, t->map, t->maplen);
        thr_free(t);
        thr_report(s, (int)e);
        __atomic_sub_fetch(&g_thr_starting, 1, __ATOMIC_SEQ_CST);
        return 0;
    }
    u32 me32 = (u32)tid;
    if (flags & CNG_CLONE_PARENT_SETTID)
        cng_user_copyout((void *)r.x[2], &me32, sizeof me32);
    if (flags & CNG_CLONE_CHILD_SETTID)
        cng_user_copyout((void *)r.x[4], &me32, sizeof me32);

    /* The frame, from the parent's state, before the parent is told: its
     * record is on the parent's stack, and gone once it is. */
    struct thr_frame frame __attribute__((aligned(16)));
    struct thr_frame *f = &frame;
    memset(f, 0, sizeof *f);
    for (int i = 0; i < 31; i++)
        f->uc.uc_mcontext.regs[i] = r.x[i];
    f->uc.uc_mcontext.regs[0] = 0;
    f->uc.uc_mcontext.sp = r.x[1] ? r.x[1] : r.sp;
    f->uc.uc_mcontext.pc = r.pc;
    f->uc.uc_mcontext.pstate = r.pstate;
    f->uc.uc_sigmask.sig[0] = s->mask;
    f->uc.uc_stack.ss_sp = (void *)t->alt;
    f->uc.uc_stack.ss_size = THR_ALT;
    f->uc.uc_stack.ss_flags = thr_alt_flags();
    unsigned char *rec = f->uc.uc_mcontext.__reserved;
    *(u32 *)rec = 0x46508001;                 /* FPSIMD_MAGIC */
    *(u32 *)(rec + 4) = 528;
    memcpy(rec + 8, s->fp + 512, 4);          /* fpsr */
    memcpy(rec + 12, s->fp + 516, 4);         /* fpcr */
    memcpy(rec + 16, s->fp, 512);             /* v0..v31 */
    /* the terminator: zeroed by the memset */
    unsigned long tp = (flags & CNG_CLONE_SETTLS) ? r.x[3] : s->gtp;

    /* `s` is not touched again: the parent may be gone with it once told.
     * Uncounted before the guest's mask comes back: the unwind a request
     * then makes does not come back here. */
    thr_report(s, (int)tid);
    __atomic_sub_fetch(&g_thr_starting, 1, __ATOMIC_SEQ_CST);
    cng_thread_enter(f, tp);
}

long cng_thread_clone(const struct cng_uregs *ur, const void *fp,
                      unsigned long mask, unsigned long gtp) {
    unsigned long flags = ur->x[0];
    /* The kernel's own refusals, and the one shape there is no thread for: a
     * process sharing the address space, which is not a thread. */
    if ((flags & CNG_CLONE_THREAD) && !(flags & CNG_CLONE_SIGHAND))
        return -EINVAL;
    if ((flags & CNG_CLONE_SIGHAND) && !(flags & CNG_CLONE_VM))
        return -EINVAL;
    if (!(flags & CNG_CLONE_THREAD))
        return -EINVAL;
    thr_reclaim();

    unsigned long g = cng_page_size, len = THR_ALT + THR_STACK + 2 * g;
    void *m = sys_mmap(0, len, CNG_PROT_NONE, CNG_MAP_PRIVATE | CNG_MAP_ANONYMOUS,
                       -1, 0);
    if (m == CNG_MAP_FAILED || cng_is_err((long)m))
        return -EAGAIN;
    unsigned long base = (unsigned long)m;
    if (sys_mprotect((void *)(base + g), THR_ALT + THR_STACK,
                     CNG_PROT_READ | CNG_PROT_WRITE) < 0) {
        sys_munmap(m, len);
        return -EAGAIN;
    }
    cng_own_map(m, len);

    struct thr_start s;
    memset(&s, 0, sizeof s);
    s.r = *ur;
    if (fp)
        memcpy(s.fp, fp, sizeof s.fp);
    s.mask = mask;
    s.gtp = gtp;
    s.flags = flags;
    s.map = base;
    s.maplen = len;

    /* glibc's first pthread_create installs the handlers of its own two
     * signals, SIGCANCEL and SIGSETXID (32 and 33, late_init), over whatever
     * is there -- the guest's, whose own libc uses the same two for the same
     * things (a setuid of a threaded program broadcasts SIGSETXID), and whose
     * dispositions they are. The monitor makes no call of the libc's that
     * needs them, so the guest's are put back, as they were. */
    unsigned long sa_was[2][4];
    int had[2];
    for (int k = 0; k < 2; k++)
        had[k] = CNG_SYS(__NR_rt_sigaction, 32 + k, 0, sa_was[k], 8, 0, 0) == 0;

    pthread_attr_t attr;
    pthread_t pt;
    pthread_attr_init(&attr);
    pthread_attr_setstack(&attr, (void *)(base + g + THR_ALT), THR_STACK);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    /* SIGSYS held off, and the thread counted as starting (g_thr_starting),
     * until the child has said how it went: `s` is on this stack. The child
     * inherits the mask and enters the guest with the guest's. */
    unsigned long sys = 1UL << (CNG_SIGSYS - 1), was;
    CNG_SYS(__NR_rt_sigprocmask, 0 /*SIG_BLOCK*/, (long)&sys, (long)&was,
            sizeof sys, 0, 0);
    __atomic_add_fetch(&g_thr_starting, 1, __ATOMIC_SEQ_CST);
    int err = pthread_create(&pt, &attr, thr_start, &s);
    if (err)
        __atomic_sub_fetch(&g_thr_starting, 1, __ATOMIC_SEQ_CST);
    pthread_attr_destroy(&attr);
    for (int k = 0; k < 2; k++)
        if (had[k])
            CNG_SYS(__NR_rt_sigaction, 32 + k, sa_was[k], 0, 8, 0, 0);
    /* A thread that started and refused has buried its own stack, given
     * back once it is gone. */
    int v = 0;
    while (!err && (v = __atomic_load_n(&s.state, __ATOMIC_ACQUIRE)) == 0)
        CNG_SYS(__NR_futex, &s.state, 0 /* FUTEX_WAIT */, 0, 0, 0, 0);
    CNG_SYS(__NR_rt_sigprocmask, 2 /*SIG_SETMASK*/, (long)&was, 0, sizeof was,
            0, 0);
    if (err) {
        cng_own_drop(m, len);
        sys_munmap(m, len);
        return -err;
    }
    if (cng_g_debug)
        cng_dprintf(2, "[cng] thread %d (flags %lx)\n", v, flags);
    return v;
}

/* ---- leaving -------------------------------------------------------------- */

int cng_thread_unwinds(void) {
    long tid = sys_gettid();
    if (tid == sys_getpid())
        return 0;
    struct cng_thr *t = thr_find(tid);
    return t && t->pthread;
}

void cng_thread_exit(void) {
    struct cng_thr *t = thr_find(sys_gettid());
    cng_scratch_gone();
    cng_ctx_jump(&t->back);
}

/* A fork child has one thread, the one that forked, whose host context is
 * this one's: keyed by its new tid, and the fallback for anything else. The
 * other threads' stacks are copies the child has no thread for, buried
 * under the parent's tids, which no thread of the child's answers to; the
 * parent's own buried ones likewise -- but for bionic's, whose list of
 * threads still holds their records, on those stacks (see the top). */
void cng_thread_fork_child(void) {
    long tid = sys_gettid();
    g_thr_starting = 0; /* the parent's, starting there: none here */
    unsigned long tp, x18;
    __asm__ volatile("mrs %0, tpidr_el0\n\tmov %1, x18" : "=r"(tp), "=r"(x18));
    struct cng_thr keep;
    int have = 0;
    memset(&keep, 0, sizeof keep);
    for (int i = 0; i < THR_N; i++) {
        if (g_thr[i].tid <= 0)
            continue;
        if (g_thr[i].h.tp == tp && !have) {
            keep = g_thr[i];
            keep.ctid = 0; /* the leader's is the kernel's (dispatch.c) */
            have = 1;
            continue;
        }
#ifdef __BIONIC__
        continue; /* kept, and ours for good: still own-mapped */
#endif
        if (g_thr[i].map)
            thr_bury(g_thr[i].tid, g_thr[i].map, g_thr[i].maplen);
    }
    for (int i = 0; i < THR_N; i++)
        g_thr[i].tid = 0;
    cng_host_tp = tp;
    g_main_hctx.tp = tp;
    g_main_hctx.x18 = have ? keep.h.x18 : x18;
    if (have) {
        struct cng_thr *t = thr_claim(tid);
        if (t) {
            keep.tid = tid;
            *t = keep;
        }
    }
}

/* sigaltstack, for a guest thread of ours (sigsys.c, sigsys_altstack, has the
 * frame's side of it). The guest is answered with the stack it set; the
 * kernel has that one while there is one, and ours while there is none (see
 * the top), which the frame's uc_stack installs at sigreturn. 1 with the
 * result in *ret, 0 for a thread that is not one of ours. */
int cng_thread_altstack(struct cng_ucontext *uc, const void *uss, void *uoss,
                        long *ret) {
    struct cng_thr *t = thr_find(sys_gettid());
    if (!t || !t->alt)
        return 0;
    cng_stack_t old = t->gss, neu;
    unsigned long sp = uc->uc_mcontext.sp;
    int on = !(old.ss_flags & CNG_SS_DISABLE) && old.ss_size &&
             sp > (unsigned long)old.ss_sp &&
             sp - (unsigned long)old.ss_sp <= old.ss_size;
    if (old.ss_flags & CNG_SS_DISABLE) {
        old.ss_sp = 0;
        old.ss_size = 0;
        old.ss_flags = CNG_SS_DISABLE;
    } else {
        old.ss_flags = (old.ss_flags & CNG_SS_AUTODISARM) |
                       (on ? CNG_SS_ONSTACK : 0);
    }
    if (uss) {
        if (cng_user_copyin(&neu, uss, sizeof neu) < 0) {
            *ret = -EFAULT;
            return 1;
        }
        if (on) {
            *ret = -EPERM;
            return 1;
        }
        int mode = neu.ss_flags & ~CNG_SS_AUTODISARM;
        if (mode != 0 && mode != CNG_SS_DISABLE && mode != CNG_SS_ONSTACK) {
            *ret = -EINVAL;
            return 1;
        }
        cng_stack_t ours = {(void *)t->alt, thr_alt_flags(), THR_ALT};
        if (mode == CNG_SS_DISABLE) {
            t->gss.ss_sp = 0;
            t->gss.ss_size = 0;
            t->gss.ss_flags = CNG_SS_DISABLE;
            uc->uc_stack = ours;
        } else {
            /* The kernel judges the size; it is ours again after. */
            cng_stack_t now, off = {0, CNG_SS_DISABLE, 0};
            long r = CNG_SYS(__NR_sigaltstack, (long)&neu, 0, 0, 0, 0, 0);
            if (r < 0) {
                *ret = r;
                return 1;
            }
            if (CNG_SYS(__NR_sigaltstack, 0, (long)&now, 0, 0, 0, 0) == 0) {
                t->gss = now;
                uc->uc_stack = now;
            }
            CNG_SYS(__NR_sigaltstack, (long)&off, 0, 0, 0, 0, 0);
        }
    }
    if (uoss && cng_user_copyout(uoss, &old, sizeof old) < 0) {
        *ret = -EFAULT;
        return 1;
    }
    *ret = 0;
    return 1;
}

/* set_tid_address (dispatch.c): kept for a thread of ours where the
 * guest's clear_child_tid is not the kernel's (see the top), the kernel's
 * call otherwise -- the leader's, any thread's under glibc. The caller's tid
 * either way. */
long cng_thread_tid_address(unsigned long addr) {
#if !THR_CTID_KERNEL
    long tid = sys_gettid();
    if (tid != sys_getpid()) {
        struct cng_thr *t = thr_find(tid);
        if (t && t->pthread) {
            t->ctid = addr;
            return tid;
        }
    }
#endif
    return CNG_SYS(__NR_set_tid_address, addr, 0, 0, 0, 0, 0);
}

/* The libc's fork(), from the SIGSYS handler (dispatch.c, cng_clone_convert):
 * the child's pid, 0 in the child, or -errno. The handler interrupted the
 * guest, never the libc, so no lock of the libc's is this thread's. */
long cng_host_fork(void) {
    pid_t p = fork();
    return p < 0 ? -(long)errno : (long)p;
}

/* glibc's deferred thread setup (late_init: its SIGCANCEL and SIGSETXID
 * handlers), done before the guest runs rather than at its first thread, where
 * it would land on the guest's dispositions for a moment: a thread that does
 * nothing, joined. The entry's reset of the dispositions then takes the two
 * handlers back to the default (execve.c, cng_entry_reset). Bionic has none
 * to do. */
#ifdef __GLIBC__
static void *thr_noop(void *a) { return a; }
#endif

void cng_thread_warmup(void) {
    thr_alt_probe();
#ifdef __GLIBC__
    pthread_attr_t attr;
    pthread_t t;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64 * 1024);
    if (pthread_create(&t, &attr, thr_noop, 0) == 0)
        pthread_join(t, 0);
    pthread_attr_destroy(&attr);
#endif
}
