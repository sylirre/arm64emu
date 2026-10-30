/* A queued sigval is 8 bytes on arm64, and all 8 reach the receiver: to
 * itself, to one of its threads, to another process, and through a signalfd.
 * An emulator on a 32-bit host re-sends the signal through a host kernel
 * whose sigval is 4 bytes, where only the low half would survive. */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/signalfd.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#define WIDE 0x123456789abcdef0ull

static volatile sig_atomic_t got;
static volatile uint64_t g_val;
static volatile int g_code;

static void h(int sig, siginfo_t *si, void *u) {
    (void)u;
    g_val = (uint64_t)(uintptr_t)si->si_value.sival_ptr;
    g_code = si->si_code;
    got = sig;
}

static void wait_got(void) {
    for (int i = 0; i < 2000 && !got; i++) usleep(1000);
}

static long queue(pid_t tgid, pid_t tid, int sig, uint64_t val) {
    siginfo_t si;
    memset(&si, 0, sizeof si);
    si.si_signo = sig;
    si.si_code = SI_QUEUE;
    si.si_pid = getpid();
    si.si_uid = getuid();
    si.si_value.sival_ptr = (void *)(uintptr_t)val;
    return tid ? syscall(SYS_rt_tgsigqueueinfo, tgid, tid, sig, &si)
               : syscall(SYS_rt_sigqueueinfo, tgid, sig, &si);
}

int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = h;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGRTMIN + 2, &sa, NULL);

    union sigval v;
    v.sival_ptr = (void *)(uintptr_t)WIDE;
    int rc = sigqueue(getpid(), SIGUSR1, v);
    wait_got();
    printf("self rc=%d got=%d code_q=%d val=%#llx\n", rc, (int)got,
           g_code == SI_QUEUE, (unsigned long long)g_val);

    got = 0;
    long r = queue(getpid(), (pid_t)syscall(SYS_gettid), SIGRTMIN + 2, WIDE + 1);
    wait_got();
    printf("thread rc=%ld got=%d val=%#llx\n", r, got == SIGRTMIN + 2,
           (unsigned long long)g_val);

    /* Another process, which waits for it with the signal blocked. */
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGRTMIN + 3);
    sigprocmask(SIG_BLOCK, &set, NULL);
    fflush(stdout);
    pid_t k = fork();
    if (k == 0) {
        siginfo_t si;
        struct timespec ts = { 5, 0 };
        int s = sigtimedwait(&set, &si, &ts);
        printf("child sig=%d code_q=%d val=%#llx\n", s == SIGRTMIN + 3,
               si.si_code == SI_QUEUE,
               (unsigned long long)(uintptr_t)si.si_value.sival_ptr);
        fflush(stdout);
        _exit(0);
    }
    r = queue(k, 0, SIGRTMIN + 3, WIDE + 2);
    int st;
    waitpid(k, &st, 0);
    printf("to child rc=%ld exited=%d\n", r, WIFEXITED(st) && !WEXITSTATUS(st));

    /* A signalfd read: ssi_ptr is the full value. */
    int fd = signalfd(-1, &set, 0);
    r = queue(getpid(), 0, SIGRTMIN + 3, WIDE + 3);
    struct signalfd_siginfo ssi;
    ssize_t n = read(fd, &ssi, sizeof ssi);
    printf("signalfd rc=%ld read=%d code_q=%d ptr=%#llx int=%#x\n", r,
           n == (ssize_t)sizeof ssi, ssi.ssi_code == SI_QUEUE,
           (unsigned long long)ssi.ssi_ptr, (unsigned)ssi.ssi_int);

    /* A value whose high half alone is set. */
    r = queue(getpid(), 0, SIGRTMIN + 3, 0xfedcba9800000000ull);
    n = read(fd, &ssi, sizeof ssi);
    printf("high half only rc=%ld read=%d ptr=%#llx\n", r,
           n == (ssize_t)sizeof ssi, (unsigned long long)ssi.ssi_ptr);
    return 0;
}
