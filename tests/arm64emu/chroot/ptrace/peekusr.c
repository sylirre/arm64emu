/* Self-checking ptrace(2) regression test (emulator-only: qemu-user's ptrace
 * emulation is too incomplete to serve as the differential oracle).
 *
 * arm64 has no user area. PTRACE_PEEKUSER and PTRACE_POKEUSER are EIO at any
 * offset -- arch_ptrace has no case for them, and ptrace_request answers EIO
 * to a request it does not know -- and so are the GETREGS, SETREGS, GETFPREGS
 * and SETFPREGS of other architectures: a tracer reads and writes registers
 * with GETREGSET and SETREGSET. The emulator used to answer PEEKUSER from the
 * NT_PRSTATUS image, which no arm64 kernel does, and once with a bound that
 * wrapped for an offset near 2^64 and read its own stack.
 *
 * Verified against a 6.8 kernel (qemu-system, neoverse-n1). */
#include <elf.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>

static int fails;
static void check(int ok, const char *what, unsigned long arg, long got) {
    if (!ok) {
        printf("FAIL: %s (%#lx: got %ld)\n", what, arg, got);
        fails++;
    }
}

int main(void) {
    pid_t pid = fork();
    if (pid == 0) {
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        raise(SIGSTOP);
        for (;;) pause();
    }
    int st;
    if (waitpid(pid, &st, 0) != pid || !WIFSTOPPED(st) || WSTOPSIG(st) != SIGSTOP) {
        printf("FAIL: no stop (status %#x)\n", st);
        return 1;
    }

    struct user_regs_struct r, now;
    struct iovec iov = { &r, sizeof r };
    if (ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &iov) != 0) {
        printf("FAIL: GETREGSET\n");
        return 1;
    }

    /* In the register image and out of it, a partial word, offsets that wrap. */
    const unsigned long off[] = { 0, 8, sizeof r - 8, sizeof r - 4, sizeof r,
                                  ~0UL - 7, ~0UL - 271 };
    for (unsigned i = 0; i < sizeof off / sizeof *off; i++) {
        errno = 0;
        long v = ptrace(PTRACE_PEEKUSER, pid, (void *)off[i], 0);
        check(v == -1 && errno == EIO, "PEEKUSER is EIO", off[i], errno);
        errno = 0;
        v = ptrace(PTRACE_POKEUSER, pid, (void *)off[i], (void *)0x5a5aUL);
        check(v == -1 && errno == EIO, "POKEUSER is EIO", off[i], errno);
    }

    /* PTRACE_GETREGS 12, SETREGS 13, GETFPREGS 14, SETFPREGS 15, given a
     * buffer that would hold any of them. */
    static unsigned char buf[4096];
    for (int req = 12; req <= 15; req++) {
        errno = 0;
        long v = ptrace((enum __ptrace_request)req, pid, 0, buf);
        check(v == -1 && errno == EIO, "the request is EIO", (unsigned long)req, errno);
    }

    iov.iov_base = &now;
    iov.iov_len = sizeof now;
    ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &iov);
    check(!memcmp(&now, &r, sizeof r), "the registers as they were", 0, (long)now.regs[0]);

    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    if (fails) return 1;
    printf("OK\n");
    return 0;
}
