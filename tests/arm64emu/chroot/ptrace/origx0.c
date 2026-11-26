/* Self-checking ptrace(2)/seccomp regression test (emulator-only: qemu-user
 * implements neither a guest's seccomp filter nor enough of ptrace to serve
 * as the differential oracle).
 *
 * arm64's regs->orig_x0 is x0 as the SVC found it -- and, since e057b9477232
 * ("arm64: syscall: Ensure saved x0 is kept in-sync with tracer updates", in
 * every stable kernel), x0 as a tracer leaves it at the call's entry stop or
 * its RET_TRACE stop, for a call that is made (not -1). The kernel goes on
 * using it:
 *   - seccomp judges it as args[0] (syscall_get_arguments): the tracer's x0
 *     after the entry stop, and after a RET_TRACE stop at the recheck; for a
 *     call of -1 the x0 it was made with, its x0 being -ENOSYS by then;
 *   - RET_TRAP puts it back (syscall_rollback): the exit stop, the SIGSYS
 *     frame and the call's answer all hold it -- -ENOSYS is never left;
 *   - PTRACE_GET_SYSCALL_INFO gives it as the first argument;
 *   - a restart starts from it: do_signal rewinds pc to the SVC and puts it
 *     back in x0 before get_signal, so the stop that follows an interrupted
 *     call shows the call about to be made again as the tracer left it, and
 *     the restart is that call (a handler without SA_RESTART turning it into
 *     EINTR after the stop).
 * The interrupted call's exit stop shows its restart code (-ERESTARTSYS for
 * a read), and rt_sigreturn's exit stop the -1 a frame taken back leaves.
 *
 * Verified against a 6.8 kernel with that fix (Ubuntu 6.8.0-151, qemu-system,
 * neoverse-n2). */
#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>

#ifndef NT_ARM_SYSTEM_CALL
#define NT_ARM_SYSTEM_CALL 0x404
#endif
#ifndef PTRACE_GET_SYSCALL_INFO
#define PTRACE_GET_SYSCALL_INFO 0x420e
#endif
#define NR_READ         63
#define NR_KILL         129
#define NR_RT_SIGRETURN 139
#define NR_GETPPID      173
#define ERESTARTSYS     512

struct sci {
    uint8_t op, pad[3];
    uint32_t arch;
    uint64_t ip, sp;
    union {
        struct { uint64_t nr, args[6]; } entry;
        struct { int64_t rval; uint8_t is_error; } exit;
        struct { uint64_t nr, args[6]; uint32_t ret_data; } seccomp;
    };
};

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
    if (ptrace(PTRACE_GETREGSET, pid, (void *)NT_ARM_SYSTEM_CALL, &iov) != 0)
        check(0, "GETREGSET NT_ARM_SYSTEM_CALL", errno);
    return n;
}
static void regs(pid_t pid, struct user_regs_struct *r) {
    struct iovec iov = { r, sizeof *r };
    if (ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &iov) != 0)
        check(0, "GETREGSET NT_PRSTATUS", errno);
}
static void set_x0(pid_t pid, uint64_t v) {
    struct user_regs_struct r;
    regs(pid, &r);
    r.regs[0] = v;
    struct iovec iov = { &r, sizeof r };
    if (ptrace(PTRACE_SETREGSET, pid, (void *)NT_PRSTATUS, &iov) != 0)
        check(0, "SETREGSET NT_PRSTATUS", errno);
}
static long info(pid_t pid, struct sci *si) {
    memset(si, 0xa5, sizeof *si);
    return ptrace(PTRACE_GET_SYSCALL_INFO, pid, (void *)sizeof *si, si);
}
static unsigned long msg(pid_t pid) {
    unsigned long m = 0x7777;
    ptrace(PTRACE_GETEVENTMSG, pid, 0, &m);
    return m;
}
static int stop(pid_t pid) {
    int st;
    if (waitpid(pid, &st, __WALL) != pid) {
        check(0, "waitpid", errno);
        return -1;
    }
    return st;
}
static int is_sc(int st) { return WIFSTOPPED(st) && WSTOPSIG(st) == (SIGTRAP | 0x80); }

/* kill(self, SIGSTOP), then at once one SVC of `nr` with x0..x2: nothing in
 * between for a PTRACE_SYSCALL from the stop to land on. `site` is the
 * second SVC's address. */
