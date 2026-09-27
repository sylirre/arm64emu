/* A receiver that blocks a signal for longer than the siginfo inbox's stale
 * age (signal.c, sig_carry_send; proctab.c, proctab_carry_post) while a
 * sender goes on queueing past the inbox's 64 slots. Every instance must
 * arrive with its own value -- a slot taken back while its signal still
 * waited hands the guest the carrier's token in its place -- and the ones
 * the inbox held with their si_errno. Past the inbox, a host that cannot
 * carry an si_errno (qemu-user) loses it, as it did before the inbox: that
 * is not looked at. Run for a real-time signal and for a standard one (whose
 * later instances coalesce, as on a kernel).
 *
 * Self-checking. The ARM32 tier's qemu-user shows another process's pending
 * real-time signals in /proc at numbers other than its guest's, which is
 * what made the inbox reclaim live slots there (sig_procpnd_trusted); the
 * suite's (known-layout-tier) row runs the inbox on any host. */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static long sq(pid_t k, int sig, int err, long val) {
    siginfo_t si;
    memset(&si, 0, sizeof si);
    si.si_signo = sig;
    si.si_code = SI_QUEUE;
    si.si_errno = err;
    si.si_pid = getpid();
    si.si_uid = getuid();
    si.si_value.sival_ptr = (void *)val;
    return syscall(SYS_rt_sigqueueinfo, k, sig, &si);
}

static void one(const char *label, int sig) {
    sigset_t b, prev;
    sigemptyset(&b);
    sigaddset(&b, sig);
    sigprocmask(SIG_BLOCK, &b, &prev);
    int go[2];
    if (pipe(go)) return;
    pid_t k = fork();
    if (k == 0) {
        char c;
        (void)!read(go[0], &c, 1);
        struct timespec z = { 0, 0 };
        siginfo_t g;
        int n = 0, wrong = 0, errlost = 0;
        while (sigtimedwait(&b, &g, &z) == sig) {
            if ((long)g.si_value.sival_ptr != n) wrong++;
            else if (n < 64 && g.si_errno != 5) errlost++;
            n++;
        }
        printf("%s: received %d, wrong value %d, errno lost in the inbox %d\n",
               label, n, wrong, errlost);
        _exit(0);
    }
    usleep(200000);
    int fails = 0;
    for (int i = 0; i < 64; i++) fails += sq(k, sig, 5, i) != 0;
    struct timespec n = { 2, 400000000 };   /* past the stale age */
    nanosleep(&n, NULL);
    for (int i = 64; i < 80; i++) fails += sq(k, sig, 5, i) != 0;
    if (fails) printf("%s: %d sends failed\n", label, fails);
    (void)!write(go[1], "x", 1);
    waitpid(k, NULL, 0);
    close(go[0]);
    close(go[1]);
    sigprocmask(SIG_SETMASK, &prev, NULL);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    one("real-time", SIGRTMIN);
    one("standard", SIGUSR1);
    printf("done\n");
    return 0;
}
