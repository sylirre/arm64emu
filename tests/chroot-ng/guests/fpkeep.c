/* The FP/SIMD registers across a syscall: the kernel keeps all of v0..v31,
 * FPSR and FPCR, whatever the call does -- and so must chroot-ng, whose
 * dispatch spends them, from the SIGSYS handler (the signal frame keeps the
 * guest's) and from guest context, where a blocking call with something to
 * translate is dispatched (sigsys.c, BOUNCE_DISP: a save area of the gate's
 * keeps them).
 *
 * Every row loads all 32 q registers and FPSR with a pattern, makes one call
 * with a raw svc, and compares what comes back:
 *   - a trapped call answered in the handler (openat of a guest path);
 *   - connect(2) of a blocking AF_UNIX socket to a path, which is dispatched
 *     from guest context;
 *   - accept(2) that blocks there until a SIGALRM handler without SA_RESTART
 *     cuts it short: the handler finds the pattern in its frame's FPSIMD
 *     record, and what it writes there is what the program has after the
 *     call, as sigreturn puts it back.
 * And the two registers a handler of chroot-ng's does not run on: a trapped
 * call made with no thread pointer at all and garbage in x18 comes back with
 * both as they were.
 * The expected block is a kernel's. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <ucontext.h>
#include <unistd.h>

static unsigned char want[32][16], got[32][16];
static unsigned long fpsr_set = 0x08000000UL; /* QC */
static unsigned long fpsr_got;

static void fill(unsigned seed) {
    for (int i = 0; i < 32; i++)
        for (int j = 0; j < 16; j++)
            want[i][j] = (unsigned char)(seed + i * 16 + j);
}

/* x0..x5 and x8 for the call; the q registers loaded from `want` just before
 * the svc and stored to `got` just after, FPSR the same. */
static long fpcall(long nr, long a0, long a1, long a2) {
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    register void *w __asm__("x9") = want;
    register void *g __asm__("x10") = got;
    register unsigned long fs __asm__("x11") = fpsr_set;
    __asm__ volatile(
        "msr fpsr, x11\n"
        "ldp q0, q1, [x9, #0]\n ldp q2, q3, [x9, #32]\n"
        "ldp q4, q5, [x9, #64]\n ldp q6, q7, [x9, #96]\n"
        "ldp q8, q9, [x9, #128]\n ldp q10, q11, [x9, #160]\n"
        "ldp q12, q13, [x9, #192]\n ldp q14, q15, [x9, #224]\n"
        "ldp q16, q17, [x9, #256]\n ldp q18, q19, [x9, #288]\n"
        "ldp q20, q21, [x9, #320]\n ldp q22, q23, [x9, #352]\n"
        "ldp q24, q25, [x9, #384]\n ldp q26, q27, [x9, #416]\n"
        "ldp q28, q29, [x9, #448]\n ldp q30, q31, [x9, #480]\n"
        "svc #0\n"
        "stp q0, q1, [x10, #0]\n stp q2, q3, [x10, #32]\n"
        "stp q4, q5, [x10, #64]\n stp q6, q7, [x10, #96]\n"
        "stp q8, q9, [x10, #128]\n stp q10, q11, [x10, #160]\n"
        "stp q12, q13, [x10, #192]\n stp q14, q15, [x10, #224]\n"
        "stp q16, q17, [x10, #256]\n stp q18, q19, [x10, #288]\n"
        "stp q20, q21, [x10, #320]\n stp q22, q23, [x10, #352]\n"
        "stp q24, q25, [x10, #384]\n stp q26, q27, [x10, #416]\n"
        "stp q28, q29, [x10, #448]\n stp q30, q31, [x10, #480]\n"
        "mrs x11, fpsr\n"
        : "+r"(x0), "+r"(fs)
        : "r"(x8), "r"(x1), "r"(x2), "r"(w), "r"(g)
        : "memory", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8",
          "v9", "v10", "v11", "v12", "v13", "v14", "v15", "v16", "v17", "v18",
          "v19", "v20", "v21", "v22", "v23", "v24", "v25", "v26", "v27", "v28",
          "v29", "v30", "v31");
    fpsr_got = fs;
    return x0;
}

static const char *verdict(void) {
    int bad = -1;
    for (int i = 0; i < 32 && bad < 0; i++)
        if (memcmp(want[i], got[i], 16))
            bad = i;
    static char b[64];
    if (bad >= 0)
        snprintf(b, sizeof b, "q%d changed", bad);
    else if ((fpsr_got & 0x08000000UL) != (fpsr_set & 0x08000000UL))
        snprintf(b, sizeof b, "fpsr changed");
    else
        snprintf(b, sizeof b, "kept");
    return b;
}

