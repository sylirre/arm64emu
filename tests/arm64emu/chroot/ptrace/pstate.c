/* Self-checking ptrace(2) regression test (emulator-only: qemu-user's ptrace
 * emulation is too incomplete to serve as the differential oracle).
 *
 * The pstate of NT_PRSTATUS is the kernel's regs->pstate: what EL0 runs
 * with -- EL0t, AArch64, D, A, I and F clear -- and its NZCV, nothing of the
 * CPU's own mode. A SETREGSET's is judged as gpr_set judges it
 * (valid_user_regs): its RES0 bits dropped, and one that is not EL0t
 * AArch64 with DAIF clear refused with EINVAL, the whole write with it. Bits
 * a CPU does not implement (DIT, TCO, BTYPE here) are kept as written until
 * the tracee runs again -- a GETREGSET in the same stop, and the signal frame
 * a resumption with a signal builds, show them -- and are gone once it has
 * run, as the hardware never had them; NZCV is the tracee's from then on.
 *
 * Verified against a 6.8 kernel (qemu-system, neoverse-n1: no DIT, MTE or
 * BTI either). That CPU has SSBS, which this one does not, and runs a task
 * with it set: bit 12 is left out of every comparison. */
#include <errno.h>
#include <elf.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>

#define NZCV_ZC  0x60000000ULL
#define NZCV_C   0x20000000ULL
#define DIT      (1ULL << 24)
#define TCO      (1ULL << 25)
#define BTYPE_J  (1ULL << 10)
#define SSBS     (1ULL << 12)
#define PS(v)    ((unsigned long long)((v) & ~SSBS))

/* ps_run(tid, nzcv): NZCV set, then tkill(tid, SIGSTOP) -- the first stop --
 * then NZCV read into x10 and the second SIGSTOP; returns that NZCV. */
__asm__(".text\n.balign 4\n"
        ".global ps_run\n.type ps_run, %function\nps_run:\n"
        "    mov x11, x0\n    msr nzcv, x1\n"
        "    mov x1, #19\n    mov x8, #130\n    svc #0\n"
        "    mrs x10, nzcv\n"
        "    mov x0, x11\n    mov x1, #19\n    mov x8, #130\n    svc #0\n"
        "    mov x0, x10\n    ret\n"
        /* ps_sig(tid): NZCV to ZC, then tkill(tid, SIGUSR1). */
        ".global ps_sig\n.type ps_sig, %function\nps_sig:\n"
        "    mov x9, #0x60000000\n    msr nzcv, x9\n"
        "    mov x1, #10\n    mov x8, #130\n    svc #0\n    ret\n");
uint64_t ps_run(long tid, uint64_t nzcv);
void ps_sig(long tid);

static volatile uint64_t frame_pstate;
static void on_usr1(int s, siginfo_t *si, void *ucv) {
    (void)s; (void)si;
    frame_pstate = ((ucontext_t *)ucv)->uc_mcontext.pstate;
}

static int getregs(pid_t pid, struct user_regs_struct *r) {
    struct iovec iov = { r, sizeof *r };
    return (int)ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &iov);
}
static int setregs(pid_t pid, struct user_regs_struct *r) {
    struct iovec iov = { r, sizeof *r };
    return (int)ptrace(PTRACE_SETREGSET, pid, (void *)NT_PRSTATUS, &iov);
}

static int fails;
static void check(int ok, const char *what, unsigned long long got) {
    if (!ok) {
        printf("FAIL: %s (got %#llx)\n", what, got);
        fails++;
    }
}

