/* An indirect branch to an address nothing is mapped at faults, however
 * recently code ran from the page that went away, and whatever the JIT has
 * just thrown away. Two cases, each a branch to 0 -- a call through a null
 * function pointer:
 *  - Code runs from a page whose jump-cache slot is the one address 0 lands
 *    in (the JIT keeps its indirect-branch cache by (pc >> 2) mod 4096, and
 *    0x10000000 >> 2 is 0 mod 4096), and the page is unmapped. The JIT
 *    emptied that slot by clearing its key to 0 and leaving the code
 *    pointer, so the branch to 0 matched it and ran the unmapped page's
 *    block: the null call returned 42 instead of SIGSEGV.
 *  - The JIT has just dropped every translation, which a flush-to-zero mode
 *    set for the first time makes it do where the host cannot carry the
 *    mode. Its emptied cache was all zeroes, key 0 with no code at all, and
 *    the branch to 0 jumped the emulator itself to NULL. */
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

static sigjmp_buf jb;

static void on_segv(int s, siginfo_t *si, void *uc) {
    (void)uc;
    printf("signal %d, si_addr %p\n", s, si->si_addr);
    siglongjmp(jb, 1);
}

static long call(uintptr_t f) {
    register long x0 __asm__("x0") = 0;
    __asm__ volatile("blr %1" : "+r"(x0) : "r"(f) : "x30", "memory");
    return x0;
}

int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    uint32_t *p = mmap((void *)0x10000000, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED) { printf("mmap failed\n"); return 1; }
    p[0] = 0xd2800540;   /* mov x0, #42 */
    p[1] = 0xd65f03c0;   /* ret */
    __builtin___clear_cache((char *)p, (char *)(p + 2));
    long r = 0;
    for (int i = 0; i < 100; i++) r = call((uintptr_t)p);   /* hot: translated */
    printf("code page returned %ld\n", r);
    munmap(p, 4096);
    for (int i = 0; i < 3; i++) {
        if (!sigsetjmp(jb, 1)) {
            r = call(0);
            printf("branch to 0 returned %ld\n", r);
        }
    }
    if (!sigsetjmp(jb, 1)) {
        uint64_t fz = (1ULL << 24) | (1ULL << 19);    /* FPCR.FZ, FZ16 */
        register long x0 __asm__("x0") = 0;
        __asm__ volatile("msr fpcr, %1\n\tmov x16, #0\n\tblr x16"
                         : "+r"(x0) : "r"(fz) : "x16", "x30", "memory");
        printf("branch to 0 after the flush returned %ld\n", x0);
    }
    __asm__ volatile("msr fpcr, xzr");
    return 0;
}
