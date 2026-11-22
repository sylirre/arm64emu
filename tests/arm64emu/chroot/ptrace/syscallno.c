/* Self-checking ptrace(2) regression test (emulator-only: qemu-user's ptrace
 * emulation is too incomplete to serve as the differential oracle).
 *
 * NT_ARM_SYSTEM_CALL is the kernel's regs->syscallno, not x8. el0_svc_common
 * takes the number from x8's low 32 bits as the SVC finds them, an int, and
 * from then on that is the call made: a tracer changes it through the regset
 * (-1 skipping the call, x0 then being what the call answers), a change of x8
 * changes x8 only, and x8 is never written back. A stop inside a call -- its
 * entry and exit, and the events it reports (fork, exec, exit) -- shows the
 * call's number; every other is taken by get_signal after do_signal's
 * forget_syscall, or by an exception entered with NO_SYSCALL, and starts at
 * -1, a write there lasting the get_signal: a fatal signal's exit event, the
 * next signal's stop and a group stop see it, user code run since does not.
 * A call of -1 answers -ENOSYS, which a traced one has in x0 at its entry
 * stop already.
 *
 * Verified against a 6.8 kernel (qemu-system, neoverse-n2). */
#include <elf.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef NT_ARM_SYSTEM_CALL
#define NT_ARM_SYSTEM_CALL 0x404
#endif
#define NR_KILL       129
#define NR_GETPID     172
#define NR_GETPPID    173
#define NR_EXIT_GROUP 94
#define NR_CLONE      220
#define NR_EXECVE     221

static int fails;
static void check(int ok, const char *what, long got) {
    if (!ok) {
        printf("FAIL: %s (got %ld)\n", what, got);
        fails++;
    }
}

static int scno(pid_t pid) {
    int n = 0x7777;
    struct iovec iov = { &n, sizeof n };
    if (ptrace(PTRACE_GETREGSET, pid, (void *)NT_ARM_SYSTEM_CALL, &iov) != 0 ||
        iov.iov_len != sizeof n)
        check(0, "GETREGSET NT_ARM_SYSTEM_CALL", errno);
    return n;
}
static void set_scno(pid_t pid, int n) {
    struct iovec iov = { &n, sizeof n };
    if (ptrace(PTRACE_SETREGSET, pid, (void *)NT_ARM_SYSTEM_CALL, &iov) != 0)
        check(0, "SETREGSET NT_ARM_SYSTEM_CALL", errno);
}
static void regs(pid_t pid, struct user_regs_struct *r) {
    struct iovec iov = { r, sizeof *r };
    if (ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &iov) != 0)
        check(0, "GETREGSET NT_PRSTATUS", errno);
}
static void set_regs(pid_t pid, struct user_regs_struct *r) {
    struct iovec iov = { r, sizeof *r };
    if (ptrace(PTRACE_SETREGSET, pid, (void *)NT_PRSTATUS, &iov) != 0)
        check(0, "SETREGSET NT_PRSTATUS", errno);
}
static int stop(pid_t pid) {
    int st;
    if (waitpid(pid, &st, __WALL) != pid) {
        check(0, "waitpid", errno);
        return -1;
    }
    return st;
}
static void outside(pid_t pid, const char *where, unsigned long x8);
/* ...past the SIGCHLD a reaped fork child sends its parent, which a tracee
 * stops for like any signal (ignored by default or not): outside a call. */
static int stop_past_chld(pid_t pid) {
    for (;;) {
        int st = stop(pid);
        if (!WIFSTOPPED(st) || WSTOPSIG(st) != SIGCHLD) return st;
        outside(pid, "SIGCHLD's stop", ~0UL);
        ptrace(PTRACE_CONT, pid, 0, 0);
    }
}

/* One SVC with x8 = nr, x0 = a0, x1 = a1; returns x0 and hands back x8 as
 * the call left them. */
