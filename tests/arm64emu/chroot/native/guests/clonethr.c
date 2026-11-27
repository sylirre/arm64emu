/* A thread, made by a raw clone(2) the way each libc and runtime makes one,
 * and what it starts with: the kernel's answers, which the native tier gives too now
 * that it makes the thread itself (monitor/thread.c). Every line names what was
 * asked and what came of it; the expected block in the milestone is a
 * kernel's.
 *
 *   - Go's shape: no CLONE_SETTLS, no tid words -- the child has the parent's
 *     thread pointer; and glibc's, with CLONE_SETTLS and both tid words;
 *   - CLONE_PARENT_SETTID's word written before the call returns, the
 *     CLONE_CHILD_SETTID one in the child, and CLONE_CHILD_CLEARTID's cleared
 *     with a futex wake when the child exits;
 *   - without CLONE_FS, a chdir of the child's is its own; without
 *     CLONE_FILES, a close of the child's is its own (the chdir is to "tmp",
 *     relative: the milestone makes one in the directory the program starts
 *     in, the guest's root and the reference run's cwd alike, where a host
 *     need not have a /tmp -- Android 14 has none);
 *   - the parent's signal mask, inherited, and no alternate signal stack;
 *   - x0 = 0 in the child and the stack it was given;
 *   - a robust mutex whose owner thread exits holding it: EOWNERDEAD (not
 *     on bionic, which has none). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

/* clone(flags, stack, ptid, tls, ctid); the child calls fn(arg) on the new
 * stack and exits with its result. */
long raw_clone(unsigned long flags, void *stack, int *ptid, unsigned long tls,
               int *ctid, int (*fn)(void *), void *arg);
__asm__(".globl raw_clone\n"
        "raw_clone:\n"
        "    and  x1, x1, #~15\n"
        "    stp  x5, x6, [x1, #-16]!\n"   /* fn, arg on the child's stack */
        "    mov  x8, #220\n"              /* __NR_clone */
        "    svc  #0\n"
        "    cbz  x0, 1f\n"
        "    ret\n"
        "1:  ldp  x5, x0, [sp], #16\n"
        "    blr  x5\n"
        "    mov  x8, #93\n"               /* __NR_exit */
        "    svc  #0\n"
        "    brk  #0\n");

static void *stk(void) {
    char *p = mmap(0, 1 << 16, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    return p + (1 << 16);
}

static volatile int done;
static void wait_done(void) {
    while (!__atomic_load_n(&done, __ATOMIC_ACQUIRE))
        syscall(SYS_futex, &done, FUTEX_WAIT, 0, 0, 0, 0);
    done = 0;
}
static void set_done(void) {
    __atomic_store_n(&done, 1, __ATOMIC_RELEASE);
    syscall(SYS_futex, &done, FUTEX_WAKE, 1, 0, 0, 0);
}

static unsigned long child_tp, parent_tp;
static long child_x0 = -1;
static unsigned long child_sp;
static unsigned long child_mask;
static int child_alt;
static long child_tid;

static int probe(void *arg) {
    (void)arg;
    unsigned long tp, sp;
    __asm__ volatile("mrs %0, tpidr_el0\n mov %1, sp" : "=r"(tp), "=r"(sp));
    child_tp = tp;
    child_sp = sp;
    child_x0 = 0;
    child_tid = syscall(SYS_gettid);
    unsigned long m = 0;
    syscall(SYS_rt_sigprocmask, SIG_BLOCK, 0, &m, 8);
    child_mask = m;
    stack_t ss;
    child_alt = syscall(SYS_sigaltstack, 0, &ss) == 0 && (ss.ss_flags & SS_DISABLE);
    set_done();
    return 0;
}

static int chdir_child(void *arg) {
    (void)arg;
    syscall(SYS_chdir, "tmp");
    set_done();
    return 0;
}

static int close_child(void *arg) {
    syscall(SYS_close, (long)(int)(long)arg);
    set_done();
    return 0;
}

#ifndef __BIONIC__
static pthread_mutex_t rm;
static int robust_child(void *arg) {
    (void)arg;
    pthread_mutex_lock(&rm);
    set_done();
    return 0; /* exits holding it */
}
#endif

#define BASE (CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD | \
              CLONE_SYSVSEM)

