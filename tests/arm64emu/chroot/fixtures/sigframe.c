/* The signal frame as a Linux kernel builds it and takes it back
 * (arch/arm64/kernel/signal.c).
 *
 * Delivery (setup_rt_frame): the frame is sizeof(struct rt_sigframe) -- 4688
 * bytes, the 128-byte siginfo and a ucontext whose sigcontext keeps 4096
 * bytes of records -- under a frame record {x29, x30} of the interrupted
 * code, itself 16 bytes under the stack pointer rounded down to 16, and the
 * handler is entered with sp at the frame, x29 at the record and x30 at the
 * sa_restorer when SA_RESTORER asks for it, the vDSO's sigreturn otherwise.
 * x1 and x2 point at the siginfo and the ucontext for an SA_SIGINFO handler
 * only; any other keeps the interrupted ones. A frame that cannot be written
 * is a SIGSEGV of the kernel's own (force_sigsegv: SI_KERNEL) for the
 * handler that may have one, the signal it was for gone (an SA_RESETHAND
 * handler of it reset all the same, the flags kept), and fatal when the
 * signal was a SIGSEGV already. AT_MINSIGSTKSZ is the largest frame plus the
 * record and the alignment, and sigaltstack refuses less than MINSIGSTKSZ
 * (5120).
 *
 * rt_sigreturn (restore_sigframe): the stack pointer aligned and the frame
 * in user space, or nothing is restored; then the mask, every register read
 * on its own (one that faults reads 0), a PSTATE that must be EL0t AArch64
 * with DAIF clear (its RES0 bits dropped), and the records walked
 * (parse_user_sigframe: 16-aligned, in bounds, an fpsimd_context of exactly
 * 528 bytes required and only one, an esr_context ignored, an extra_context
 * followed, anything else refused) -- and a frame that fails any of it is
 * arm64_notify_segfault(sp): SIGSEGV at the stack pointer as far as it was
 * restored, SEGV_MAPERR when no mapping lies at or above it, SEGV_ACCERR
 * otherwise, x0 0 (the call's return) and the fault record cleared, for the
 * handler that may have one -- not death.
 *
 * Self-checking: each line is one delivery, one rt_sigreturn or one child's
 * end. The expected block is what a 6.8 kernel gave this program (qemu-system,
 * neoverse-n1: no SVE, so the frame is the base one). */
#define _GNU_SOURCE
/* siglongjmp leaves stacks of the test's own making: no __longjmp_chk. */
#undef _FORTIFY_SOURCE
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>

#ifndef AT_MINSIGSTKSZ
#define AT_MINSIGSTKSZ 51
#endif
#define K_SA_RESTORER    0x04000000UL
#define K_SA_UNSUPPORTED 0x00000400UL

__asm__(".text\n.balign 4\n"
        /* A handler's entry: x0..x2, x29, x30 and sp as delivery left them. */
        ".global sf_entry\n.type sf_entry, %function\nsf_entry:\n"
        "    adrp x9, sf_ent\n    add x9, x9, :lo12:sf_ent\n"
        "    stp x0, x1, [x9]\n    stp x2, x29, [x9, #16]\n"
        "    mov x10, sp\n    stp x30, x10, [x9, #32]\n"
        "    b sf_handler\n"
        /* tkill: on its return x0 is 0, x1 the signal, x2 the marker. */
        ".global sf_kill\n.type sf_kill, %function\nsf_kill:\n"
        "    mov x8, #130\n    svc #0\n    ret\n"
        ".global sf_kill_mis\n.type sf_kill_mis, %function\nsf_kill_mis:\n"
        "    sub sp, sp, #8\n    mov x8, #130\n    svc #0\n    add sp, sp, #8\n    ret\n"
        /* ...with the stack pointer at x3, where no frame can be written. */
        ".global sf_kill_sp\n.type sf_kill_sp, %function\nsf_kill_sp:\n"
        "    mov x9, sp\n    mov sp, x3\n    mov x8, #130\n    svc #0\n"
        ".global sf_kill_sp_after\nsf_kill_sp_after:\n"
        "    mov sp, x9\n    ret\n"
        /* rt_sigreturn of the frame at x0, with x1, x2, x28 and NZCV known. */
        ".global sf_sigreturn\n.type sf_sigreturn, %function\nsf_sigreturn:\n"
        "    mov sp, x0\n    mov x1, #0x1111\n    mov x2, #0x2222\n    mov x28, #0x2828\n"
        "    mov x9, #0x40000000\n    msr nzcv, x9\n"
        "    mov x8, #139\n    svc #0\n"
        ".global sf_after\nsf_after:\n    brk #1\n"
        /* Where a frame that is taken back resumes. */
        ".global sf_landed\n.type sf_landed, %function\nsf_landed:\n"
        "    adrp x9, sf_land\n    add x9, x9, :lo12:sf_land\n"
        "    stp x0, x1, [x9]\n    mov x10, sp\n    stp x28, x10, [x9, #16]\n"
        "    mrs x10, nzcv\n    str x10, [x9, #32]\n"
        "    b sf_land_c\n"
        /* An SA_RESTORER restorer: counts its runs, then the sigreturn. */
        ".global sf_restorer\n.type sf_restorer, %function\nsf_restorer:\n"
        "    adrp x9, sf_rcount\n    ldr x10, [x9, :lo12:sf_rcount]\n"
        "    add x10, x10, #1\n    str x10, [x9, :lo12:sf_rcount]\n"
        "    mov x8, #139\n    svc #0\n"
        /* A load through a stack pointer no frame can go under. */
        ".global sf_ld_sp\n.type sf_ld_sp, %function\nsf_ld_sp:\n"
        "    mov sp, x0\n    ldur x1, [sp, #-16]\n    brk #2\n");