extern char ox_site[];
__attribute__((noinline)) static long stop_then(long self, unsigned long nr,
                                                long a0, long a1, long a2) {
    register long x0 __asm__("x0") = self;
    register long x1 __asm__("x1") = SIGSTOP;
    register long x2 __asm__("x2") = a2;
    register unsigned long x8 __asm__("x8") = NR_KILL;
    __asm__ volatile("svc #0\n\t"
                     "mov x0, %[a0]\n\tmov x1, %[a1]\n\tmov x8, %[nr]\n\t"
                     ".globl ox_site\nox_site:\n\tsvc #0"
                     : "+r"(x0), "+r"(x1), "+r"(x8)
                     : [a0] "r"(a0), [a1] "r"(a1), [nr] "r"(nr), "r"(x2)
                     : "memory");
    return x0;
}
static long svc1(unsigned long nr, long a0) {
    register long x0 __asm__("x0") = a0;
    register unsigned long x8 __asm__("x8") = nr;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
    return x0;
}

static volatile uint64_t h_x0, h_pc;
static volatile int h_sig;
static void on_sig(int sig, siginfo_t *si, void *uv) {
    (void)si;
    ucontext_t *uc = uv;
    h_sig = sig;
    h_x0 = uc->uc_mcontext.regs[0];
    h_pc = uc->uc_mcontext.pc;
}
static void handle(int sig, int flags) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_sig;
    sa.sa_flags = SA_SIGINFO | flags;
    sigaction(sig, &sa, 0);
}

/* getppid by its first argument: 0x1234 ERRNO 77, 0x5555 TRACE 5, 0x7777
 * TRAP 9; a call of -1 with 0x1234, ERRNO 78; anything else allowed. */
#define ARG0 (offsetof(struct seccomp_data, args[0]))
static int install_filter(void) {
    struct sock_filter f[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, NR_GETPPID, 3, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0xffffffffu, 0, 1),
        BPF_JUMP(BPF_JMP | BPF_JA, 9, 0, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, ARG0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0x1234, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | 77),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0x5555, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRACE | 5),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0x7777, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRAP | 9),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, ARG0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0x1234, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | 78),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog p = { sizeof f / sizeof f[0], f };
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) return -1;
    return prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &p);
}

/* ---- seccomp, untraced ---- */
static void case_untraced(void) {
    int p[2];
    if (pipe(p)) { check(0, "pipe", errno); return; }
    pid_t pid = fork();
    if (pid == 0) {
        handle(SIGSYS, 0);
        if (install_filter() != 0) _exit(9);
        long out[4];
        h_sig = 0;
        out[0] = svc1(NR_GETPPID, 0x7777);
        out[1] = h_sig == SIGSYS ? (long)h_x0 : -1;
        out[2] = svc1(~0UL, 0x1234);
        out[3] = svc1(~0UL, 0x4444);
        _exit(write(p[1], out, sizeof out) == (ssize_t)sizeof out ? 0 : 1);
    }
    close(p[1]);
    long out[4] = { 0 };
    ssize_t k = read(p[0], out, sizeof out);
    close(p[0]);
    int st;
    waitpid(pid, &st, 0);
    check(k == (ssize_t)sizeof out && WIFEXITED(st) && WEXITSTATUS(st) == 0, "untraced child", st);
    check(out[0] == 0x7777, "RET_TRAP: the call answers x0 as it was made", out[0]);
    check(out[1] == 0x7777, "RET_TRAP: the SIGSYS frame's x0", out[1]);
    check(out[2] == -78, "a call of -1: the filter's args[0] is x0 as it was made", out[2]);
    check(out[3] == -ENOSYS, "a call of -1 the filter allows", out[3]);
}

/* ---- seccomp, traced: a tracer's x0 is the filter's, but for a call of -1 ---- */
#define NSC 6
static const unsigned long sc_nr[NSC] = { NR_GETPPID, NR_GETPPID, NR_GETPPID,
                                          NR_GETPPID, ~0UL, NR_GETPPID };
static const long sc_a0[NSC] = { 0x1234, 0, 0x5555, 0x7777, 0x1234, 0x9999 };
static const long sc_set[NSC] = { 0, 0x1234, -1, -1, -1, 0x4321 };