static long svc2(unsigned long nr, long a0, long a1, unsigned long *x8_after) {
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register unsigned long x8 __asm__("x8") = nr;
    __asm__ volatile("svc #0" : "+r"(x0), "+r"(x8) : "r"(x1) : "memory");
    *x8_after = x8;
    return x0;
}
static long svc(unsigned long nr, long a0, unsigned long *x8_after) {
    return svc2(nr, a0, 0, x8_after);
}

/* A kill(self, SIGSTOP), then at once a call of `nr`: nothing in between for
 * PTRACE_SYSCALL from the stop to land on. */
static void stop_then(long self, unsigned long nr, long a0, uint64_t *o0, uint64_t *o8) {
    uint64_t r0, r8;
    __asm__ volatile("mov x0, %[self]\n\tmov x1, #19\n\tmov x8, #129\n\tsvc #0\n\t"
                     "mov x0, %[a0]\n\tmov x8, %[nr]\n\tsvc #0\n\t"
                     "mov %[r0], x0\n\tmov %[r8], x8"
                     : [r0] "=&r"(r0), [r8] "=&r"(r8)
                     : [self] "r"(self), [a0] "r"(a0), [nr] "r"(nr)
                     : "x0", "x1", "x8", "memory");
    *o0 = r0;
    *o8 = r8;
}

#define NSC 6
static const unsigned long sc_nr[NSC] = {
    NR_GETPPID, NR_GETPPID, NR_GETPPID, ~0UL, 0xffffffffUL, (1UL << 32) | NR_GETPID,
};

static void child1(int wfd, const char *self) {
    ptrace(PTRACE_TRACEME, 0, 0, 0);
    raise(SIGSTOP);
    uint64_t out[2 * NSC];
    for (int i = 0; i < NSC; i++)
        stop_then(getpid(), sc_nr[i], 77, &out[2 * i], &out[2 * i + 1]);
    if (write(wfd, out, sizeof out) != (ssize_t)sizeof out) _exit(1);
    /* A breakpoint with x8 set: an exception, not a call. */
    __asm__ volatile("mov x8, #1234\n\tbrk #0" : : : "x8", "memory");
    pid_t g = fork();
    if (g == 0) _exit(0);
    execl(self, self, "exec2", (char *)0);
    _exit(2);
}

/* At a stop outside a call: -1, a write read back, x8 as it was. */
static void outside(pid_t pid, const char *where, unsigned long x8) {
    struct user_regs_struct r;
    char what[96];
    snprintf(what, sizeof what, "%s: NT_ARM_SYSTEM_CALL is -1", where);
    check(scno(pid) == -1, what, scno(pid));
    regs(pid, &r);
    if (x8 != ~0UL) {
        snprintf(what, sizeof what, "%s: x8 as the tracee left it", where);
        check(r.regs[8] == x8, what, (long)r.regs[8]);
    }
    set_scno(pid, 123);
    snprintf(what, sizeof what, "%s: a write read back", where);
    check(scno(pid) == 123, what, scno(pid));
    struct user_regs_struct now;
    regs(pid, &now);
    snprintf(what, sizeof what, "%s: a write leaves x8 alone", where);
    check(now.regs[8] == r.regs[8], what, (long)now.regs[8]);
}