void sf_entry(void), sf_landed(void), sf_restorer(void);
void sf_kill(long tid, long sig, uint64_t marker);
void sf_kill_mis(long tid, long sig, uint64_t marker);
void sf_kill_sp(long tid, long sig, uint64_t marker, uint64_t sp);
void sf_sigreturn(uint64_t sp);
void sf_ld_sp(uint64_t sp);
extern char sf_after[], sf_kill_sp_after[];

uint64_t sf_ent[6];      /* x0 x1 x2 x29 x30 sp at a handler's entry */
uint64_t sf_land[5];     /* x0 x1 x28 sp nzcv where a frame resumed */
uint64_t sf_rcount;

/* ---- delivery -------------------------------------------------------- */

static struct {
    uint64_t base, isp, rec0, rec1, r29, r30, x1, x2;
    int tramp;
} H;

void sf_handler(int sig, siginfo_t *si, void *ucv) {
    (void)sig; (void)si; (void)ucv;   /* x1 and x2 are not ours to trust */
    uint64_t base = sf_ent[5];
    ucontext_t *uc = (ucontext_t *)(base + 128);
    const uint64_t *rec = (const uint64_t *)sf_ent[3];
    const uint32_t *w = (const uint32_t *)sf_ent[4];
    H.base = base;
    H.isp = uc->uc_mcontext.sp;
    H.r29 = uc->uc_mcontext.regs[29];
    H.r30 = uc->uc_mcontext.regs[30];
    H.rec0 = rec[0];
    H.rec1 = rec[1];
    H.x1 = sf_ent[1];
    H.x2 = sf_ent[2];
    H.tramp = w && w[0] == 0xd2801168u && w[1] == 0xd4000001u;
}

static const char *argname(uint64_t v, uint64_t ptr, const char *what, char *b) {
    if (v == ptr) return what;
    if (v == SIGUSR1 || v == SIGUSR2) return "signo";
    if (v == 0xabcd) return "marker";
    snprintf(b, 24, "%#llx", (unsigned long long)v);
    return b;
}

static void geometry(const char *name, unsigned long flags, int how, uint64_t top) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = (void (*)(int))sf_entry;
    sa.sa_flags = (int)flags;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
    memset(&H, 0, sizeof H);
    long tid = syscall(SYS_gettid);
    if (how) sf_kill_mis(tid, SIGUSR1, 0xabcd);
    else sf_kill(tid, SIGUSR1, 0xabcd);
    char b1[24], b2[24];
    /* The frame's distance below the stack it went on: the interrupted
     * one's pointer, or the alternate stack's top. */
    printf("%-11s %s %lld record %lld fp %s lr %s ret %s x1 %s x2 %s\n", name,
           top ? "below-top" : "below-sp", (long long)((top ? top : H.isp) - H.base),
           (long long)(sf_ent[3] - H.base),
           H.rec0 == H.r29 ? "ok" : "bad", H.rec1 == H.r30 ? "ok" : "bad",
           H.tramp ? "sigreturn" : "other",
           argname(H.x1, H.base, "siginfo", b1), argname(H.x2, H.base + 128, "ucontext", b2));
}

/* ---- sigaction ------------------------------------------------------- */

struct ksa { uint64_t handler, flags, restorer, mask; };