static void case_traced(void) {
    int p[2];
    if (pipe(p)) { check(0, "pipe", errno); return; }
    pid_t pid = fork();
    if (pid == 0) {
        close(p[0]);
        handle(SIGSYS, 0);
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        raise(SIGSTOP);
        if (install_filter() != 0) _exit(9);
        long out[2 * NSC];
        for (int i = 0; i < NSC; i++) {
            h_sig = 0;
            out[2 * i] = stop_then(getpid(), sc_nr[i], sc_a0[i], 0, 0);
            out[2 * i + 1] = h_sig == SIGSYS ? (long)h_x0 : -1;
        }
        _exit(write(p[1], out, sizeof out) == (ssize_t)sizeof out ? 0 : 1);
    }
    close(p[1]);
    long me = getpid();
    int st = stop(pid);
    check(WIFSTOPPED(st) && WSTOPSIG(st) == SIGSTOP, "first stop", st);
    ptrace(PTRACE_SETOPTIONS, pid, 0, (void *)(long)(PTRACE_O_TRACESECCOMP | PTRACE_O_TRACESYSGOOD));
    ptrace(PTRACE_CONT, pid, 0, 0);
    for (int i = 0; i < NSC; i++) {
        char what[96];
        struct user_regs_struct r;
        struct sci si;
        st = stop(pid);
        snprintf(what, sizeof what, "call %d: kill's SIGSTOP", i);
        check(WIFSTOPPED(st) && WSTOPSIG(st) == SIGSTOP, what, st);
        if (!WIFSTOPPED(st)) break;
        long n = info(pid, &si);
        snprintf(what, sizeof what, "call %d: GET_SYSCALL_INFO at a signal stop is op NONE", i);
        check(n == 24 && si.op == 0 && si.arch == 0xc00000b7u, what, n);
        ptrace(PTRACE_SYSCALL, pid, 0, 0);

        st = stop(pid);
        snprintf(what, sizeof what, "call %d: entry stop", i);
        check(is_sc(st), what, st);
        regs(pid, &r);
        int want_nr = (int)(unsigned)sc_nr[i];
        uint64_t want_x0 = want_nr == -1 ? (uint64_t)-ENOSYS : (uint64_t)sc_a0[i];
        snprintf(what, sizeof what, "call %d: x0 at the entry stop", i);
        check(r.regs[0] == want_x0, what, (long)r.regs[0]);
        snprintf(what, sizeof what, "call %d: the entry stop's eventmsg", i);
        check(msg(pid) == 1, what, (long)msg(pid));
        if (sc_set[i] != -1) set_x0(pid, (uint64_t)sc_set[i]);
        n = info(pid, &si);
        snprintf(what, sizeof what, "call %d: GET_SYSCALL_INFO entry", i);
        check(n == 80 && si.op == 1 && si.arch == 0xc00000b7u &&
              (int64_t)si.entry.nr == want_nr && si.ip == r.pc && si.sp == r.sp, what, n);
        snprintf(what, sizeof what, "call %d: GET_SYSCALL_INFO entry: args[0] is the tracer's x0", i);
        check(si.entry.args[0] == (uint64_t)(sc_set[i] != -1 ? sc_set[i] : sc_a0[i]), what,
              (long)si.entry.args[0]);
        ptrace(PTRACE_SYSCALL, pid, 0, 0);

        st = stop(pid);
        if (i == 2) {
            check(WIFSTOPPED(st) && (st >> 8) == (SIGTRAP | (PTRACE_EVENT_SECCOMP << 8)),
                  "call 2: the RET_TRACE stop", st);
            check(msg(pid) == 5, "call 2: RET_TRACE's data", (long)msg(pid));
            set_x0(pid, 0x1234);
            n = info(pid, &si);
            check(n == 84 && si.op == 3 && (int64_t)si.seccomp.nr == NR_GETPPID &&
                  si.seccomp.ret_data == 5, "call 2: GET_SYSCALL_INFO seccomp", n);
            check(si.seccomp.args[0] == 0x1234, "call 2: GET_SYSCALL_INFO seccomp: args[0] is the tracer's x0",
                  (long)si.seccomp.args[0]);
            ptrace(PTRACE_SYSCALL, pid, 0, 0);
            st = stop(pid);
        }
        snprintf(what, sizeof what, "call %d: exit stop", i);
        check(is_sc(st), what, st);
        regs(pid, &r);
        const long want_ret[NSC] = { me, -77, -77, 0x7777, -78, me };
        snprintf(what, sizeof what, "call %d: x0 at the exit stop", i);
        check((long)r.regs[0] == want_ret[i], what, (long)r.regs[0]);
        snprintf(what, sizeof what, "call %d: the exit stop's eventmsg", i);
        check(msg(pid) == 2, what, (long)msg(pid));
        n = info(pid, &si);
        snprintf(what, sizeof what, "call %d: GET_SYSCALL_INFO exit", i);
        check(n == 33 && si.op == 2 && si.exit.rval == want_ret[i] &&
              si.exit.is_error == (want_ret[i] < 0), what, n);
        ptrace(PTRACE_CONT, pid, 0, 0);
        if (i == 3) {
            st = stop(pid);
            check(WIFSTOPPED(st) && WSTOPSIG(st) == SIGSYS, "call 3: the SIGSYS stop", st);
            regs(pid, &r);
            check(r.regs[0] == 0x7777, "call 3: x0 at the SIGSYS stop", (long)r.regs[0]);
            check(scno(pid) == -1, "call 3: the SIGSYS stop is outside the call", scno(pid));
            ptrace(PTRACE_CONT, pid, 0, (void *)SIGSYS);
        }
    }
    long out[2 * NSC] = { 0 };
    ssize_t k = read(p[0], out, sizeof out);
    close(p[0]);
    st = stop(pid);
    check(WIFEXITED(st) && WEXITSTATUS(st) == 0 && k == (ssize_t)sizeof out, "traced child", st);
    const long want[NSC] = { me, -77, -77, 0x7777, -78, me };
    for (int i = 0; i < NSC; i++) {
        char what[64];
        snprintf(what, sizeof what, "call %d answered", i);
        check(out[2 * i] == want[i], what, out[2 * i]);
    }
    check(out[7] == 0x7777, "call 3: the SIGSYS frame's x0", out[7]);
}