static void trace1(pid_t pid, int rfd) {
    int st = stop(pid);
    check(WIFSTOPPED(st) && WSTOPSIG(st) == SIGSTOP, "first stop", st);
    ptrace(PTRACE_SETOPTIONS, pid, 0, (void *)(long)(PTRACE_O_TRACESYSGOOD |
           PTRACE_O_TRACEFORK | PTRACE_O_TRACEEXEC | PTRACE_O_TRACEEXIT));
    outside(pid, "raise's SIGSTOP", ~0UL);
    ptrace(PTRACE_CONT, pid, 0, 0);

    for (int i = 0; i < NSC; i++) {
        char what[96];
        struct user_regs_struct r;
        st = stop(pid);
        check(WIFSTOPPED(st) && WSTOPSIG(st) == SIGSTOP, "kill's SIGSTOP", st);
        outside(pid, "kill's SIGSTOP", NR_KILL);
        ptrace(PTRACE_SYSCALL, pid, 0, 0);
        st = stop(pid);
        check(WIFSTOPPED(st) && WSTOPSIG(st) == (SIGTRAP | 0x80), "entry stop", st);
        regs(pid, &r);
        snprintf(what, sizeof what, "call %d: x8 at the entry stop", i);
        check(r.regs[8] == sc_nr[i], what, (long)r.regs[8]);
        int want = (int)(unsigned)sc_nr[i];
        snprintf(what, sizeof what, "call %d: the number at the entry stop", i);
        check(scno(pid) == want, what, scno(pid));
        if (want == -1) {
            snprintf(what, sizeof what, "call %d: -1 has -ENOSYS in x0 at the stop", i);
            check((long)r.regs[0] == -ENOSYS, what, (long)r.regs[0]);
        }
        switch (i) {
        case 0: set_scno(pid, NR_GETPID); break;            /* the call is getpid */
        case 1: r.regs[8] = NR_GETPID; set_regs(pid, &r); break;   /* x8 only */
        case 2: set_scno(pid, -1); r.regs[0] = 4242; set_regs(pid, &r); break;
        }
        ptrace(PTRACE_SYSCALL, pid, 0, 0);
        st = stop(pid);
        check(WIFSTOPPED(st) && WSTOPSIG(st) == (SIGTRAP | 0x80), "exit stop", st);
        regs(pid, &r);
        int at_exit = i == 0 ? NR_GETPID : i == 2 ? -1 : want;
        snprintf(what, sizeof what, "call %d: the number at the exit stop", i);
        check(scno(pid) == at_exit, what, scno(pid));
        ptrace(PTRACE_CONT, pid, 0, 0);
    }

    /* What each call answered, and the x8 it left. */
    uint64_t out[2 * NSC];
    if (read(rfd, out, sizeof out) != (ssize_t)sizeof out) {
        check(0, "the tracee's report", errno);
        return;
    }
    const struct { long x0; unsigned long x8; } want[NSC] = {
        { pid, NR_GETPPID },          /* getpid ran; x8 untouched */
        { getpid(), NR_GETPID },      /* getppid ran; x8 as the tracer set it */
        { 4242, NR_GETPPID },         /* skipped, with the tracer's x0 */
        { -ENOSYS, ~0UL },
        { -ENOSYS, 0xffffffffUL },
        { pid, (1UL << 32) | NR_GETPID },   /* the low 32 bits are the call */
    };
    for (int i = 0; i < NSC; i++) {
        char what[64];
        snprintf(what, sizeof what, "call %d answered", i);
        check((long)out[2 * i] == want[i].x0, what, (long)out[2 * i]);
        snprintf(what, sizeof what, "call %d left x8", i);
        check(out[2 * i + 1] == want[i].x8, what, (long)out[2 * i + 1]);
    }

    /* The breakpoint: an exception's stop. */
    st = stop(pid);
    check(WIFSTOPPED(st) && WSTOPSIG(st) == SIGTRAP, "brk stop", st);
    outside(pid, "brk", 1234);
    struct user_regs_struct r;
    regs(pid, &r);
    r.pc += 4;
    set_regs(pid, &r);
    ptrace(PTRACE_CONT, pid, 0, 0);

    /* fork: the event is the clone call's; the child's first stop is not. */
    st = stop(pid);
    check(WIFSTOPPED(st) && (st >> 8) == (SIGTRAP | PTRACE_EVENT_FORK << 8), "fork event", st);
    check(scno(pid) == NR_CLONE, "fork event: the clone call", scno(pid));
    unsigned long g = 0;
    ptrace(PTRACE_GETEVENTMSG, pid, 0, &g);
    st = stop((pid_t)g);
    check(WIFSTOPPED(st) && WSTOPSIG(st) == SIGSTOP, "fork child's stop", st);
    outside((pid_t)g, "fork child's SIGSTOP", ~0UL);
    ptrace(PTRACE_CONT, (pid_t)g, 0, 0);
    st = stop((pid_t)g);
    check(WIFSTOPPED(st) && (st >> 8) == (SIGTRAP | PTRACE_EVENT_EXIT << 8), "child's exit event", st);
    check(scno((pid_t)g) == NR_EXIT_GROUP, "_exit's event: exit_group", scno((pid_t)g));
    ptrace(PTRACE_CONT, (pid_t)g, 0, 0);
    st = stop((pid_t)g);
    check(WIFEXITED(st) && WEXITSTATUS(st) == 0, "fork child exited", st);
    ptrace(PTRACE_CONT, pid, 0, 0);

    /* exec: the event is the execve call's (start_thread keeps syscallno). */
    st = stop_past_chld(pid);
    check(WIFSTOPPED(st) && (st >> 8) == (SIGTRAP | PTRACE_EVENT_EXEC << 8), "exec event", st);
    check(scno(pid) == NR_EXECVE, "exec event: the execve call", scno(pid));
    ptrace(PTRACE_CONT, pid, 0, 0);
    st = stop(pid);
    check(WIFSTOPPED(st) && (st >> 8) == (SIGTRAP | PTRACE_EVENT_EXIT << 8), "exit event", st);
    check(scno(pid) == NR_EXIT_GROUP, "exit event: the exit_group call", scno(pid));
    ptrace(PTRACE_CONT, pid, 0, 0);
    st = stop(pid);
    check(WIFEXITED(st) && WEXITSTATUS(st) == 7, "the exec'd image exited 7", st);
}

