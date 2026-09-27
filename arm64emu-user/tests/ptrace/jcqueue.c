/* Self-checking: a queued job-control signal's siginfo for a traced process
 * must arrive as it was sent -- at the tracee's signal-delivery stop
 * (PTRACE_GETSIGINFO) and, once the tracer hands the signal on, in the
 * tracee's handler: its code, its si_errno and its payload, aimed at the
 * process and at a thread alike. The emulator carries such a signal on its
 * kick (signal.c, sig_send_jc); on a host that cannot carry a private si_code
 * (the known-layout tier, qemu-user) it goes through the receiver's inbox
 * (sig_carry_send), and the suite runs it there too. */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef PTRACE_SEIZE
#define PTRACE_SEIZE 0x4206
#endif
#ifndef PTRACE_EVENT_STOP
#define PTRACE_EVENT_STOP 128
#endif

struct rep { int sig, code, err, value, pid; };

static int pfd[2];

static void handler(int s, siginfo_t *si, void *uc) {
    (void)uc;
    struct rep r = { s, si->si_code, si->si_errno, si->si_value.sival_int,
                     (int)si->si_pid };
    (void)!write(pfd[1], &r, sizeof r);
}

static int fail(int n) { printf("FAIL %d\n", n); return n; }

/* The next stop of `k` that delivers `sig`. A SIGCONT to a SEIZEd tracee is
 * notified first as a PTRACE_EVENT_STOP (prepare_signal's
 * ptrace_trap_notify), which is passed over. */
static int stop_of(pid_t k, int sig) {
    for (;;) {
        int st;
        if (waitpid(k, &st, __WALL) != k || !WIFSTOPPED(st)) return 0;
        if (sig == SIGCONT && (st >> 16) == PTRACE_EVENT_STOP) {
            if (ptrace(PTRACE_CONT, k, 0, 0) < 0) return 0;
            continue;
        }
        return WSTOPSIG(st) == sig && (st >> 16) == 0;
    }
}

static long queue(pid_t k, int thread, int sig, int err, int val) {
    siginfo_t si;
    memset(&si, 0, sizeof si);
    si.si_signo = sig;
    si.si_errno = err;
    si.si_code = SI_QUEUE;
    si.si_pid = getpid();
    si.si_uid = getuid();
    si.si_value.sival_int = val;
    return thread ? syscall(SYS_rt_tgsigqueueinfo, k, k, sig, &si)
                  : syscall(SYS_rt_sigqueueinfo, k, sig, &si);
}

static int round_trip(pid_t k, int thread, int sig, int err, int val, int base) {
    if (queue(k, thread, sig, err, val) != 0) return fail(base);
    if (!stop_of(k, sig)) return fail(base + 1);
    siginfo_t si;
    memset(&si, 0, sizeof si);
    if (ptrace(PTRACE_GETSIGINFO, k, 0, &si) < 0 || si.si_signo != sig ||
        si.si_code != SI_QUEUE || si.si_errno != err ||
        si.si_value.sival_int != val || si.si_pid != getpid()) {
        printf("stop: signo=%d code=%d errno=%d value=%d pid=%d\n", si.si_signo,
               si.si_code, si.si_errno, si.si_value.sival_int, (int)si.si_pid);
        return fail(base + 2);
    }
    if (ptrace(PTRACE_CONT, k, 0, (void *)(long)sig) < 0) return fail(base + 3);
    struct rep r;
    if (read(pfd[0], &r, sizeof r) != sizeof r || r.sig != sig ||
        r.code != SI_QUEUE || r.err != err || r.value != val || r.pid != getpid()) {
        printf("handler: sig=%d code=%d errno=%d value=%d pid=%d\n", r.sig, r.code,
               r.err, r.value, r.pid);
        return fail(base + 4);
    }
    return 0;
}

int main(void) {
    if (pipe(pfd) < 0) return fail(99);
    pid_t k = fork();
    if (k == 0) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = handler;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigaction(SIGTSTP, &sa, NULL);
        sigaction(SIGCONT, &sa, NULL);
        for (;;) {
            struct timespec ts = { 0, 5000000 };
            nanosleep(&ts, NULL);
        }
    }
    struct timespec nap = { 0, 50000000 };
    nanosleep(&nap, NULL);
    if (ptrace(PTRACE_SEIZE, k, 0, 0) < 0) return fail(1);
    int r;
    if ((r = round_trip(k, 0, SIGTSTP, 5, 77, 10)) ||
        (r = round_trip(k, 1, SIGTSTP, 0, 88, 20)) ||
        (r = round_trip(k, 0, SIGCONT, 3, 99, 30)) ||
        (r = round_trip(k, 1, SIGCONT, 12, -7, 40)))
        goto out;
    printf("OK\n");
out:
    kill(k, SIGKILL);
    waitpid(k, NULL, __WALL);
    return r;
}
