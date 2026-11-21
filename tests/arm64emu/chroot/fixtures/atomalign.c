/* Which accesses must be aligned, and what their fault leaves in the frame.
 *
 * Without FEAT_LSE2 every exclusive (LDXR/STXR, the acquire/release and pair
 * forms), every ordered access (LDAR/STLR, LDAPR, LDAPUR/STLUR) and every LSE
 * atomic (LD<op>, SWP, CAS, CASP) takes an alignment fault at an address not
 * aligned to the size it accesses -- a pair's to the whole pair -- which Linux
 * answers with SIGBUS/BUS_ADRALN (arch/arm64/mm/fault.c do_alignment_fault):
 * si_addr and the frame's fault_address are the address, the ESR record has
 * DFSC 0x21, and WnR for a store and for an atomic that also writes. A plain
 * load or store runs (SCTLR_EL1.A is 0), and a byte never faults. The check
 * comes ahead of translation and permission -- a hole or a read-only page
 * still takes the alignment fault -- and a store-exclusive takes it whatever
 * the monitor holds (AArch64.ExclusiveMonitorsPass checks the alignment
 * first).
 *
 * Self-checking: qemu-user keeps no ESR record. Each case runs 200 times
 * aligned first, so that every engine has translated it, then twice at its
 * address (the two must agree); one line per case: the signal, si_code and
 * si_addr, the frame's fault_address and ESR. Where the CPU has no LRCPC2
 * (AT_HWCAP), the LDAPUR/STLUR cases print "skipped". */
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

/* One instruction on the address in x0. */
#define FN(n, body)                                                         \
    ".global aa_" #n "\n.type aa_" #n ", %function\naa_" #n ":\n" body      \
    "\n    ret\n"
__asm__(".arch armv8.4-a\n.text\n.balign 4\n"
        FN(ldxrh,    "    ldxrh   w2, [x0]")
        FN(ldxr_w,   "    ldxr    w2, [x0]")
        FN(ldaxr,    "    ldaxr   x2, [x0]")
        FN(stxr,     "    clrex\n    stxr    w3, x2, [x0]")   /* no monitor */
        FN(stlxr_m,  "    bic     x1, x0, #15\n"              /* monitor on the */
                     "    ldxr    x2, [x1]\n"                 /* aligned address */
                     "    stlxr   w3, x2, [x0]")
        FN(stxrh,    "    clrex\n    stxrh   w3, w2, [x0]")
        FN(ldxp_w,   "    ldxp    w2, w3, [x0]")
        FN(ldaxp,    "    ldaxp   x2, x3, [x0]")
        FN(stxp,     "    clrex\n    stxp    w4, x2, x3, [x0]")
        FN(stlxp_w,  "    clrex\n    stlxp   w4, w2, w3, [x0]")
        FN(stxrh_m,  "    ldxrb   w2, [x0]\n"              /* the monitor on */
                     "    stxrh   w3, w2, [x0]")              /* this address */
        FN(stxp_m,   "    ldxr    x2, [x0]\n"
                     "    stxp    w4, x2, x3, [x0]")
        FN(ldarh,    "    ldarh   w2, [x0]")
        FN(ldar_w,   "    ldar    w2, [x0]")
        FN(ldar,     "    ldar    x2, [x0]")
        FN(stlrh,    "    stlrh   w2, [x0]")
        FN(stlr_w,   "    stlr    w2, [x0]")
        FN(stlr,     "    stlr    x2, [x0]")
        FN(ldaprh,   "    ldaprh  w2, [x0]")
        FN(ldapr_w,  "    ldapr   w2, [x0]")
        FN(ldapr,    "    ldapr   x2, [x0]")
        FN(ldapur,   "    ldapur  x2, [x0]")
        FN(ldapursh, "    ldapursh x2, [x0]")
        FN(stlur_w,  "    stlur   w2, [x0]")
        FN(stlur,    "    stlur   x2, [x0]")
        FN(ldadd,    "    ldadd   x2, x3, [x0]")
        FN(ldaddh,   "    ldaddh  w2, w3, [x0]")
        FN(stset_w,  "    stset   w2, [x0]")
        FN(swpal,    "    swpal   x2, x3, [x0]")
        FN(ldumax,   "    ldumax  x2, x3, [x0]")
        FN(ldsminh,  "    ldsminh w2, w3, [x0]")
        FN(cas_w,    "    cas     w2, w3, [x0]")
        FN(casal,    "    casal   x2, x3, [x0]")
        FN(casp_w,   "    casp    w2, w3, w4, w5, [x0]")
        FN(casp,     "    casp    x2, x3, x4, x5, [x0]")
        FN(ldxrb,    "    ldxrb   w2, [x0]")
        FN(stxrb,    "    clrex\n    stxrb   w3, w2, [x0]")
        FN(ldarb,    "    ldarb   w2, [x0]")
        FN(stlrb,    "    stlrb   w2, [x0]")
        FN(ldaddb,   "    ldaddb  w2, w3, [x0]")
        FN(casb,     "    casb    w2, w3, [x0]")
        FN(ldr,      "    ldr     x2, [x0]")
        FN(str,      "    str     x2, [x0]"));