static long ksigaction(int sig, const struct ksa *act, struct ksa *old) {
    return syscall(SYS_rt_sigaction, sig, act, old, 8);
}

/* ---- rt_sigreturn ---------------------------------------------------- */

#define FR       4688
#define UC       128
#define MASKOFF  (UC + 40)
#define MCTX     (UC + 176)
#define REGS     (MCTX + 8)
#define SPOFF    (MCTX + 256)
#define PCOFF    (MCTX + 264)
#define PSOFF    (MCTX + 272)
#define RSV      (MCTX + 288)
#define FPSIMD_MAGIC_ 0x46508001u
#define ESR_MAGIC_    0x45535201u
#define EXTRA_MAGIC_  0x45585401u
#define SVE_MAGIC_    0x53564501u

static sigjmp_buf jb;
static unsigned char *buf;          /* 64 KB: crafted frames at +32K */
static uint64_t F, LSP, HALF, HOLE, NONE_TOP;
static struct {
    int code;
    uint64_t addr, pc, sp, x0, x1, x2, x28, pstate, fa, mask;
    char esr[24];
} S;

static void esr_of(ucontext_t *uc, char *e, size_t n) {
    unsigned char *r = (unsigned char *)uc->uc_mcontext.__reserved;
    snprintf(e, n, "none");
    for (unsigned off = 0; off + 8 <= 4096; ) {
        uint32_t magic, size;
        memcpy(&magic, r + off, 4);
        memcpy(&size, r + off + 4, 4);
        if (!magic || size < 8) break;
        if (magic == ESR_MAGIC_) {
            uint64_t esr;
            memcpy(&esr, r + off + 8, 8);
            snprintf(e, n, "%#llx", (unsigned long long)esr);
        }
        off += size;
    }
}

static void on_segv(int sig, siginfo_t *si, void *ucv) {
    (void)sig;
    ucontext_t *uc = ucv;
    S.code = si->si_code;
    S.addr = (uint64_t)(uintptr_t)si->si_addr;
    S.pc = uc->uc_mcontext.pc;
    S.sp = uc->uc_mcontext.sp;
    S.x0 = uc->uc_mcontext.regs[0];
    S.x1 = uc->uc_mcontext.regs[1];
    S.x2 = uc->uc_mcontext.regs[2];
    S.x28 = uc->uc_mcontext.regs[28];
    S.pstate = uc->uc_mcontext.pstate;
    S.fa = uc->uc_mcontext.fault_address;
    memcpy(&S.mask, &uc->uc_sigmask, 8);
    esr_of(uc, S.esr, sizeof S.esr);
    siglongjmp(jb, 1);
}

void sf_land_c(void) { siglongjmp(jb, 2); }

static const char *sym(uint64_t a, char *b) {
    if (a == 0) return "0";
    if (a == F) return "frame";
    if (a == F + 8) return "frame+8";
    if (a == (F | 0x5aULL << 56)) return "frame, tag 0x5a";
    if (a == LSP) return "lsp";
    if (a == HALF) return "half";
    if (a == HOLE) return "hole";
    if (a == NONE_TOP) return "none-top";
    if (a == 1ULL << 48) return "2^48";
    if (a == (uint64_t)(uintptr_t)sf_after) return "after-svc";
    if (a == (uint64_t)(uintptr_t)sf_kill_sp_after) return "after-svc";
    if (a == (uint64_t)(uintptr_t)sf_landed) return "landed";
    snprintf(b, 24, "%#llx", (unsigned long long)a);
    return b;
}

static void wr64(unsigned char *f, unsigned off, uint64_t v) { memcpy(f + off, &v, 8); }
static void wr32(unsigned char *f, unsigned off, uint32_t v) { memcpy(f + off, &v, 4); }

/* A frame rt_sigreturn takes back: resumes at sf_landed with x0 0x5a5a, xN
 * 0x100+N, sp LSP, NZCV set to ZC, SIGUSR2 blocked; one fpsimd_context and the
 * terminator. Returns the offset past the fpsimd_context. */
