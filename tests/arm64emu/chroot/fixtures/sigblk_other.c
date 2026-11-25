/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* SigBlk of another task's /proc status: the guest's blocked set, every bit
 * of it, whichever task the file describes (src/arm64emu/chroot/proctab.c, "blocked
 * sets"). The emulator's own host numbers are never in a host mask, so the
 * host's line showed a task that blocks everything as leaving a dozen signals
 * open -- chroot-ng, run in a guest, took that for "my kick signal gets
 * through" and waited for ever on an attach that never happened.
 *
 * Self-checking: qemu-user answers these reads with the host's line too. The
 * expected block is what a kernel prints (checked natively); masks are set
 * with the raw system call, whose set no C library trims. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned long long blk_of(const char *path) {
    unsigned long long v = 0xdead;
    FILE *f = fopen(path, "r");
    char l[4096];
    while (f && fgets(l, sizeof l, f))
        if (!strncmp(l, "SigBlk:", 7)) v = strtoull(l + 7, NULL, 16);
    if (f) fclose(f);
    return v;
}
static unsigned long long blk_task(pid_t pid, pid_t tid) {
    char p[96];
    snprintf(p, sizeof p, "/proc/%d/task/%d/status", (int)pid, (int)tid);
    return blk_of(p);
}
static unsigned long long blk_pid(pid_t pid) {
    char p[64];
    snprintf(p, sizeof p, "/proc/%d/status", (int)pid);
    return blk_of(p);
}
static void setmask(unsigned long long m) {
    syscall(SYS_rt_sigprocmask, SIG_SETMASK, &m, NULL, 8);
}
static void handler(int s) { (void)s; }

struct shm { volatile int ready, go, tid[2]; };
static struct shm *sh;

static void *thr(void *a) {
    long i = (long)a;
    setmask(i ? 0x8000000080000010ull : ~0ull);   /* RT 64, 32 and SIGTRAP */
    sh->tid[i] = (int)syscall(SYS_gettid);
    __atomic_add_fetch(&sh->ready, 1, __ATOMIC_SEQ_CST);
    while (!sh->go) usleep(1000);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "sleep")) {
        for (;;) pause();
    }
    sh = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);

    /* A child blocks everything, and later nothing at all. */
    pid_t k = fork();
    if (k == 0) {
        setmask(~0ull);
        sh->ready = 1;
        while (!sh->go) usleep(1000);
        setmask(0);
        sh->ready = 2;
        for (;;) usleep(1000);
    }
    while (sh->ready != 1) usleep(1000);
    printf("child_all=%016llx\n", blk_pid(k));
    sh->go = 1;
    while (sh->ready != 2) usleep(1000);
    printf("child_none=%016llx\n", blk_pid(k));
    kill(k, SIGKILL);
    waitpid(k, NULL, 0);

    /* Inherited at the fork: read before the child can have run much. */
    setmask(0xc000000080000010ull);
    k = fork();
    if (k == 0) for (;;) usleep(1000);
    printf("at_fork=%016llx\n", blk_pid(k));
    kill(k, SIGKILL);
    waitpid(k, NULL, 0);

    /* Kept across execve. */
    memset(sh, 0, sizeof *sh);
    k = fork();
    if (k == 0) {
        execl("/proc/self/exe", "x", "sleep", NULL);
        _exit(1);
    }
    for (int i = 0; i < 2000; i++) {          /* until it is the new image */
        char p[64], b[64] = "";
        snprintf(p, sizeof p, "/proc/%d/cmdline", (int)k);
        FILE *f = fopen(p, "r");
        if (f) { size_t n = fread(b, 1, sizeof b - 1, f); (void)n; fclose(f); }
        if (!strcmp(b, "x")) break;
        usleep(1000);
    }
    printf("after_exec=%016llx\n", blk_pid(k));
    kill(k, SIGKILL);
    waitpid(k, NULL, 0);
    setmask(0);

    /* A sigsuspend's temporary mask is the blocked set while it lasts. */
    k = fork();
    if (k == 0) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = handler;
        sigaction(SIGUSR1, &sa, NULL);
        setmask(~0ull);
        unsigned long long m = ~0ull & ~(1ull << (SIGUSR1 - 1));
        sh->ready = 1;
        syscall(SYS_rt_sigsuspend, &m, 8);
        sh->ready = 2;
        for (;;) usleep(1000);
    }
    while (sh->ready != 1) usleep(1000);
    unsigned long long v = 0;
    for (int i = 0; i < 2000; i++) {
        v = blk_pid(k);
        if (v != 0xfffffffffffbfeffull) break;
        usleep(1000);
    }
    printf("suspended=%016llx\n", v);
    kill(k, SIGUSR1);
    while (sh->ready != 2) usleep(1000);
    printf("resumed=%016llx\n", blk_pid(k));
    kill(k, SIGKILL);
    waitpid(k, NULL, 0);

    /* Threads: a sibling's, and those of another process. */
    for (int other = 0; other < 2; other++) {
        memset(sh, 0, sizeof *sh);
        k = other ? fork() : getpid();
        if (!other || k == 0) {
            pthread_t t[2];
            for (long i = 0; i < 2; i++) pthread_create(&t[i], NULL, thr, (void *)i);
            if (!other) {
                while (sh->ready != 2) usleep(1000);
                printf("sibling=%016llx %016llx main=%016llx\n", blk_task(k, sh->tid[0]),
                       blk_task(k, sh->tid[1]), blk_pid(k));
                sh->go = 1;
            }
            pthread_join(t[0], NULL);
            pthread_join(t[1], NULL);
            if (other) _exit(0);
            continue;
        }
        while (sh->ready != 2) usleep(1000);
        printf("foreign=%016llx %016llx main=%016llx\n", blk_task(k, sh->tid[0]),
               blk_task(k, sh->tid[1]), blk_pid(k));
        sh->go = 1;
        waitpid(k, NULL, 0);
    }
    return 0;
}