typedef void Fn(uint64_t);
#define D(n) extern Fn aa_##n;
D(ldxrh) D(ldxr_w) D(ldaxr) D(stxr) D(stlxr_m) D(stxrh) D(ldxp_w) D(ldaxp)
D(stxp) D(stlxp_w) D(stxrh_m) D(stxp_m) D(ldarh) D(ldar_w) D(ldar) D(stlrh) D(stlr_w) D(stlr)
D(ldaprh) D(ldapr_w) D(ldapr) D(ldapur) D(ldapursh) D(stlur_w) D(stlur)
D(ldadd) D(ldaddh) D(stset_w) D(swpal) D(ldumax) D(ldsminh) D(cas_w)
D(casal) D(casp_w) D(casp) D(ldxrb) D(stxrb) D(ldarb) D(stlrb) D(ldaddb)
D(casb) D(ldr) D(str)

#define ESR_MAGIC_ 0x45535201u
#define HWCAP_ILRCPC_ (1UL << 26)

static long pg;
static uint64_t b, hole, ro, cross;
static sigjmp_buf jb;
static char res[200];

static const char *sym(uint64_t a, char *s, size_t n) {
    static const struct { const char *name; uint64_t *base; } bases[] = {
        { "b", &b }, { "hole", &hole }, { "ro", &ro }, { "cross", &cross },
    };
    if (a == 0) return "0";
    for (unsigned i = 0; i < sizeof bases / sizeof bases[0]; i++) {
        uint64_t d = a - *bases[i].base;
        if (d < 64) {
            if (d) snprintf(s, n, "%s+%llu", bases[i].name, (unsigned long long)d);
            else snprintf(s, n, "%s", bases[i].name);
            return s;
        }
    }
    snprintf(s, n, "%#llx", (unsigned long long)a);
    return s;
}

static void on_sig(int sig, siginfo_t *si, void *uc_) {
    ucontext_t *uc = uc_;
    char b1[24], b2[24], e[48] = "none";
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
    snprintf(res, sizeof res, "%s code %d addr %s | fault_address %s | esr %s",
             sig == SIGBUS ? "SIGBUS" : sig == SIGSEGV ? "SIGSEGV" :
             sig == SIGILL ? "SIGILL" : "?", si->si_code,
             sym((uint64_t)(uintptr_t)si->si_addr, b1, sizeof b1),
             sym(uc->uc_mcontext.fault_address, b2, sizeof b2), e);
    siglongjmp(jb, 1);
}

/* Run fn at a: res is what it did. */
static void run(Fn *fn, uint64_t a) {
    strcpy(res, "no signal");
    if (!sigsetjmp(jb, 1)) fn(a);
}

static void t(const char *what, Fn *fn, uint64_t a) {
    char first[200];
    for (int i = 0; i < 200; i++) {             /* aligned: every engine's */
        run(fn, b + 16);
        if (strcmp(res, "no signal")) {
            printf("%-9s BAD: aligned, %s\n", what, res);
            return;
        }
    }
    run(fn, a);
    strcpy(first, res);
    run(fn, a);
    if (strcmp(first, res)) printf("%-9s BAD: %s, then %s\n", what, first, res);
    else printf("%-9s %s\n", what, res);
    fflush(stdout);
}