/* ---- restart: a read whose descriptor the tracer changed at its entry
 * stop, interrupted by a signal: with an SA_RESTART handler, a handler
 * without it, and none. ---- */
static void case_restart(const char *name, int sig, int flags) {
    int a[2], b[2], rep[2];
    if (pipe(a) || pipe(b) || pipe(rep)) { check(0, "pipe", errno); return; }
    if (write(a[1], "A", 1) != 1) { check(0, "write", errno); return; }
    pid_t pid = fork();
    if (pid == 0) {
        if (flags >= 0) handle(sig, flags);
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        raise(SIGSTOP);
        char ch = 0;
        h_sig = 0;
        long r = stop_then(getpid(), NR_READ, a[0], (long)&ch, 1);
        long out[5] = { r, ch, h_sig, (long)h_x0, (long)h_pc };
        _exit(write(rep[1], out, sizeof out) == (ssize_t)sizeof out ? 0 : 1);
    }
    uint64_t site = (uint64_t)ox_site;
    char what[128];
    struct user_regs_struct r;
    struct sci si;
#define EXPECT(cond, txt, v) do { snprintf(what, sizeof what, "%s: %s", name, txt); \
                                  check(cond, what, (long)(v)); } while (0)
    int st = stop(pid);
    ptrace(PTRACE_SETOPTIONS, pid, 0, (void *)(long)PTRACE_O_TRACESYSGOOD);
    ptrace(PTRACE_CONT, pid, 0, 0);
    st = stop(pid);
    EXPECT(WIFSTOPPED(st) && WSTOPSIG(st) == SIGSTOP, "kill's SIGSTOP", st);
    ptrace(PTRACE_SYSCALL, pid, 0, 0);

    st = stop(pid);
    regs(pid, &r);
    EXPECT(is_sc(st) && r.pc == site + 4 && scno(pid) == NR_READ, "the read's entry stop", st);
    EXPECT(r.regs[0] == (uint64_t)a[0], "x0 at the entry stop", r.regs[0]);
    set_x0(pid, (uint64_t)b[0]);   /* an empty pipe: the read blocks */
    ptrace(PTRACE_SYSCALL, pid, 0, 0);
    usleep(200000);
    kill(pid, sig);

    st = stop(pid);
    regs(pid, &r);
    EXPECT(is_sc(st) && scno(pid) == NR_READ, "the read's exit stop", st);
    EXPECT((long)r.regs[0] == -ERESTARTSYS, "the exit stop shows ERESTARTSYS", r.regs[0]);
    EXPECT(r.pc == site + 4, "the exit stop is past the SVC", r.pc - site);
    long n = info(pid, &si);
    EXPECT(n == 33 && si.op == 2 && si.exit.rval == -ERESTARTSYS && si.exit.is_error == 1,
           "GET_SYSCALL_INFO exit", n);
    ptrace(PTRACE_SYSCALL, pid, 0, 0);

    st = stop(pid);
    regs(pid, &r);
    EXPECT(WIFSTOPPED(st) && WSTOPSIG(st) == sig, "the signal's stop", st);
    EXPECT(scno(pid) == -1, "the signal's stop is outside the call", scno(pid));
    EXPECT(r.pc == site, "the signal's stop is at the SVC, rewound", r.pc - site);
    EXPECT(r.regs[0] == (uint64_t)b[0], "the signal's stop has x0 as the tracer left it", r.regs[0]);
    ptrace(PTRACE_SYSCALL, pid, 0, (void *)(long)sig);

    int restarts = flags < 0 || (flags & SA_RESTART);
    if (flags >= 0) {
        st = stop(pid);
        EXPECT(is_sc(st) && scno(pid) == NR_RT_SIGRETURN, "rt_sigreturn's entry stop", st);
        ptrace(PTRACE_SYSCALL, pid, 0, 0);
        st = stop(pid);
        regs(pid, &r);
        EXPECT(is_sc(st), "rt_sigreturn's exit stop", st);
        EXPECT(scno(pid) == -1, "rt_sigreturn's exit stop: no call", scno(pid));
        if (restarts)
            EXPECT(r.pc == site && r.regs[0] == (uint64_t)b[0],
                   "rt_sigreturn restores the restart", r.pc - site);
        else
            EXPECT(r.pc == site + 4 && (long)r.regs[0] == -EINTR,
                   "rt_sigreturn restores the EINTR", r.regs[0]);
        ptrace(restarts ? PTRACE_SYSCALL : PTRACE_CONT, pid, 0, 0);
    }
    if (restarts) {
        st = stop(pid);
        regs(pid, &r);
        EXPECT(is_sc(st) && scno(pid) == NR_READ && r.pc == site + 4, "the restart's entry stop", st);
        EXPECT(r.regs[0] == (uint64_t)b[0], "the restart is the read the tracer left", r.regs[0]);
        if (r.regs[0] == (uint64_t)b[0] && write(b[1], "B", 1) != 1) check(0, "write", errno);
        ptrace(PTRACE_SYSCALL, pid, 0, 0);
        st = stop(pid);
        regs(pid, &r);
        EXPECT(is_sc(st) && r.regs[0] == 1, "the restart's exit stop", r.regs[0]);
        ptrace(PTRACE_CONT, pid, 0, 0);
    }
    st = stop(pid);
    EXPECT(WIFEXITED(st) && WEXITSTATUS(st) == 0, "exited", st);
    long out[5] = { 0 };
    if (read(rep[0], out, sizeof out) != (ssize_t)sizeof out) check(0, "the tracee's report", errno);
    if (!restarts) {
        EXPECT(out[0] == -EINTR, "the read answered EINTR", out[0]);
        EXPECT(out[3] == -EINTR && out[4] == (long)site + 4,
               "the handler's frame: EINTR, past the SVC", out[3]);
    } else {
        EXPECT(out[0] == 1 && out[1] == 'B', "the read restarted, as the tracer left it", out[0]);
        if (flags >= 0)
            EXPECT(out[2] == sig && out[3] == b[0] && out[4] == (long)site,
                   "the handler's frame: the restart, from the tracer's x0", out[3]);
    }
#undef EXPECT
    close(a[0]); close(a[1]); close(b[0]); close(b[1]); close(rep[0]); close(rep[1]);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    case_untraced();
    case_traced();
    case_restart("SA_RESTART handler", SIGUSR1, SA_RESTART);
    case_restart("handler", SIGUSR2, 0);
    case_restart("no handler", SIGWINCH, -1);
    if (fails) return 1;
    printf("OK\n");
    return 0;
}