int main(void) {
    setvbuf(stdout, 0, _IONBF, 0);
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(parent_tp));

    /* Go's shape. */
    void *s = stk();
    sigset_t blk;
    sigemptyset(&blk);
    sigaddset(&blk, SIGUSR1);
    sigprocmask(SIG_BLOCK, &blk, 0);
    long r = raw_clone(BASE, s, 0, 0, 0, probe, 0);
    wait_done();
    printf("go-style: %s, child x0 %s, its tp %s, its stack %s, mask %s, "
           "alt stack %s\n",
           r > 0 ? "a thread" : strerror((int)-r),
           child_x0 == 0 ? "0" : "other",
           child_tp == parent_tp ? "the parent's" : "other",
           child_sp < (unsigned long)s && child_sp > (unsigned long)s - 65536
               ? "the one given" : "other",
           (child_mask & (1UL << (SIGUSR1 - 1))) ? "inherited" : "not inherited",
           child_alt ? "none" : "one");
    printf("go-style: the tid returned is the child's: %s\n",
           r == child_tid ? "yes" : "no");
    sigprocmask(SIG_UNBLOCK, &blk, 0);

    /* glibc's shape: CLONE_SETTLS and the tid words. */
    static int ptid, ctid = -1;
    static unsigned long tlsblock[64];
    s = stk();
    r = raw_clone(BASE | CLONE_SETTLS | CLONE_PARENT_SETTID | CLONE_CHILD_SETTID |
                      CLONE_CHILD_CLEARTID,
                  s, &ptid, (unsigned long)&tlsblock[8], &ctid, probe, 0);
    int ptid_now = ptid;
    wait_done();
    /* The child is on its way out: CHILD_CLEARTID's word goes to 0 with a
     * wake once it is gone. */
    for (int v; (v = __atomic_load_n(&ctid, __ATOMIC_ACQUIRE)) != 0;)
        syscall(SYS_futex, &ctid, FUTEX_WAIT, v, 0, 0, 0);
    printf("glibc-style: tp %s, PARENT_SETTID %s, CHILD_CLEARTID %s\n",
           child_tp == (unsigned long)&tlsblock[8] ? "the one set" : "other",
           ptid_now == r ? "written before the return" : "not written",
           ctid == 0 ? "cleared and woken" : "left");

    /* CHILD_SETTID alone: the child's own word. */
    static int ct2;
    s = stk();
    r = raw_clone(BASE | CLONE_CHILD_SETTID, s, 0, 0, &ct2, probe, 0);
    wait_done();
    printf("CHILD_SETTID: %s\n", ct2 == r ? "the child's tid" : "not written");

    /* No CLONE_FS: the child's cwd is its own. */
    char before[256], after[256];
    getcwd(before, sizeof before);
    s = stk();
    r = raw_clone(BASE & ~CLONE_FS, s, 0, 0, 0, chdir_child, 0);
    wait_done();
    usleep(10000);
    getcwd(after, sizeof after);
    printf("without CLONE_FS: %s, the parent's cwd %s\n",
           r > 0 ? "a thread" : strerror((int)-r),
           strcmp(before, after) ? "moved" : "kept");
    /* Back where it was, which under the native tier it is not (one guest cwd a
     * process), or the next leg would find it moved already. */
    chdir(before);
    s = stk();
    r = raw_clone(BASE, s, 0, 0, 0, chdir_child, 0);
    wait_done();
    usleep(10000);
    getcwd(after, sizeof after);
    printf("with CLONE_FS: the parent's cwd %s\n",
           strcmp(before, after) ? "moved with it" : "kept");
    chdir(before);

    /* No CLONE_FILES: the child's close is its own. */
    int fd = open("/dev/null", O_RDONLY);
    s = stk();
    r = raw_clone(BASE & ~CLONE_FILES, s, 0, 0, 0, close_child, (void *)(long)fd);
    wait_done();
    usleep(10000);
    printf("without CLONE_FILES: %s, the parent's fd %s\n",
           r > 0 ? "a thread" : strerror((int)-r),
           fcntl(fd, F_GETFD) >= 0 ? "kept" : "closed");
    s = stk();
    r = raw_clone(BASE, s, 0, 0, 0, close_child, (void *)(long)fd);
    wait_done();
    usleep(10000);
    printf("with CLONE_FILES: the parent's fd %s\n",
           fcntl(fd, F_GETFD) >= 0 ? "kept" : "closed with it");

    /* The kernel's refusals. */
    printf("CLONE_THREAD without CLONE_SIGHAND: %s\n",
           raw_clone(CLONE_VM | CLONE_THREAD, stk(), 0, 0, 0, probe, 0) == -EINVAL
               ? "EINVAL" : "other");
    printf("CLONE_SIGHAND without CLONE_VM: %s\n",
           raw_clone(CLONE_SIGHAND | SIGCHLD, stk(), 0, 0, 0, probe, 0) == -EINVAL
               ? "EINVAL" : "other");

#ifndef __BIONIC__
    /* A robust mutex left held by a thread that exited. */
    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_setrobust(&ma, PTHREAD_MUTEX_ROBUST);
    pthread_mutex_init(&rm, &ma);
    pthread_t t;
    pthread_create(&t, 0, (void *(*)(void *))(void *)robust_child, 0);
    pthread_join(t, 0);
    int e = pthread_mutex_lock(&rm);
    printf("a robust mutex its owner thread exited holding: %s\n",
           e == EOWNERDEAD ? "EOWNERDEAD" : strerror(e));
#endif
    return 0;
}
