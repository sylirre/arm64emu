/* What a fault leaves in the signal frame, as a Linux kernel keeps it: the
 * thread's fault_address (sigcontext.fault_address) and fault_code, the ESR
 * of the fault, in an esr_context record (magic 0x45535201) after the
 * fpsimd_context, present only while fault_code is not 0.
 *
 * Neither is the siginfo's, and neither is cleared once delivered: they are
 * thread.fault_address / thread.fault_code, which only some exceptions set
 * (arch/arm64/mm/fault.c set_thread_esr: a data or instruction abort, an
 * alignment fault -- the address untagged, the ESR as the CPU gave it, or for
 * an address that is no user one a level-0 translation fault keeping only
 * EC/IL/CM/WnR; arch/arm64/kernel/traps.c arm64_notify_die: SP/PC alignment,
 * the address 0 and the ESR as it is, an undefined instruction, both 0) and
 * every frame reads. A BRK, a kill, a seccomp trap leave them as the last
 * fault set them; a new thread or a fork child starts with its creator's
 * (arch_dup_task_struct copies the whole task), and an execve keeps them.
 *
 * Self-checking: qemu-user keeps none of this. Each line is one signal: what
 * the siginfo says, then the frame's fault_address and ESR record. The cases
 * pick what every kernel and CPU report alike: a hole and a read-only page
 * inside a region whose other pages are touched (so the last-level table is
 * there and the fault is a level-3 one), a read-only page read first (a
 * permission fault, not a translation fault), an atomic crossing a 16-byte
 * boundary (an alignment fault even with FEAT_LSE2). */
#define _GNU_SOURCE
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>

__asm__(".arch armv8.1-a\n"          /* LDADD */
        ".text\n.balign 4\n"
        ".global fc_udf\n.type fc_udf, %function\nfc_udf:\n    udf #0\n    ret\n"
        ".global fc_brk\n.type fc_brk, %function\nfc_brk:\n    brk #0\n    ret\n"
        ".global fc_ld\n.type fc_ld, %function\nfc_ld:\n    ldr x1, [x0]\n    ret\n"
        ".global fc_st\n.type fc_st, %function\nfc_st:\n    str x1, [x0]\n    ret\n"
        ".global fc_zva\n.type fc_zva, %function\nfc_zva:\n    dc zva, x0\n    ret\n"
        ".global fc_ldadd\n.type fc_ldadd, %function\n"
        "fc_ldadd:\n    mov x1, #1\n    ldadd x1, x2, [x0]\n    ret\n"
        ".global fc_br\n.type fc_br, %function\nfc_br:\n    br x0\n");
void fc_udf(void), fc_brk(void), fc_ld(uint64_t), fc_st(uint64_t),
     fc_zva(uint64_t), fc_ldadd(uint64_t), fc_br(uint64_t);

#define ESR_MAGIC_ 0x45535201u

static long pg;
static uint64_t hole, ro, un, eof, mis;
static __thread sigjmp_buf jb;
static __thread char res[200];

static const char *sym(uint64_t a, char *b, size_t n) {
    if (a == (hole | 0x5aULL << 56)) return "hole, tag 0x5a";
    if (a == 0) return "0";
    if (a == hole) return "hole";
    if (a == hole + 8) return "hole+8";
    if (a == ro) return "ro";
    if (a == un) return "un";
    if (a == eof) return "eof";
    if (a == mis) return "mis";
    if (a == (uint64_t)(uintptr_t)fc_udf) return "udf";
    if (a == (uint64_t)(uintptr_t)fc_brk) return "brk";
    snprintf(b, n, "%#llx", (unsigned long long)a);
    return b;
}

static void on_sig(int sig, siginfo_t *si, void *uc_) {
    ucontext_t *uc = uc_;
    char b1[24], b2[24], e[48] = "none";
    /* The records of __reserved, walked as the kernel lays them: {magic,
     * size} each, a zero magic ends them. */
    unsigned char *r = (unsigned char *)uc->uc_mcontext.__reserved;
    for (unsigned off = 0, i = 0; off + 8 <= 4096; i++) {
        uint32_t magic, size;
        memcpy(&magic, r + off, 4);
        memcpy(&size, r + off + 4, 4);
        if (!magic || size < 8) break;
        if (magic == ESR_MAGIC_) {
            uint64_t esr;
            memcpy(&esr, r + off + 8, 8);
            snprintf(e, sizeof e, "%#llx (record %u%s)", (unsigned long long)esr,
                     i, size == 16 ? "" : ", BAD size");
        }
        off += size;
    }
    const char *name = sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" :
                       sig == SIGILL ? "SIGILL" : sig == SIGTRAP ? "SIGTRAP" :
                       sig == SIGUSR1 ? "SIGUSR1" : "?";
    if (sig == SIGUSR1)
        snprintf(res, sizeof res, "%s | fault_address %s | esr %s", name,
                 sym(uc->uc_mcontext.fault_address, b2, sizeof b2), e);
    else
        snprintf(res, sizeof res, "%s code %d addr %s | fault_address %s | esr %s",
                 name, si->si_code, sym((uint64_t)(uintptr_t)si->si_addr, b1, sizeof b1),
                 sym(uc->uc_mcontext.fault_address, b2, sizeof b2), e);
    if (sig != SIGUSR1) siglongjmp(jb, 1);
}