static int stop(pid_t pid, int sig) {
    int st;
    if (waitpid(pid, &st, 0) != pid || !WIFSTOPPED(st) || WSTOPSIG(st) != sig) {
        printf("FAIL: no stop for signal %d (status %#x)\n", sig, st);
        return -1;
    }
    return 0;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    pid_t pid = fork();
    if (pid == 0) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = on_usr1;
        sa.sa_flags = SA_SIGINFO;
        sigaction(SIGUSR1, &sa, NULL);
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        long tid = getpid();
        ps_run(tid, NZCV_ZC);
        ps_sig(tid);
        ps_run(tid, 0);
        _exit(0);
    }

    struct user_regs_struct r, w;
    if (stop(pid, SIGSTOP)) return 1;

    /* What the tracee runs with. */
    getregs(pid, &r);
    check(PS(r.pstate) == NZCV_ZC, "pstate at the stop", PS(r.pstate));

    /* Not EL0t AArch64 with DAIF clear: EINVAL, and nothing of the write. */
    static const uint64_t bad[] = { 0x5, 0x4, 0x1, 0x10, 0x3c0, 0x200, 0x80 };
    for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++) {
        w = r;
        w.regs[12] = 0xdead;
        w.pstate = NZCV_C | bad[i];
        errno = 0;
        int rc = setregs(pid, &w);
        check(rc == -1 && errno == EINVAL, "an invalid pstate refused", bad[i]);
        struct user_regs_struct now;
        getregs(pid, &now);
        check(now.regs[12] == r.regs[12] && now.pstate == r.pstate,
              "a refused write changes nothing", PS(now.pstate));
    }

    /* RES0 bits: dropped, the rest taken. */
    w = r;
    w.pstate = NZCV_C | 1ULL << 20 | 1ULL << 22 | 1ULL << 23 | 1ULL << 5 | 1ULL << 32 |
               1ULL << 63 | 1ULL << 26;
    check(setregs(pid, &w) == 0, "RES0 bits accepted", w.pstate);
    getregs(pid, &w);
    check(PS(w.pstate) == NZCV_C, "RES0 bits dropped", PS(w.pstate));

    /* Bits the CPU lacks: kept until the tracee runs. */
    w.pstate = NZCV_C | DIT | TCO | BTYPE_J;
    check(setregs(pid, &w) == 0, "DIT/TCO/BTYPE accepted", w.pstate);
    getregs(pid, &w);
    check(PS(w.pstate) == (NZCV_C | DIT | TCO | BTYPE_J), "kept in the same stop", PS(w.pstate));

    ptrace(PTRACE_CONT, pid, 0, 0);
    if (stop(pid, SIGSTOP)) return 1;
    getregs(pid, &r);
    check(r.regs[10] == NZCV_C, "the tracee's NZCV is the written one", r.regs[10]);
    check(PS(r.pstate) == NZCV_C, "the lacking bits gone once it ran", PS(r.pstate));

    /* A signal frame built from a written pstate shows it. */
    ptrace(PTRACE_CONT, pid, 0, 0);
    if (stop(pid, SIGUSR1)) return 1;
    getregs(pid, &r);
    check(PS(r.pstate) == NZCV_ZC, "pstate at the signal stop", PS(r.pstate));
    r.pstate |= DIT;
    check(setregs(pid, &r) == 0, "DIT accepted at the signal stop", r.pstate);
    ptrace(PTRACE_CONT, pid, 0, (void *)(long)SIGUSR1);
    if (stop(pid, SIGSTOP)) return 1;
    errno = 0;
    long fp = ptrace(PTRACE_PEEKDATA, pid, (void *)&frame_pstate, 0);
    check(errno == 0 && PS((uint64_t)fp) == (NZCV_ZC | DIT), "the frame's pstate", PS((uint64_t)fp));
    getregs(pid, &r);
    check(PS(r.pstate) == 0, "pstate after the handler and its sigreturn", PS(r.pstate));

    ptrace(PTRACE_CONT, pid, 0, 0);
    if (stop(pid, SIGSTOP)) return 1;
    ptrace(PTRACE_CONT, pid, 0, 0);
    int st;
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        printf("FAIL: tracee status %#x\n", st);
        return 1;
    }
    if (fails) return 1;
    printf("OK\n");
    return 0;
}
