/* Self-checking ptrace(2) regression test (emulator-only: qemu-user's ptrace
 * emulation is too incomplete to serve as the differential oracle).
 *
 * ptrace_regset's lengths, for each regset the emulator has. A length that is
 * not a whole number of the regset's units -- 8 bytes for NT_PRSTATUS and
 * NT_ARM_TLS, 4 for NT_PRFPREG and NT_ARM_SYSTEM_CALL -- is EINVAL, for a read
 * as for a write, before anything is copied or iov_len written. Any other is
 * cut to the regset's size, and the cut length is what iov_len says
 * afterwards: a short buffer is never reported as a full transfer, and nothing
 * past the regset is touched, mapped or not. A short SETREGSET writes what it
 * carries and leaves the rest of the regset as it is (gpr_set's newregs,
 * __fpr_set's newstate): 8 bytes of NT_PRSTATUS are x0 alone. NT_ARM_TLS is
 * two words, TPIDR_EL0 and TPIDR2_EL0, the second 0 on a CPU without SME. The
 * regset is the address cut to 32 bits (ptrace_regset's unsigned int type).
 *
 * Verified against a 6.8 kernel (qemu-system, neoverse-n1). */
#include <elf.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAXSZ 528   /* the largest regset here, NT_PRFPREG */

static pid_t pid;
static int fails;

static void check(int ok, const char *set, const char *what, long len, long got) {
    if (!ok) {
        printf("FAIL: %s: %s (len %#lx, got %#lx)\n", set, what, len, got);
        fails++;
    }
}

/* GETREGSET/SETREGSET; returns 0 or -errno, and the iov_len it left. */
static long regset(int req, unsigned long type, void *base, size_t len, size_t *out) {
    struct iovec iov = { base, len };
    errno = 0;
    long rc = ptrace(req, pid, (void *)type, &iov);
    *out = iov.iov_len;
    return rc == -1 ? -errno : rc;
}

static void get(unsigned type, unsigned char *to, size_t size) {
    size_t n;
    memset(to, 0, size);
    if (regset(PTRACE_GETREGSET, type, to, size, &n) != 0 || n != size)
        check(0, "read", "the regset whole", (long)size, (long)n);
}

