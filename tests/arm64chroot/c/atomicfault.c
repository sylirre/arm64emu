/* An LSE atomic or a store-exclusive to a page whose protection refuses the
 * write is SEGV_ACCERR, like any store there, and only one to no mapping at
 * all is SEGV_MAPERR. A CAS takes the fault whatever its comparison finds: the
 * architecture translates it as a write either way. The emulator, where guest
 * threads run concurrently, performs these on a host pointer, and where the
 * page's protection gave it none it made up a fault of its own: SEGV_MAPERR,
 * a page that is not there, for every atomic on a read-only one, and a
 * store-exclusive pair simply failed, so its retry loop would spin for ever.
 * si_addr is the address accessed, and the register the faulting instruction
 * would have written keeps its value.
 * REQUIRES: atomics
 * BUILDFLAGS: -march=armv8.1-a */
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>

#define MEM  0x5a5a5a5a5a5a5a5aULL     /* what the pages hold */
#define INIT 0x1234ULL                  /* a destination's value before */

static sigjmp_buf jb;
static volatile int sig, code;
static void *volatile addr;
static volatile uint64_t init2;     /* x2 before the faulting instruction */
static volatile uint64_t kept;      /* x2 at the fault: the destination */

static void on_fault(int s, siginfo_t *si, void *uc) {
    sig = s;
    code = si->si_code;
    addr = si->si_addr;
    kept = ((ucontext_t *)uc)->uc_mcontext.regs[2];
    siglongjmp(jb, 1);
}

/* Each form takes its address in x0 and writes its result to x2, which holds
 * init2 before. */
static void op_ldadd(void *p) {
    register uint64_t x0 __asm__("x0") = (uint64_t)p, x1 __asm__("x1") = 1,
                      x2 __asm__("x2") = init2 = INIT;
    __asm__ volatile("ldadd x1, x2, [x0]" : "+r"(x2) : "r"(x0), "r"(x1) : "memory");
}
static void op_swp(void *p) {
    register uint64_t x0 __asm__("x0") = (uint64_t)p, x1 __asm__("x1") = 1,
                      x2 __asm__("x2") = init2 = INIT;
    __asm__ volatile("swpal x1, x2, [x0]" : "+r"(x2) : "r"(x0), "r"(x1) : "memory");
}
static void op_cas_match(void *p) {          /* compares equal */
    register uint64_t x0 __asm__("x0") = (uint64_t)p, x1 __asm__("x1") = 7,
                      x2 __asm__("x2") = init2 = MEM;
    __asm__ volatile("casal x2, x1, [x0]" : "+r"(x2) : "r"(x0), "r"(x1) : "memory");
}
static void op_cas_differ(void *p) {         /* compares unequal */
    register uint64_t x0 __asm__("x0") = (uint64_t)p, x1 __asm__("x1") = 7,
                      x2 __asm__("x2") = init2 = INIT;
    __asm__ volatile("casal x2, x1, [x0]" : "+r"(x2) : "r"(x0), "r"(x1) : "memory");
}
static void op_casp_differ(void *p) {
    register uint64_t x0 __asm__("x0") = (uint64_t)p, x2 __asm__("x2") = init2 = INIT,
                      x3 __asm__("x3") = INIT, x4 __asm__("x4") = 7,
                      x5 __asm__("x5") = 7;
    __asm__ volatile("caspal x2, x3, x4, x5, [x0]"
                     : "+r"(x2), "+r"(x3) : "r"(x0), "r"(x4), "r"(x5) : "memory");
}
static void op_stxr(void *p) {               /* the load succeeds, the store not */
    register uint64_t x0 __asm__("x0") = (uint64_t)p, x1 __asm__("x1") = 0,
                      x2 __asm__("x2") = init2 = INIT;
    __asm__ volatile("ldxr x1, [x0]\n\tstxr w2, x1, [x0]"
                     : "+r"(x2), "+r"(x1) : "r"(x0) : "memory");
}
static void op_stxp(void *p) {
    register uint64_t x0 __asm__("x0") = (uint64_t)p, x1 __asm__("x1") = 0,
                      x2 __asm__("x2") = init2 = INIT, x3 __asm__("x3") = 0;
    __asm__ volatile("ldxp x1, x3, [x0]\n\tstxp w2, x1, x3, [x0]"
                     : "+r"(x2), "+r"(x1), "+r"(x3) : "r"(x0) : "memory");
}

static void run(const char *what, void (*op)(void *), void *p) {
    sig = code = 0;
    addr = NULL;
    kept = 0;
    if (!sigsetjmp(jb, 1)) {
        op(p);
        printf("%-12s no fault\n", what);
        return;
    }
    printf("%-12s %s %s, si_addr %s, x2 %s\n", what,
           sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" : "?",
           sig == SIGSEGV ? (code == SEGV_ACCERR ? "SEGV_ACCERR" :
                             code == SEGV_MAPERR ? "SEGV_MAPERR" : "?")
                          : (code == BUS_ADRALN ? "BUS_ADRALN" : "?"),
           addr == p ? "ok" : "WRONG", kept == init2 ? "kept" : "written");
}

static void all(const char *page, char *p) {
    printf("-- %s\n", page);
    run("ldadd", op_ldadd, p);
    run("swpal", op_swp, p);
    run("cas (equal)", op_cas_match, p);
    run("cas (unequal)", op_cas_differ, p);
    run("casp", op_casp_differ, p);
    run("ldxr/stxr", op_stxr, p);
    run("ldxp/stxp", op_stxp, p);
}

int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    char *p = mmap(NULL, 2 * 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { printf("mmap failed\n"); return 1; }
    memset(p, 0x5a, 2 * 4096);               /* MEM, both pages populated */
    mprotect(p, 4096, PROT_READ);
    all("read-only page", p);
    munmap(p + 4096, 4096);
    all("unmapped page", p + 4096);
    return 0;
}
