/* A SIGSYS a task queues itself, dressed as a seccomp trap (si_code
 * SYS_SECCOMP, which the kernel lets a task send itself), is an ordinary
 * signal: it goes to the task's own disposition. A seccomp monitor's tests
 * raise one that way. The emulator took it for its host's filter trapping one
 * of its own calls, answered "ENOSYS" into whatever its context held and
 * never ran the handler. */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

static volatile int runs, code, arch_ok, nr_seen;

static void h(int s, siginfo_t *si, void *uc) {
    (void)s; (void)uc;
    runs++;
    code = si->si_code;
    nr_seen = si->si_syscall;
    arch_ok = si->si_arch == 0xC00000B7u;
}

int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = h;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSYS, &sa, NULL);
    siginfo_t si;
    memset(&si, 0, sizeof si);
    si.si_signo = SIGSYS;
    si.si_code = 1;                 /* SYS_SECCOMP */
    si.si_syscall = 1234;
    si.si_arch = 0xC00000B7u;       /* AUDIT_ARCH_AARCH64 */
    si.si_call_addr = (void *)0x1000;
    long r = syscall(SYS_rt_tgsigqueueinfo, getpid(), gettid(), SIGSYS, &si);
    printf("queued=%ld runs=%d code=%d nr=%d arch_ok=%d\n", r, runs, code, nr_seen, arch_ok);
    return 0;
}
