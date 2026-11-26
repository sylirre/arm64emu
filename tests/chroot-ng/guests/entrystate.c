/* What a program finds at its first instruction, which is the kernel's
 * execve's to set: no thread pointer, no alternate signal stack, every caught
 * signal back at its default (an ignored one stays ignored: SIGUSR2, which
 * nothing between the shell and the program catches), no rseq area
 * registered, and the descriptors its exec'er left it. Freestanding, with an
 * entry of its own: a libc's startup would have set the thread pointer, and
 * glibc's registers an rseq area, before main could look.
 *
 * Then the same again past an exec of itself, made after setting each of
 * them: a thread pointer, an alternate stack, a caught signal and an rseq
 * area, which the exec has to drop.
 *
 * And two answers a program gets later: the kernel keyrings (KEYCTL_GET_KEYRING_ID
 * of the user's keyring, ENOSYS where the facility is not there), and
 * PR_PAC_RESET_KEYS, by the kernel's checks.
 *
 *   entrystate [again]
 *
 * Built -nostdlib -static. */
#include <stddef.h>

typedef unsigned long ul;

#define NR_close 57
#define NR_write 64
#define NR_exit 93
#define NR_sigaltstack 132
#define NR_rt_sigaction 134
#define NR_execve 221
#define NR_fcntl 25
#define NR_prctl 167
#define NR_keyctl 219
#define NR_mmap 222
#define NR_rseq 293

static long sys6(long n, long a, long b, long c, long d, long e, long f) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    register long x4 __asm__("x4") = e;
    register long x5 __asm__("x5") = f;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
                     : "memory");
    return x0;
}
#define S(n, a, b, c) sys6(n, (long)(a), (long)(b), (long)(c), 0, 0, 0)

static ul slen(const char *s) {
    ul n = 0;
    while (s[n])
        n++;
    return n;
}
static void out(const char *s) { S(NR_write, 1, s, slen(s)); }
static void outn(long v) {
    char b[24];
    int i = 23;
    int neg = v < 0;
    ul u = neg ? (ul)-v : (ul)v;
    b[i] = 0;
    do
        b[--i] = (char)('0' + u % 10);
    while (u /= 10);
    if (neg)
        b[--i] = '-';
    out(b + i);
}
static int streq(const char *a, const char *b) {
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

static void handler(int s) { (void)s; }
static void restorer(void) { sys6(139 /* rt_sigreturn */, 0, 0, 0, 0, 0, 0); }

static unsigned char rseq_area[32] __attribute__((aligned(32)));

static void report(const char *pfx, ul tp, char **argv, char **envp) {
    out(pfx);
    out(tp ? "tpidr_el0 set\n" : "tpidr_el0 zero\n");

    ul ss[3];
    S(NR_sigaltstack, 0, ss, 0);
    out(pfx);
    out((ss[1] & 2) ? "no alternate stack\n" : "an alternate stack\n");

    int caught = 0, ign = 0;
    for (int s = 1; s <= 64; s++) {
        ul a[4];
        if (sys6(NR_rt_sigaction, s, 0, (long)a, 8, 0, 0) == 0) {
            if (a[0] > 1)
                caught++;
            if (s == 12 && a[0] == 1)
                ign = 1;
        }
    }
    out(pfx);
    out("caught: ");
    outn(caught);
    out(ign ? ", SIGUSR2 ignored\n" : ", SIGUSR2 not ignored\n");

    long r = sys6(NR_rseq, (long)rseq_area, 32, 0, 0x53053053, 0, 0);
    out(pfx);
    out("rseq: ");
    if (r == 0)
        out("registers\n");
    else {
        outn(-r);
        out("\n");
    }

    out(pfx);
    out("fds:");
    int any = 0;
    for (int fd = 3; fd < 10; fd++)
        if (S(NR_fcntl, fd, 1 /* F_GETFD */, 0) >= 0) {
            out(" ");
            outn(fd);
            any = 1;
        }
    out(any ? "\n" : " none\n");
    (void)argv;
    (void)envp;
}

void cstart(ul *sp, ul tp) {
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    char **envp = argv + argc + 1;
    int again = argc > 1 && streq(argv[1], "again");
    report(again ? "exec: " : "entry: ", tp, argv, envp);
    if (again)
        S(NR_exit, 0, 0, 0);

    /* The rest of what a program meets. */
    long k = S(NR_keyctl, 0 /* KEYCTL_GET_KEYRING_ID */, -4 /* USER */, 0);
    out("keyctl: ");
    out(k == -38 ? "ENOSYS\n" : "answered\n");
    out("pac reset all: ");
    outn(S(NR_prctl, 54, 0, 0));
    out("\npac reset a bad key: ");
    outn(S(NR_prctl, 54, 0x20, 0));
    out("\npac reset an extra argument: ");
    outn(S(NR_prctl, 54, 1, 1));
    out("\n");

    /* Everything an exec is to drop, then the exec. */
    __asm__ volatile("msr tpidr_el0, %0" ::"r"(0x1234UL));
    /* The alternate stack is a mapping of its own: the exec's own signal
     * frame is delivered onto it, and one in the image's .bss would be under
     * the new image, which chroot-ng refuses to map over it. */
    long altstack = sys6(NR_mmap, 0, 16384, 3, 0x22, -1, 0);
    ul ss[3] = {(ul)altstack, 0, 16384};
    S(NR_sigaltstack, ss, 0, 0);
    ul sa[4] = {(ul)handler, 0x04000000UL /* SA_RESTORER */, (ul)restorer, 0};
    sys6(NR_rt_sigaction, 10 /* SIGUSR1 */, (long)sa, 0, 8, 0, 0);
    sys6(NR_rseq, (long)rseq_area, 32, 0, 0x53053053, 0, 0);
    char *av[] = {argv[0], "again", 0};
    long e = S(NR_execve, "/proc/self/exe", av, envp);
    out("exec failed: ");
    outn(-e);
    out("\n");
    S(NR_exit, 1, 0, 0);
}

__asm__(".globl _start\n"
        "_start:\n"
        "    mrs  x1, tpidr_el0\n"
        "    mov  x0, sp\n"
        "    bl   cstart\n"
        "    brk  #0\n");
