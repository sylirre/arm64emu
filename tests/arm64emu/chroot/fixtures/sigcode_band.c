/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* A guest's queued si_code from the band the emulator's own marks and
 * carriers are drawn from (src/arm64emu/chroot/signal.c, SIG_CODE_BAND): the
 * kernel lets a process queue itself any negative code and hands it back as
 * it was. Sent on to the host as written, the emulator read one for its own:
 * arm64emu run in a guest (the AArch64 build under chroot mode, which is how
 * the native tier's fallback runs in its suite on an x86 host) probes its
 * host with a kick whose code is in the job-control carriers' range, and the
 * emulator underneath took it for a SIGSTOP and stopped.
 *
 * Self-checking: each signal is queued blocked and read back at once with a
 * zero timeout, then queued again to a handler, where a kernel gives back
 * what was queued, byte for byte (checked natively). The numbers are raw: the
 * guest libc's SIGRTMAX is the kernel's 64, the emulator's kick. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static void one(int sig, int code, int ign) {
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = ign ? SIG_IGN : SIG_DFL;
    sigaction(sig, &sa, &old);
    unsigned long mask = 1UL << (sig - 1), prev = 0;
    syscall(SYS_rt_sigprocmask, SIG_BLOCK, &mask, &prev, 8);
    siginfo_t si, got;
    memset(&si, 0, sizeof si);
    si.si_signo = sig;
    si.si_errno = 0x5a5a;
    si.si_code = code;
    si.si_pid = getpid();
    si.si_uid = getuid();
    si.si_value.sival_int = 0x1234567;
    long q = syscall(SYS_rt_sigqueueinfo, (pid_t)getpid(), sig, &si);
    struct timespec zero = {0, 0};
    memset(&got, 0, sizeof got);
    long r = q ? -1 : syscall(SYS_rt_sigtimedwait, &mask, &got, &zero, (size_t)8);
    printf("sig=%d queue=%ld wait=%ld code=%d same=%d errno=%d val=%x\n", sig, q, r,
           got.si_code, got.si_code == code, got.si_errno, got.si_value.sival_int);
    fflush(stdout);
    syscall(SYS_rt_sigprocmask, SIG_SETMASK, &prev, NULL, 8);
    sigaction(sig, &old, NULL);
}

/* The same codes delivered to a handler, the path on which the emulator
 * reads its marks and carriers off a host signal. */
static volatile sig_atomic_t g_sig, g_code;
static void h(int sig, siginfo_t *si, void *u) {
    (void)u;
    g_sig = sig;
    g_code = si->si_code;
}

static void caught(int sig, int code) {
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = h;
    sa.sa_flags = SA_SIGINFO;
    sigaction(sig, &sa, &old);
    unsigned long mask = 1UL << (sig - 1), prev = 0;
    syscall(SYS_rt_sigprocmask, SIG_UNBLOCK, &mask, &prev, 8);
    siginfo_t si;
    memset(&si, 0, sizeof si);
    si.si_signo = sig;
    si.si_code = code;
    si.si_pid = getpid();
    si.si_uid = getuid();
    g_sig = 0;
    g_code = 0;
    long q = syscall(SYS_rt_sigqueueinfo, (pid_t)getpid(), sig, &si);
    printf("caught sig=%d queue=%ld got=%d same=%d\n", sig, q, (int)g_sig,
           g_code == code);
    fflush(stdout);
    syscall(SYS_rt_sigprocmask, SIG_SETMASK, &prev, NULL, 8);
    sigaction(sig, &old, NULL);
}

int main(void) {
    one(64, -(0x7ffff000 + 0x123), 1);  /* a job-control carrier's, ignored */
    one(64, -(0x7ffff000 + 0x123), 0);  /* ...and at its default */
    one(SIGUSR1, -(0x7fffff00 + 5), 0); /* the thread mark's */
    one(SIGUSR1, -(0x7ffffe00 + 5), 0); /* the origin mark's */
    one(SIGUSR2, INT_MIN, 0);           /* the bottom of the band */
    caught(SIGUSR1, -(0x7fffff00 + 5));
    caught(SIGUSR1, -(0x7ffffe00 + 5));
    caught(SIGUSR2, INT_MIN);
    caught(64, -(0x7ffff000 + 0x123));  /* where the emulator stopped */
    printf("done\n");
    return 0;
}