static void one(unsigned type, const char *name, size_t unit, size_t size,
                unsigned char *edge) {
    unsigned char before[MAXSZ], buf[2 * MAXSZ], now[MAXSZ];
    size_t n;
    long rc;
    get(type, before, size);

    /* Not a whole number of units: EINVAL, nothing copied either way, iov_len
     * left as it was. */
    const size_t bad[] = { 1, unit / 2, unit + unit / 2, size + unit / 2,
                           (1UL << 63) + unit / 2 };
    for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++) {
        memset(buf, 0xa5, sizeof buf);
        rc = regset(PTRACE_GETREGSET, type, buf, bad[i], &n);
        check(rc == -EINVAL, name, "a read of a part unit refused", (long)bad[i], rc);
        check(n == bad[i], name, "its iov_len left alone", (long)bad[i], (long)n);
        check(buf[0] == 0xa5, name, "nothing read", (long)bad[i], buf[0]);

        memcpy(buf, before, size);
        buf[0] ^= 0x5a;
        rc = regset(PTRACE_SETREGSET, type, buf, bad[i], &n);
        check(rc == -EINVAL, name, "a write of a part unit refused", (long)bad[i], rc);
        check(n == bad[i], name, "its iov_len left alone", (long)bad[i], (long)n);
        get(type, now, size);
        check(!memcmp(now, before, size), name, "nothing written", (long)bad[i], now[0]);
    }

    /* Nothing asked: nothing done, and iov_len 0. */
    memset(buf, 0xa5, sizeof buf);
    rc = regset(PTRACE_GETREGSET, type, buf, 0, &n);
    check(rc == 0 && n == 0 && buf[0] == 0xa5, name, "a read of nothing", 0, rc ? rc : (long)n);
    memcpy(buf, before, size);
    buf[0] ^= 0x5a;
    rc = regset(PTRACE_SETREGSET, type, buf, 0, &n);
    check(rc == 0 && n == 0, name, "a write of nothing", 0, rc ? rc : (long)n);
    get(type, now, size);
    check(!memcmp(now, before, size), name, "a write of nothing changes nothing", 0, now[0]);

    /* Longer than the regset: cut to it. */
    const size_t big[] = { 2 * size, 1UL << 63 };
    for (unsigned i = 0; i < 2; i++) {
        memset(buf, 0xa5, sizeof buf);
        rc = regset(PTRACE_GETREGSET, type, buf, big[i], &n);
        check(rc == 0 && n == size, name, "a long read cut to the regset", (long)big[i],
              rc ? rc : (long)n);
        check(!memcmp(buf, before, size) && buf[size] == 0xa5, name,
              "the regset read, nothing past it", (long)big[i], buf[size]);
        memcpy(buf, before, size);
        rc = regset(PTRACE_SETREGSET, type, buf, big[i], &n);
        check(rc == 0 && n == size, name, "a long write cut to the regset", (long)big[i],
              rc ? rc : (long)n);
    }

    /* A regset that ends where the mapping does, with a length that runs far
     * past both: only the regset is copied. */
    unsigned char *at = edge - size;
    rc = regset(PTRACE_GETREGSET, type, at, 4096, &n);
    check(rc == 0 && n == size, name, "a read up to an unmapped page", 4096, rc ? rc : (long)n);
    check(!memcmp(at, before, size), name, "...reads the regset", 4096, at[0]);
    rc = regset(PTRACE_SETREGSET, type, at, 4096, &n);
    check(rc == 0 && n == size, name, "a write up to an unmapped page", 4096, rc ? rc : (long)n);
    get(type, now, size);
    check(!memcmp(now, before, size), name, "...writes the regset as it was", 4096, now[0]);

    /* Short: one unit read is that unit; one unit written is that unit, and
     * the rest stays. */
    memset(buf, 0xa5, sizeof buf);
    rc = regset(PTRACE_GETREGSET, type, buf, unit, &n);
    check(rc == 0 && n == unit, name, "a read of one unit", (long)unit, rc ? rc : (long)n);
    check(!memcmp(buf, before, unit) && buf[unit] == 0xa5, name, "...is the first", (long)unit,
          buf[unit]);
    if (unit < size) {
        memcpy(buf, before, size);
        for (size_t i = 0; i < size; i++) buf[i] ^= 0x5a;
        for (size_t len = unit; len <= 2 * unit && len < size; len += unit) {
            rc = regset(PTRACE_SETREGSET, type, buf, len, &n);
            check(rc == 0 && n == len, name, "a short write", (long)len, rc ? rc : (long)n);
            get(type, now, size);
            check(!memcmp(now, buf, len), name, "...writes what it carries", (long)len, now[0]);
            check(!memcmp(now + len, before + len, size - len), name, "...and nothing else",
                  (long)len, now[len]);
        }
        rc = regset(PTRACE_SETREGSET, type, before, size, &n);
        check(rc == 0 && n == size, name, "the regset put back", (long)size, rc ? rc : (long)n);
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    pid = fork();
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

    unsigned char *map = mmap(NULL, 8192, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (map == MAP_FAILED || munmap(map + 4096, 4096)) {
        printf("FAIL: mmap\n");
        return 1;
    }
    unsigned char *edge = map + 4096;

    one(NT_PRSTATUS, "NT_PRSTATUS", 8, sizeof(struct user_regs_struct), edge);
    one(NT_PRFPREG, "NT_PRFPREG", 4, 528, edge);
    one(NT_ARM_TLS, "NT_ARM_TLS", 8, 16, edge);
    one(NT_ARM_SYSTEM_CALL, "NT_ARM_SYSTEM_CALL", 4, 4, edge);

    /* NT_ARM_TLS: TPIDR_EL0, which fork gave the child as it was here, and a
     * TPIDR2_EL0 this CPU does not have: 0, and not written. */
    uint64_t tls[2] = { 1, 1 }, tp;
    size_t n;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    long rc = regset(PTRACE_GETREGSET, NT_ARM_TLS, tls, sizeof tls, &n);
    check(rc == 0 && tls[0] == tp, "NT_ARM_TLS", "TPIDR_EL0", 16, (long)tls[0]);
    check(tls[1] == 0, "NT_ARM_TLS", "TPIDR2_EL0 without SME", 16, (long)tls[1]);
    tls[1] = 0xdead;
    rc = regset(PTRACE_SETREGSET, NT_ARM_TLS, tls, sizeof tls, &n);
    check(rc == 0 && n == 16, "NT_ARM_TLS", "a write of both words", 16, rc ? rc : (long)n);
    rc = regset(PTRACE_GETREGSET, NT_ARM_TLS, tls, sizeof tls, &n);
    check(rc == 0 && tls[0] == tp && tls[1] == 0, "NT_ARM_TLS", "TPIDR2_EL0 not written", 16,
          (long)tls[1]);

    /* The regset is the address's low 32 bits; one the kernel does not have
     * is EINVAL. */
    struct user_regs_struct a, b;
    memset(&a, 0, sizeof a);
    get(NT_PRSTATUS, (unsigned char *)&b, sizeof b);
    rc = regset(PTRACE_GETREGSET, (1UL << 32) | NT_PRSTATUS, &a, sizeof a, &n);
    check(rc == 0 && n == sizeof a && !memcmp(&a, &b, sizeof a), "NT_PRSTATUS",
          "the type cut to 32 bits", (long)sizeof a, rc ? rc : (long)n);
    rc = regset(PTRACE_GETREGSET, 0x7777, &a, sizeof a, &n);
    check(rc == -EINVAL, "0x7777", "a regset there is not", (long)sizeof a, rc);

    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    if (fails) return 1;
    printf("OK\n");
    return 0;
}