/* A death by signal: the signal's stop and the exit event both outside any
 * call, though the signal came from one. */
static void case_death(void) {
    pid_t pid = fork();
    if (pid == 0) {
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        raise(SIGSTOP);
        unsigned long x8;
        long self = getpid();
        svc2(NR_KILL, self, SIGTERM, &x8);
        _exit(3);
    }
    int st = stop(pid);
    ptrace(PTRACE_SETOPTIONS, pid, 0, (void *)(long)PTRACE_O_TRACEEXIT);
    ptrace(PTRACE_CONT, pid, 0, 0);
    st = stop(pid);
    check(WIFSTOPPED(st) && WSTOPSIG(st) == SIGTERM, "SIGTERM's stop", st);
    outside(pid, "SIGTERM's stop", NR_KILL);
    ptrace(PTRACE_CONT, pid, 0, (void *)SIGTERM);
    st = stop(pid);
    check(WIFSTOPPED(st) && (st >> 8) == (SIGTRAP | PTRACE_EVENT_EXIT << 8), "SIGTERM's exit event", st);
    /* One get_signal: what the tracer wrote at the signal's stop. */
    check(scno(pid) == 123, "a signal death's exit event: the signal stop's write", scno(pid));
    struct user_regs_struct r;
    regs(pid, &r);
    check(r.regs[8] == NR_KILL, "a signal death's exit event: x8", (long)r.regs[8]);
    ptrace(PTRACE_CONT, pid, 0, 0);
    st = stop(pid);
    check(WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM, "killed by SIGTERM", st);
}

/* Two signals taken on one way back to user space, the first suppressed,
 * the second resumed as a SIGSTOP: three stops of one get_signal, where a
 * write at the first is what the other two see. */