static unsigned frame(unsigned char *f, uint64_t pstate) {
    memset(f, 0, FR);
    stack_t ss;
    sigaltstack(NULL, &ss);
    wr64(f, UC + 16, (uint64_t)(uintptr_t)ss.ss_sp);
    wr32(f, UC + 24, (uint32_t)ss.ss_flags);
    wr64(f, UC + 32, (uint64_t)ss.ss_size);
    wr64(f, MASKOFF, 1ULL << (SIGUSR2 - 1));
    for (int i = 0; i < 31; i++) wr64(f, REGS + 8 * i, 0x100 + i);
    wr64(f, REGS, 0x5a5a);
    wr64(f, SPOFF, LSP);
    wr64(f, PCOFF, (uint64_t)(uintptr_t)sf_landed);
    wr64(f, PSOFF, pstate);
    wr32(f, RSV, FPSIMD_MAGIC_);
    wr32(f, RSV + 4, 528);
    return RSV + 528;
}

static void rec(unsigned char *f, unsigned off, uint32_t magic, uint32_t size) {
    wr32(f, off, magic);
    wr32(f, off + 4, size);
}

static void sigret(const char *name, uint64_t sp) {
    char b[8][24];
    memset(&S, 0, sizeof S);
    memset(sf_land, 0, sizeof sf_land);
    int r = sigsetjmp(jb, 1);
    if (r == 0) {
        sf_sigreturn(sp);
        return;
    }
    if (r == 2) {
        printf("%-11s resumed x0 %#llx x1 %#llx x28 %#llx sp %s nzcv %#llx\n", name,
               (unsigned long long)sf_land[0], (unsigned long long)sf_land[1],
               (unsigned long long)sf_land[2], sym(sf_land[3], b[0]),
               (unsigned long long)sf_land[4]);
        return;
    }
    printf("%-11s SIGSEGV code %d addr %s | pc %s sp %s x0 %#llx x1 %#llx x2 %#llx "
           "x28 %#llx pstate %#llx | %s | fault_address %s esr %s\n",
           name, S.code, sym(S.addr, b[0]), sym(S.pc, b[1]), sym(S.sp, b[2]),
           (unsigned long long)S.x0, (unsigned long long)S.x1, (unsigned long long)S.x2,
           (unsigned long long)S.x28, (unsigned long long)S.pstate,
           (S.mask & (1ULL << (SIGUSR2 - 1))) ? "usr2 blocked" : "mask as called",
           sym(S.fa, b[3]), S.esr);
}

/* ---- a frame that cannot be written ---------------------------------- */

static volatile int usr1_ran;
static void on_usr1(int s) { (void)s; usr1_ran++; }

static void child_end(const char *name, pid_t pid) {
    int st;
    waitpid(pid, &st, 0);
    if (WIFSIGNALED(st)) printf("%-11s child killed by %s\n", name, strsignal(WTERMSIG(st)));
    else printf("%-11s child exited %d\n", name, WEXITSTATUS(st));
}

