/* The SP-alignment check (SCTLR_EL1.SA0), as a Linux kernel sets it: a load or
 * store whose base register is SP, with SP not 16-byte aligned, takes an
 * SP-alignment fault, which the kernel answers with SIGBUS, BUS_ADRALN and
 * si_addr = SP (el0_sp -> do_sp_pc_abort(regs->sp)). A prefetch never checks,
 * nor does an access whose base is another register, or one through an aligned
 * SP at an unaligned address.
 *
 * Self-checking, because qemu-user never checks SP alignment. Each case is an
 * assembly function that saves SP in x9, drops it by 64 + `off`, sets x6 as a
 * marker, then runs the one access at a global label. It runs 200 times with
 * off = 0 first, so the access is in translated and predecoded code, then with
 * off = 8; then 10 times aligned again and a second time misaligned, from code
 * that has already faulted once. The handler checks the frame against what
 * the kernel gives: the PC of the access, SP as it was before it (no
 * writeback), si_addr = that SP, the marker set before the access committed,
 * and its own stack 16-byte aligned. */
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ucontext.h>

__asm__(".arch armv8.4-a\n");     /* LSE atomics, CAS, LDAPUR */

#define CASE(name, setup, insn)                                         \
    __asm__(".text\n.balign 4\n"                                        \
            ".global spal_" #name "\n"                                  \
            ".type spal_" #name ", %function\n"                         \
            "spal_" #name ":\n"                                         \
            "    mov x9, sp\n"                                          \
            "    sub sp, sp, #64\n"                                     \
            "    sub sp, sp, x0\n"                                      \
            setup                                                       \
            "    mov x6, #0x1234\n"                                     \
            ".global spal_pc_" #name "\n"                               \
            "spal_pc_" #name ":\n"                                      \
            "    " insn "\n"                                            \
            "    mov sp, x9\n"                                          \
            "    ret\n"                                                 \
            ".size spal_" #name ", . - spal_" #name "\n");              \
    void spal_##name(unsigned long off);                                \
    extern const char spal_pc_##name[];

/* Fault: integer, every addressing form the decoders know. */
CASE(ldr,      "", "ldr x1, [sp, #8]")
CASE(ldrw,     "", "ldr w1, [sp, #4]")
CASE(ldrb,     "", "ldrb w1, [sp, #1]")
CASE(ldrsw,    "", "ldrsw x1, [sp]")
CASE(ldur,     "", "ldur x1, [sp, #-8]")
CASE(str,      "", "str x1, [sp, #16]")
CASE(strpre,   "", "str x1, [sp, #-16]!")
CASE(ldrpost,  "", "ldr x1, [sp], #16")
CASE(ldrreg,   "    mov x2, #8\n", "ldr x1, [sp, x2]")
CASE(ldp,      "", "ldp x1, x2, [sp]")
CASE(stppre,   "", "stp x1, x2, [sp, #-16]!")
CASE(ldppost,  "", "ldp x1, x2, [sp], #16")
CASE(run,      "", "ldr x1, [sp]\n    ldr x2, [sp, #8]\n    ldr x3, [sp, #16]")
/* Fault: FP/SIMD. */
CASE(ldrq,     "", "ldr q0, [sp, #16]")
CASE(strd,     "", "str d0, [sp, #8]")
CASE(ldrqpre,  "", "ldr q0, [sp, #-16]!")
CASE(ldpq,     "", "ldp q0, q1, [sp]")
CASE(stpd,     "", "stp d0, d1, [sp]")
CASE(ld1,      "", "ld1 {v0.16b}, [sp]")
CASE(st1lane,  "", "st1 {v0.d}[0], [sp]")
/* Fault: exclusives, ordered, atomics. */
CASE(ldxr,     "", "ldxr x1, [sp]")
CASE(ldar,     "", "ldar x1, [sp]")
CASE(stlr,     "", "stlr x1, [sp]")
CASE(ldadd,    "", "ldadd x1, x2, [sp]")
CASE(cas,      "", "cas x1, x2, [sp]")
CASE(ldapur,   "", "ldapur x1, [sp, #8]")
/* No fault: prefetches, another base, an aligned SP (rounded down by the setup)
 * at an unaligned address. */
CASE(prfm,     "", "prfm pldl1keep, [sp]")
CASE(prfmreg,  "    mov x2, #8\n", "prfm pldl1keep, [sp, x2]")
CASE(prfum,    "", "prfum pldl1keep, [sp, #3]")
CASE(xbase,    "    mov x3, sp\n", "ldr x1, [x3]")
CASE(unalva,   "    mov x3, sp\n    and x3, x3, #-16\n    mov sp, x3\n",
               "ldur x1, [sp, #3]")

struct Case { const char *name; void (*fn)(unsigned long); const char *pc; };
#define E(n) { #n, spal_##n, spal_pc_##n }
static const struct Case cases[] = {
    E(ldr), E(ldrw), E(ldrb), E(ldrsw), E(ldur), E(str), E(strpre), E(ldrpost),
    E(ldrreg), E(ldp), E(stppre), E(ldppost), E(run),
    E(ldrq), E(strd), E(ldrqpre), E(ldpq), E(stpd), E(ld1), E(st1lane),
    E(ldxr), E(ldar), E(stlr), E(ldadd), E(cas), E(ldapur),
    E(prfm), E(prfmreg), E(prfum), E(xbase), E(unalva),
};

static sigjmp_buf jb;
static const struct Case *cur;
static char res[160];

static void on_fault(int sig, siginfo_t *si, void *uc_) {
    ucontext_t *uc = uc_;
    uint64_t hsp;
    __asm__ volatile("mov %0, sp" : "=r"(hsp));
    if (sig != SIGBUS || si->si_code != BUS_ADRALN) {
        snprintf(res, sizeof res, "signal %d code %d", sig, si->si_code);
        siglongjmp(jb, 1);
    }
    uint64_t sp = uc->uc_mcontext.sp, x9 = uc->uc_mcontext.regs[9];
    snprintf(res, sizeof res, "SIGBUS BUS_ADRALN%s%s%s%s%s",
             uc->uc_mcontext.pc == (uint64_t)(uintptr_t)cur->pc ? "" : " BAD-pc",
             sp == x9 - 64 - 8 ? "" : " BAD-sp",
             (uint64_t)(uintptr_t)si->si_addr == sp ? "" : " BAD-si_addr",
             uc->uc_mcontext.regs[6] == 0x1234 ? "" : " BAD-x6",
             (hsp & 15) == 0 ? "" : " BAD-handler-sp");
    siglongjmp(jb, 1);
}

static void probe(char *out, size_t n) {
    strcpy(res, "none");
    if (!sigsetjmp(jb, 1)) cur->fn(8);
    snprintf(out, n, "%s", res);
}

int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGBUS, &sa, 0);
    sigaction(SIGSEGV, &sa, 0);
    sigaction(SIGILL, &sa, 0);
    for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        char r1[160], r2[160];
        cur = &cases[k];
        for (int i = 0; i < 200; i++) cur->fn(0);
        probe(r1, sizeof r1);
        for (int i = 0; i < 10; i++) cur->fn(0);
        probe(r2, sizeof r2);
        if (strcmp(r1, r2)) printf("%s: %s | again: %s\n", cur->name, r1, r2);
        else printf("%s: %s\n", cur->name, r1);
    }
    return 0;
}