/* struct fpsimd_context in the frame's __reserved records. */
static unsigned char *frame_fpsimd(ucontext_t *uc) {
    unsigned char *p = (unsigned char *)uc->uc_mcontext.__reserved;
    for (int k = 0; k < 64; k++) {
        unsigned magic = *(unsigned *)p, size = *(unsigned *)(p + 4);
        if (!magic || !size)
            return 0;
        if (magic == 0x46508001)
            return p;
        p += size;
    }
    return 0;
}

static int seen = -1;
static void on_alrm(int sig, siginfo_t *si, void *ucv) {
    (void)sig;
    (void)si;
    unsigned char *f = frame_fpsimd((ucontext_t *)ucv);
    if (!f) {
        seen = 2;
        return;
    }
    /* vregs at offset 16, after head, fpsr and fpcr */
    seen = memcmp(f + 16, want, sizeof want) == 0 ? 1 : 0;
    /* Change q5 in the frame: sigreturn is to put it back so. */
    memset(f + 16 + 5 * 16, 0xee, 16);
}

/* openat with TPIDR_EL0 = 0 and x18 = a pattern, both read back after. */
static long tp0call(const char *path, unsigned long *tp_after,
                    unsigned long *x18_after) {
    register long x8 __asm__("x8") = SYS_openat;
    register long x0 __asm__("x0") = AT_FDCWD;
    register long x1 __asm__("x1") = (long)path;
    register long x2 __asm__("x2") = O_RDONLY;
    unsigned long tp = 0, x18 = 0;
    __asm__ volatile("mrs x12, tpidr_el0\n"
                     "msr tpidr_el0, xzr\n"
                     "movz x18, #0x5a5a\n movk x18, #0xa5a5, lsl #16\n"
                     "movk x18, #0x5a5a, lsl #32\n movk x18, #0xa5a5, lsl #48\n"
                     "svc #0\n"
                     "mov %[x18], x18\n"
                     "mrs %[tp], tpidr_el0\n"
                     "msr tpidr_el0, x12\n"
                     : "+r"(x0), [tp] "=&r"(tp), [x18] "=&r"(x18)
                     : "r"(x8), "r"(x1), "r"(x2)
                     : "memory", "x12", "x18");
    *tp_after = tp;
    *x18_after = x18;
    return x0;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    setvbuf(stdout, 0, _IONBF, 0);

    unsigned long tpa, x18a;
    long t = tp0call("fpkeep.d/f", &tpa, &x18a);
    printf("a call with no thread pointer and x18 garbage: %s, tpidr_el0 %s, "
           "x18 %s\n", t >= 0 ? "answered" : strerror((int)-t),
           tpa == 0 ? "still 0" : "changed",
           x18a == 0xa5a55a5aa5a55a5aUL ? "kept" : "changed");
    if (t >= 0)
        close((int)t);

    fill(1);
    long r = fpcall(SYS_openat, AT_FDCWD, (long)"fpkeep.d/f", O_RDONLY);
    printf("a call answered in the handler: %s (r=%s)\n", verdict(),
           r >= 0 ? "fd" : strerror((int)-r));
    if (r >= 0)
        close((int)r);

    int ls = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    strcpy(a.sun_path, "fpkeep.d/s");
    unlink(a.sun_path);
    if (ls < 0 || bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 4)) {
        printf("no socket: %s\n", strerror(errno));
        return 1;
    }
    int cs = socket(AF_UNIX, SOCK_STREAM, 0);
    fill(2);
    r = fpcall(SYS_connect, cs, (long)&a, sizeof a);
    printf("a call dispatched from guest context: %s (r=%ld)\n", verdict(), r);
    int as = accept(ls, 0, 0);
    close(as);
    close(cs);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_alrm;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGALRM, &sa, 0);
    alarm(1);
    fill(3);
    r = fpcall(SYS_accept, ls, 0, 0);
    printf("a blocked call cut short: r=%s, the handler %s\n",
           r == -EINTR ? "EINTR" : "other",
           seen == 1 ? "found them in its frame"
                     : seen == 0 ? "found other values" : "ran not or found no record");
    memset(want[5], 0xee, 16);
    printf("...and after it: %s\n", verdict());
    unlink(a.sun_path);
    return 0;
}