static void show(const char *what) { printf("%-8s %s\n", what, res); fflush(stdout); }

#define TRY(what, call) do {                                            \
        strcpy(res, "no signal");                                       \
        if (!sigsetjmp(jb, 1)) { call; }                                \
        show(what);                                                     \
    } while (0)
#define USR1(what) do { strcpy(res, "no signal"); raise(SIGUSR1); show(what); } while (0)

#define SA_EXPOSE_TAGBITS_ 0x00000800

static void handler(int sig, int flags) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_sig;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER | flags;
    sigaction(sig, &sa, 0);
}

static void handlers(void) {
    int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGUSR1 };
    for (unsigned i = 0; i < sizeof sigs / sizeof sigs[0]; i++) handler(sigs[i], 0);
}

static void *thread_fn(void *arg) {
    (void)arg;
    USR1("thr-new");                        /* its creator's */
    TRY("thr-wr", fc_st(hole));
    USR1("thr-usr1");
    return 0;
}

int main(int argc, char **argv) {
    handlers();
    if (argc > 2) {                         /* the image an execve started */
        eof = strtoull(argv[2], 0, 16);     /* the old image's last fault */
        USR1("exec");
        TRY("undef", fc_udf());
        USR1("usr1");
        return 0;
    }
    pg = sysconf(_SC_PAGESIZE);
    /* Four pages, all touched: [0] hole+8 lives at hole, [1] the hole,
     * [2] read-only, [3] the atomic's. */
    unsigned char *m = mmap(0, 4 * pg, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) return 1;
    memset(m, 1, 4 * pg);
    munmap(m + pg, pg);
    mprotect(m + 2 * pg, pg, PROT_READ);
    hole = (uint64_t)(uintptr_t)(m + pg);
    ro = (uint64_t)(uintptr_t)(m + 2 * pg);
    un = (uint64_t)(uintptr_t)(m + 3 * pg + 12);
    mis = (uint64_t)(uintptr_t)fc_ld + 2;
    /* A file one page long, mapped two: the second is past end-of-file. */
    FILE *f = tmpfile();
    if (!f || ftruncate(fileno(f), pg)) return 1;
    unsigned char *fm = mmap(0, 2 * pg, PROT_READ, MAP_SHARED, fileno(f), 0);
    if (fm == MAP_FAILED) return 1;
    eof = (uint64_t)(uintptr_t)(fm + pg);
    volatile unsigned char sink = fm[0] + ((volatile unsigned char *)ro)[0];
    (void)sink;

    TRY("undef", fc_udf());                 /* both 0, whatever came before */
    USR1("usr1");
    TRY("rdhole", fc_ld(hole));
    USR1("usr1");                           /* the fault's, still */
    TRY("brk", fc_brk());                   /* sets neither */
    TRY("wrhole", fc_st(hole));
    TRY("wrro", fc_st(ro));
    TRY("zvaro", fc_zva(ro));               /* a write, CM clear */
    TRY("tagged", fc_ld(hole | 0x5aULL << 56));   /* si_addr untagged too... */
    handler(SIGSEGV, SA_EXPOSE_TAGBITS_);
    TRY("exposed", fc_ld(hole | 0x5aULL << 56));  /* ...unless asked for */
    handler(SIGSEGV, 0);
    TRY("khalf", fc_ld(0xffff000000001000ULL));
    TRY("toohigh", fc_ld(0x0070000000001000ULL));
    TRY("xro", ((void (*)(void))(uintptr_t)ro)());
    TRY("xhole", ((void (*)(void))(uintptr_t)hole)());
    TRY("ldadd", fc_ldadd(un));             /* WnR clear: the read faults too */
    TRY("pcalign", fc_br(mis));             /* SP/PC alignment: address 0 */
    USR1("usr1");
    TRY("eof", fc_ld(eof));

    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) { USR1("child"); _exit(0); }
    int st;
    waitpid(pid, &st, 0);

    pthread_t t;
    pthread_create(&t, 0, thread_fn, 0);
    pthread_join(t, 0);
    USR1("usr1");                           /* the thread's fault was its own */

    char a[24];
    snprintf(a, sizeof a, "%llx", (unsigned long long)eof);
    fflush(stdout);
    execl(argv[0], argv[0], "exec", a, (char *)0);
    return 1;
}