static void prime(void) {
    /* A fault of its own first, so what follows shows whether it kept the
     * fault record or cleared it. */
    char b[2][24];
    memset(&S, 0, sizeof S);
    if (sigsetjmp(jb, 1) == 0) {
        *(volatile char *)(uintptr_t)HOLE;
        return;
    }
    printf("%-11s SIGSEGV code %d addr %s | fault_address %s esr %s\n", "prime", S.code,
           sym(S.addr, b[0]), sym(S.fa, b[1]), S.esr);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    long pg = sysconf(_SC_PAGESIZE);

    /* The alternate stack every SIGSEGV here is taken on. */
    stack_t ss;
    ss.ss_size = 65536;
    ss.ss_sp = mmap(NULL, ss.ss_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ss.ss_flags = 0;

    /* ---- delivery ---- */
    geometry("siginfo", SA_SIGINFO, 0, 0);
    geometry("plain", 0, 0, 0);
    geometry("misaligned", SA_SIGINFO, 1, 0);
    sigaltstack(&ss, NULL);
    geometry("onstack", SA_SIGINFO | SA_ONSTACK, 0,
             (uint64_t)(uintptr_t)ss.ss_sp + ss.ss_size);

    /* SA_RESTORER: the handler returns through the restorer it names. */
    struct ksa ka = { (uint64_t)(uintptr_t)sf_entry, SA_SIGINFO | K_SA_RESTORER,
                      (uint64_t)(uintptr_t)sf_restorer, 0 }, ko;
    ksigaction(SIGUSR2, &ka, NULL);
    memset(&H, 0, sizeof H);
    sf_rcount = 0;
    sf_kill(syscall(SYS_gettid), SIGUSR2, 0xabcd);
    printf("%-11s lr %s, ran %llu\n", "restorer",
           sf_ent[4] == (uint64_t)(uintptr_t)sf_restorer ? "the restorer" : "other",
           (unsigned long long)sf_rcount);
    memset(&ko, 0, sizeof ko);
    ksigaction(SIGUSR2, NULL, &ko);
    printf("%-11s flags %#llx restorer %s\n", "readback", (unsigned long long)ko.flags,
           ko.restorer == (uint64_t)(uintptr_t)sf_restorer ? "kept" : "lost");
    /* Flags the kernel does not know are dropped (do_sigaction's
     * UAPI_SA_FLAGS), SA_UNSUPPORTED the one there to show it. */
    ka.flags = SA_SIGINFO | K_SA_UNSUPPORTED | 0x00200000UL;
    ka.restorer = 0;
    ksigaction(SIGUSR2, &ka, NULL);
    ksigaction(SIGUSR2, NULL, &ko);
    printf("%-11s flags %#llx\n", "unknown", (unsigned long long)ko.flags);
    /* libc's own sigaction, for what it asks of the kernel. */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    ksigaction(SIGSEGV, NULL, &ko);
    printf("%-11s flags %#llx\n", "libc", (unsigned long long)ko.flags);

    /* What a frame needs, and the least an alternate stack may be. */
    printf("%-11s AT_MINSIGSTKSZ %lu\n", "minsigstksz", getauxval(AT_MINSIGSTKSZ));
    {
        stack_t t, keep;
        sigaltstack(NULL, &keep);
        sigaltstack(&(stack_t){ .ss_flags = SS_DISABLE }, NULL);
        const size_t sizes[] = { 2048, 4096, 5119, 5120 };
        printf("%-11s", "sigaltstack");
        for (int i = 0; i < 4; i++) {
            t.ss_sp = ss.ss_sp;
            t.ss_size = sizes[i];
            t.ss_flags = 0;
            int r = sigaltstack(&t, NULL);
            printf(" %zu:%s", sizes[i], r == 0 ? "ok" : errno == ENOMEM ? "ENOMEM" : "?");
        }
        printf("\n");
        sigaltstack(&keep, NULL);
    }

    /* ---- rt_sigreturn ---- */
    buf = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    F = (uint64_t)(uintptr_t)buf + 32768;
    LSP = (uint64_t)(uintptr_t)buf + 16384;
    unsigned char *f = buf + 32768;
    /* A page with no page above it, and a hole two mappings apart. */
    unsigned char *two = mmap(NULL, 3 * pg, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    munmap(two + pg, pg);
    two[0] = two[2 * pg] = 1;          /* the hole's last-level table is there */
    HOLE = (uint64_t)(uintptr_t)(two + pg);
    HALF = HOLE - (REGS + 15 * 8);     /* x0..x14 readable, x15 on gone */
    unsigned char *none = mmap(NULL, 65536, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    NONE_TOP = (uint64_t)(uintptr_t)none + 65536;

    prime();

    unsigned o;
    frame(f, 0x20000000);
    sigret("good", F);
    frame(f, 0x60000000);
    sigret("sp-mis", F + 8);
    sigret("tagged", F | 0x5aULL << 56);
    sigret("toohigh", 1ULL << 48);
    sigret("unmapped", HOLE);
    {
        static unsigned char tmp[FR];
        frame(tmp, 0x60000000);
        memcpy((void *)(uintptr_t)HALF, tmp, (size_t)(HOLE - HALF));
    }
    sigret("half", HALF);

    frame(f, 0x60000005);
    sigret("pstate-el1", F);
    frame(f, 0x600003c0);
    sigret("pstate-daif", F);
    frame(f, 0x60000010);
    sigret("pstate-32", F);
    frame(f, 0x60500020);
    sigret("pstate-res0", F);

    frame(f, 0x60000000);
    rec(f, RSV, 0, 0);
    sigret("no-fpsimd", F);
    frame(f, 0x60000000);
    rec(f, RSV, FPSIMD_MAGIC_, 512);
    sigret("fpsimd-512", F);
    o = frame(f, 0x60000000);
    rec(f, o, FPSIMD_MAGIC_, 528);
    sigret("two-fpsimd", F);
    o = frame(f, 0x60000000);
    rec(f, o, 0x12345678u, 16);
    sigret("unknown", F);
    o = frame(f, 0x60000000);
    rec(f, o, SVE_MAGIC_, 16);
    sigret("sve", F);
    o = frame(f, 0x60000000);
    rec(f, o, ESR_MAGIC_, 16);
    sigret("esr", F);
    frame(f, 0x60000000);
    memmove(f + RSV + 16, f + RSV, 528);
    memset(f + RSV, 0, 16);
    rec(f, RSV, ESR_MAGIC_, 16);
    sigret("esr-first", F);
    o = frame(f, 0x60000000);
    rec(f, o, ESR_MAGIC_, 24);
    sigret("esr-24", F);
    o = frame(f, 0x60000000);
    rec(f, o, ESR_MAGIC_, 4);
    sigret("esr-4", F);
    o = frame(f, 0x60000000);
    rec(f, o, ESR_MAGIC_, 8192);
    sigret("esr-8192", F);
    o = frame(f, 0x60000000);
    rec(f, o, 0, 16);
    sigret("end-16", F);
    o = frame(f, 0x60000000);
    for (unsigned a = o; a < 4096 + RSV; a += 16) rec(f, a, ESR_MAGIC_, 16);
    sigret("no-end", F);
    /* extra_context: its data straight after the terminator that follows
     * it, as setup_sigframe lays it. */
    o = frame(f, 0x60000000);
    rec(f, o, EXTRA_MAGIC_, 32);
    wr64(f, o + 8, F + o + 48);
    wr32(f, o + 16, 16);
    sigret("extra", F);
    frame(f, 0x60000000);
    memmove(f + RSV + 48, f + RSV, 528);
    memset(f + RSV, 0, 48);
    rec(f, RSV, EXTRA_MAGIC_, 32);
    wr64(f, RSV + 8, F + RSV + 48);
    wr32(f, RSV + 16, 528 + 16);
    sigret("extra-fp", F);
    o = frame(f, 0x60000000);
    rec(f, o, EXTRA_MAGIC_, 32);
    wr64(f, o + 8, F + o + 64);
    wr32(f, o + 16, 16);
    sigret("extra-bad", F);

    /* ---- a frame that cannot be written ---- */
    prime();
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_usr1;
    sa.sa_flags = SA_RESETHAND;
    sigaction(SIGUSR1, &sa, NULL);
    memset(&S, 0, sizeof S);
    usr1_ran = 0;
    if (sigsetjmp(jb, 1) == 0) {
        sf_kill_sp(syscall(SYS_gettid), SIGUSR1, 0xabcd, NONE_TOP);
        printf("%-11s no signal\n", "nostack");
    } else {
        char b[4][24];
        struct sigaction now;
        sigaction(SIGUSR1, NULL, &now);
        sigset_t pend;
        sigpending(&pend);
        printf("%-11s SIGSEGV code %d addr %s | pc %s sp %s x0 %#llx | fault_address %s "
               "esr %s | usr1 ran %d, %s, flags %#x, %s\n",
               "nostack", S.code, sym(S.addr, b[0]), sym(S.pc, b[1]), sym(S.sp, b[2]),
               (unsigned long long)S.x0, sym(S.fa, b[3]), S.esr, usr1_ran,
               now.sa_handler == on_usr1 ? "handler kept" : "handler reset",
               (unsigned)now.sa_flags, sigismember(&pend, SIGUSR1) ? "pending" : "gone");
    }

    /* SIGSEGV's own frame unwritable: fatal, handler or not. */
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        struct sigaction s2;
        memset(&s2, 0, sizeof s2);
        s2.sa_sigaction = on_segv;
        s2.sa_flags = SA_SIGINFO;     /* not on the alternate stack */
        sigaction(SIGSEGV, &s2, NULL);
        sf_ld_sp(NONE_TOP);
        _exit(3);
    }
    child_end("segv-nost", pid);
    /* ...and the SIGSEGV a failed delivery raises, when its frame cannot be
     * written either. */
    pid = fork();
    if (pid == 0) {
        struct sigaction s2;
        memset(&s2, 0, sizeof s2);
        s2.sa_sigaction = on_segv;
        s2.sa_flags = SA_SIGINFO;
        sigaction(SIGSEGV, &s2, NULL);
        signal(SIGUSR1, on_usr1);     /* the one above reset itself */
        sf_kill_sp(syscall(SYS_gettid), SIGUSR1, 0xabcd, NONE_TOP);
        _exit(3);
    }
    child_end("usr1-nost", pid);
    /* SA_RESTORER with no restorer: the handler returns to 0. */
    pid = fork();
    if (pid == 0) {
        struct ksa k0 = { (uint64_t)(uintptr_t)on_usr1, K_SA_RESTORER, 0, 0 };
        ksigaction(SIGUSR1, &k0, NULL);
        signal(SIGSEGV, SIG_DFL);
        raise(SIGUSR1);
        _exit(3);
    }
    child_end("restorer-0", pid);
    printf("done\n");
    return 0;
}