static void case_one_get_signal(void) {
    pid_t pid = fork();
    if (pid == 0) {
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        sigset_t ss;
        sigemptyset(&ss);
        sigaddset(&ss, SIGUSR1);
        sigaddset(&ss, SIGUSR2);
        sigprocmask(SIG_BLOCK, &ss, 0);
        raise(SIGSTOP);
        raise(SIGUSR1);
        raise(SIGUSR2);
        sigprocmask(SIG_UNBLOCK, &ss, 0);
        _exit(0);
    }
    int st = stop(pid);
    check(WIFSTOPPED(st) && WSTOPSIG(st) == SIGSTOP, "first stop", st);
    ptrace(PTRACE_CONT, pid, 0, 0);
    st = stop(pid);
    check(WIFSTOPPED(st) && WSTOPSIG(st) == SIGUSR1, "SIGUSR1's stop", st);
    check(scno(pid) == -1, "SIGUSR1's stop: -1", scno(pid));
    set_scno(pid, 55);
    ptrace(PTRACE_CONT, pid, 0, 0);
    st = stop(pid);
    check(WIFSTOPPED(st) && WSTOPSIG(st) == SIGUSR2, "SIGUSR2's stop", st);
    check(scno(pid) == 55, "the next signal's stop: the write at the first", scno(pid));
    ptrace(PTRACE_CONT, pid, 0, (void *)SIGSTOP);
    st = stop(pid);
    check(WIFSTOPPED(st) && WSTOPSIG(st) == SIGSTOP, "the group stop", st);
    check(scno(pid) == 55, "the group stop: the write at the first", scno(pid));
    ptrace(PTRACE_CONT, pid, 0, 0);
    st = stop(pid);
    check(WIFEXITED(st) && WEXITSTATUS(st) == 0, "exited", st);
}

/* PTRACE_INTERRUPT of a SEIZEd tracee blocked in read: the trap is
 * get_signal's, the read given up for its restart. */
static void case_interrupt(void) {
    int in[2], ready[2];
    if (pipe(in) || pipe(ready)) { check(0, "pipe", errno); return; }
    pid_t pid = fork();
    if (pid == 0) {
        char c = 'r';
        if (write(ready[1], &c, 1) != 1) _exit(1);
        _exit(read(in[0], &c, 1) == 1 && c == 'x' ? 0 : 4);
    }
    char c;
    if (read(ready[0], &c, 1) != 1) { check(0, "ready", errno); return; }
    usleep(100000);
    if (ptrace(PTRACE_SEIZE, pid, 0, 0) || ptrace(PTRACE_INTERRUPT, pid, 0, 0)) {
        check(0, "SEIZE/INTERRUPT", errno);
        kill(pid, SIGKILL);
        return;
    }
    int st = stop(pid);
    check(WIFSTOPPED(st) && (st >> 16) == PTRACE_EVENT_STOP, "the INTERRUPT's trap", st);
    outside(pid, "an INTERRUPT in read", ~0UL);
    if (write(in[1], "x", 1) != 1) check(0, "write", errno);
    ptrace(PTRACE_CONT, pid, 0, 0);
    st = stop(pid);
    check(WIFEXITED(st) && WEXITSTATUS(st) == 0, "the read went on", st);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 1 && !strcmp(argv[1], "exec2")) {
        unsigned long x8;
        svc(NR_EXIT_GROUP, 7, &x8);
        return 9;
    }

    /* Untraced, with no filter: -1 is an unknown number, -ENOSYS; the bits
     * of x8 above 32 are no part of the number. */
    unsigned long x8;
    check(svc(~0UL, 77, &x8) == -ENOSYS, "syscall -1", 0);
    check(svc(0xffffffffUL, 77, &x8) == -ENOSYS, "syscall 0xffffffff", 0);
    long p = svc((1UL << 32) | NR_GETPID, 77, &x8);
    check(p == getpid() && x8 == ((1UL << 32) | NR_GETPID), "x8 = 1<<32 | getpid", p);

    int rep[2];
    if (pipe(rep)) { printf("FAIL: pipe\n"); return 1; }
    pid_t pid = fork();
    if (pid == 0) {
        close(rep[0]);
        child1(rep[1], argv[0]);
    }
    close(rep[1]);
    trace1(pid, rep[0]);
    case_death();
    case_one_get_signal();
    case_interrupt();
    if (fails) return 1;
    printf("OK\n");
    return 0;
}
