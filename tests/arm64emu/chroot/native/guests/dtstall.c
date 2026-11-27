/* dtstall [-s] MS PROG ARGS... -- run PROG under ptrace, every thread of it,
 * and hold each de_thread request (a SIGSYS with si_code SI_DETHREAD, which
 * only the native tier's emulated execve sends) at its delivery for MS milliseconds
 * before letting it through; every other signal goes through untouched (M6).
 *
 * The exec tells each sibling to die with that signal and waits for it to be
 * gone. A delivered request is no longer pending, so one sent after it is
 * delivered too, on top -- a signal frame each, on the thread's alternate
 * stack (bionic's is 16 KiB) or on its own. It used to be sent every 100 us
 * round to every thread still listed, and a thread the scheduler left waiting
 * at the handler's first instruction collected a frame a round until there
 * was no room for the next one, and the kernel killed the whole process with
 * a SIGSEGV: a few runs in a hundred on a slow device. Holding the delivery
 * is that wait, every time.
 *
 * With -s, each new thread (and process) is held instead, for MS
 * milliseconds at the stop it starts in, while every other stop goes on being
 * served: the thread that made it is back from the clone meanwhile, waiting
 * for its child's word, which is where an exec's de_thread found the parent
 * under the native tier, and killed it while the child was still reading what the
 * parent had left it on a stack that went with the parent: a SIGSEGV in the
 * new thread, the whole process with it, about one run in two on a slow
 * device whose scheduler left the new thread waiting to run as long.
 *
 * Exits with PROG's status (128 + the signal that killed it), or prints a
 * lone SKIP line when the host will not let it trace its own child. Runs
 * where AArch64 binaries run directly: a host tracer under qemu-user sees
 * the emulator's signals, not the guest's.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef SI_DETHREAD /* glibc names it; bionic and musl do not */
#define SI_DETHREAD (-7)
#endif

/* -s: the threads held at their start, each until its time comes. */
#define HOLD_N 4096
static struct {
    pid_t tid;
    struct timespec due;
} held[HOLD_N];
static int nheld;

static int is_due(const struct timespec *d) {
    struct timespec n;
    clock_gettime(CLOCK_MONOTONIC, &n);
    return n.tv_sec > d->tv_sec ||
           (n.tv_sec == d->tv_sec && n.tv_nsec >= d->tv_nsec);
}

static void hold(pid_t tid, long ms) {
    clock_gettime(CLOCK_MONOTONIC, &held[nheld].due);
    held[nheld].due.tv_sec += ms / 1000;
    held[nheld].due.tv_nsec += (ms % 1000) * 1000000L;
    if (held[nheld].due.tv_nsec >= 1000000000L) {
        held[nheld].due.tv_sec++;
        held[nheld].due.tv_nsec -= 1000000000L;
    }
    held[nheld++].tid = tid;
}

/* Let go of those whose time has come, and of `gone` (0: none), which is. */
static void release(pid_t gone) {
    for (int i = 0; i < nheld;) {
        if (held[i].tid == gone || is_due(&held[i].due)) {
            if (held[i].tid != gone)
                ptrace(PTRACE_CONT, held[i].tid, 0, 0);
            held[i] = held[--nheld];
        } else {
            i++;
        }
    }
}

int main(int argc, char **argv) {
    int starts = argc > 1 && !strcmp(argv[1], "-s");
    argv += starts;
    argc -= starts;
    if (argc < 3) {
        fprintf(stderr, "usage: dtstall [-s] MS PROG ARGS...\n");
        return 2;
    }
    long ms = atol(argv[1]);
    pid_t c = fork();
    if (c < 0)
        return 1;
    if (c == 0) {
        if (ptrace(PTRACE_TRACEME, 0, 0, 0) != 0)
            _exit(125);
        raise(SIGSTOP);
        execvp(argv[2], argv + 2);
        _exit(127);
    }
    int st;
    if (waitpid(c, &st, 0) != c || !WIFSTOPPED(st)) {
        puts("SKIP: cannot trace a child here");
        return 0;
    }
    ptrace(PTRACE_SETOPTIONS, c, 0,
           PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK | PTRACE_O_TRACECLONE |
               PTRACE_O_TRACEEXEC | PTRACE_O_EXITKILL);
    ptrace(PTRACE_CONT, c, 0, 0);
    int rc = 1;
    for (;;) {
        if (nheld)
            release(0);
        pid_t p = waitpid(-1, &st, __WALL | (nheld ? WNOHANG : 0));
        if (p == 0) { /* none stopped, some held: look again shortly */
            struct timespec nap = {0, 100 * 1000L};
            nanosleep(&nap, 0);
            continue;
        }
        if (p < 0)
            break;
        if (WIFEXITED(st) || WIFSIGNALED(st)) {
            if (p == c)
                rc = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
            if (nheld)
                release(p);
            continue;
        }
        int sig = WSTOPSIG(st);
        /* The stop a new thread starts in: held with -s, and nothing to
         * deliver either way. */
        if (!(st >> 16) && sig == SIGSTOP && starts && nheld < HOLD_N) {
            hold(p, ms);
            continue;
        }
        /* An event stop, or that start: nothing to deliver. */
        if ((st >> 16) || sig == SIGSTOP) {
            ptrace(PTRACE_CONT, p, 0, 0);
            continue;
        }
        if (sig == SIGSYS && !starts) {
            siginfo_t si;
            memset(&si, 0, sizeof si);
            ptrace(PTRACE_GETSIGINFO, p, 0, &si);
            if (si.si_code == SI_DETHREAD) {
                struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};
                nanosleep(&t, 0);
            }
        }
        ptrace(PTRACE_CONT, p, 0, sig);
    }
    return rc;
}
