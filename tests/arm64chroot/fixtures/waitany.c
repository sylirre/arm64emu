/* A wait for ANY child, or for a process group, with a clone child about --
 * one forked with an exit signal other than SIGCHLD, which the kernel's
 * waits find only under __WCLONE (or __WALL), while a wait without either
 * finds only the other children (eligible_child). The host knows every guest
 * child as an ordinary fork, and the emulator used to hand such a wait to it
 * as it was: without __WCLONE it took the clone child -- the first ready, if
 * it was -- and with __WCLONE it found nothing, ECHILD.
 *
 * Covered: wait4(-1), wait4(0) and waitid(P_ALL) passing a dead clone child
 * by for an ordinary one still running, and finding none (ECHILD) once only
 * the clone child is left; __WCLONE finding the clone child and not the
 * ordinary ones; a wait made the moment fork returns; WNOHANG while the
 * ordinary child runs; and __WALL, which finds either.
 *
 * Self-checking: qemu-user forks for a clone child and gives it SIGCHLD, so
 * the expected block in run_tests.sh is the kernel's. */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef __WCLONE
#define __WCLONE 0x80000000
#endif
#ifndef __WALL
#define __WALL 0x40000000
#endif

static void nap(int ms) {
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&t, &t) && errno == EINTR) ;
}

/* A fork-like clone with its own exit signal (0: none). */
static pid_t clone_kid(int exitsig, int code, int ms) {
    long p = syscall(SYS_clone, (long)exitsig, 0L, 0L, 0L, 0L);
    if (p == 0) { nap(ms); _exit(code); }
    return (pid_t)p;
}
static pid_t kid(int code, int ms) {
    pid_t p = fork();
    if (p == 0) { nap(ms); _exit(code); }
    return p;
}

static const char *who(pid_t r, pid_t ck, pid_t k) {
    static char b[32];
    if (r == ck) return "clone child";
    if (r == k) return "ordinary child";
    if (r == 0) return "none ready";
    if (r < 0) {
        snprintf(b, sizeof b, "%s", errno == ECHILD ? "ECHILD" : strerror(errno));
        return b;
    }
    return "?";
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int st = 0;
    siginfo_t si;

    /* The clone child dies first; the ordinary one is still running. */
    pid_t ck = clone_kid(0, 9, 0);
    nap(100);
    pid_t k = kid(4, 200);
    pid_t r = wait4(-1, &st, 0, NULL);
    printf("wait4(-1): %s status=%d\n", who(r, ck, k), WEXITSTATUS(st));
    r = wait4(-1, &st, WNOHANG, NULL);
    printf("wait4(-1) with only the clone child: %s\n", who(r, ck, k));
    r = wait4(-1, &st, __WCLONE, NULL);
    printf("wait4(-1, __WCLONE): %s status=%d\n", who(r, ck, k), WEXITSTATUS(st));

    /* The same over waitid, the clone child with a signal of its own. */
    signal(SIGUSR1, SIG_IGN);
    ck = clone_kid(SIGUSR1, 8, 0);
    nap(100);
    k = kid(5, 200);
    memset(&si, 0, sizeof si);
    int e = waitid(P_ALL, 0, &si, WEXITED | WNOHANG);
    printf("waitid(P_ALL, WNOHANG): %s\n",
           e < 0 ? who(-1, ck, k) : who(si.si_pid, ck, k));
    memset(&si, 0, sizeof si);
    e = waitid(P_ALL, 0, &si, WEXITED);
    printf("waitid(P_ALL): %s status=%d\n",
           e < 0 ? who(-1, ck, k) : who(si.si_pid, ck, k), si.si_status);
    memset(&si, 0, sizeof si);
    e = waitid(P_ALL, 0, &si, WEXITED | __WCLONE);
    printf("waitid(P_ALL, __WCLONE): %s status=%d\n",
           e < 0 ? who(-1, ck, k) : who(si.si_pid, ck, k), si.si_status);

    /* A process-group wait, and __WCLONE with an ordinary child dead first. */
    ck = clone_kid(0, 6, 200);
    k = kid(3, 0);
    nap(100);
    r = wait4(0, &st, __WCLONE, NULL);
    printf("wait4(0, __WCLONE): %s status=%d\n", who(r, ck, k), WEXITSTATUS(st));
    r = wait4(0, &st, 0, NULL);
    printf("wait4(0): %s status=%d\n", who(r, ck, k), WEXITSTATUS(st));

    /* Waited for the moment fork returns, the clone child about. */
    ck = clone_kid(0, 2, 300);
    k = kid(1, 0);
    r = wait4(-1, &st, 0, NULL);
    printf("wait4(-1) at once: %s status=%d\n", who(r, ck, k), WEXITSTATUS(st));

    /* __WALL finds either kind: the clone child dies first, well before. */
    k = kid(0, 800);
    r = wait4(-1, &st, __WALL, NULL);
    printf("wait4(-1, __WALL): %s status=%d\n", who(r, ck, k), WEXITSTATUS(st));
    r = wait4(-1, &st, __WALL, NULL);
    printf("wait4(-1, __WALL) again: %s\n", who(r, ck, k));
    r = wait4(-1, &st, __WALL, NULL);
    printf("then: %s\n", who(r, ck, k));
    printf("done\n");
    return 0;
}