int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_sig;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGBUS, &sa, 0);
    sigaction(SIGSEGV, &sa, 0);
    sigaction(SIGILL, &sa, 0);
    int rcpc2 = (getauxval(AT_HWCAP) & HWCAP_ILRCPC_) != 0;

    /* Five pages, all touched: [0] the scratch b, [1] a hole, [2] read-only,
     * [3] and [4] in one, cross straddling them. The hole sits between
     * touched pages, so its fault is a level-3 translation fault. */
    pg = sysconf(_SC_PAGESIZE);
    unsigned char *m = mmap(0, 5 * pg, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) return 1;
    memset(m, 1, 5 * pg);
    munmap(m + pg, pg);
    mprotect(m + 2 * pg, pg, PROT_READ);
    b = (uint64_t)(uintptr_t)m + 64;
    hole = (uint64_t)(uintptr_t)(m + pg);
    ro = (uint64_t)(uintptr_t)(m + 2 * pg);
    cross = (uint64_t)(uintptr_t)(m + 4 * pg) - 4;

    /* Exclusives: the size, a pair's the pair's; a store-exclusive with no
     * monitor, with one on another address, and with one on its own that a
     * narrower load-exclusive set. */
    t("ldxrh",    aa_ldxrh,   b + 1);
    t("ldxr w",   aa_ldxr_w,  b + 2);
    t("ldaxr",    aa_ldaxr,   b + 4);
    t("ldaxr 16", aa_ldaxr,   b + 12);          /* crossing 16 bytes */
    t("stxr",     aa_stxr,    b + 4);
    t("stlxr m",  aa_stlxr_m, b + 4);
    t("stxrh",    aa_stxrh,   b + 1);
    t("ldxp w",   aa_ldxp_w,  b + 4);
    t("ldaxp",    aa_ldaxp,   b + 8);
    t("stxp",     aa_stxp,    b + 8);
    t("stlxp w",  aa_stlxp_w, b + 4);
    t("stxrh m",  aa_stxrh_m, b + 1);           /* a narrower LDXR set the */
    t("stxp m",   aa_stxp_m,  b + 8);           /* monitor on this address */
    /* Ordered accesses. */
    t("ldarh",    aa_ldarh,   b + 1);
    t("ldar w",   aa_ldar_w,  b + 2);
    t("ldar",     aa_ldar,    b + 4);
    t("stlrh",    aa_stlrh,   b + 1);
    t("stlr w",   aa_stlr_w,  b + 2);
    t("stlr",     aa_stlr,    b + 12);          /* crossing 16 bytes */
    t("ldaprh",   aa_ldaprh,  b + 1);
    t("ldapr w",  aa_ldapr_w, b + 2);
    t("ldapr",    aa_ldapr,   b + 4);
    /* LRCPC2, each crossing 16 bytes. */
    if (rcpc2) {
        t("ldapur",   aa_ldapur,   b + 12);
        t("ldapursh", aa_ldapursh, b + 15);
        t("stlur w",  aa_stlur_w,  b + 14);
        t("stlur",    aa_stlur,    b + 9);
    } else {
        printf("ldapur    skipped\nldapursh  skipped\n"
               "stlur w   skipped\nstlur     skipped\n");
    }
    /* LSE atomics: a write, whatever they read. */
    t("ldadd",    aa_ldadd,   b + 4);
    t("ldaddh",   aa_ldaddh,  b + 1);
    t("stset w",  aa_stset_w, b + 2);
    t("swpal",    aa_swpal,   b + 4);
    t("ldumax",   aa_ldumax,  b + 4);
    t("ldsminh",  aa_ldsminh, b + 1);
    t("cas w",    aa_cas_w,   b + 2);
    t("casal",    aa_casal,   b + 4);
    t("casp w",   aa_casp_w,  b + 4);
    t("casp",     aa_casp,    b + 8);
    /* Ahead of translation, of permission, at a page boundary. */
    t("hole",     aa_ldar,    hole + 4);
    t("hole al",  aa_ldar,    hole);
    t("ro stxr",  aa_stxr,    ro + 4);
    t("ro ldadd", aa_ldadd,   ro + 4);
    t("ro al",    aa_stlr,    ro);
    t("cross",    aa_ldaxr,   cross);
    t("tagged",   aa_ldxr_w,  (b + 2) | 0x5aULL << 56);
    /* Bytes, and plain accesses: never. */
    t("ldxrb",    aa_ldxrb,   b + 1);
    t("stxrb",    aa_stxrb,   b + 1);
    t("ldarb",    aa_ldarb,   b + 3);
    t("stlrb",    aa_stlrb,   b + 3);
    t("ldaddb",   aa_ldaddb,  b + 1);
    t("casb",     aa_casb,    b + 1);
    t("ldr",      aa_ldr,     b + 4);
    t("str",      aa_str,     cross);
    return 0;
}
