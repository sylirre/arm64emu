/* The fault status a kernel's page tables give a fault, in the esr_context
 * of its frame: the level the walk stopped at, and translation or permission.
 *
 * A translation fault is reported at the first level whose entry is empty,
 * and a kernel fills a level's table in when a page under it is first
 * faulted in: an area of 512 GB with nothing ever touched in it stops the
 * walk at level 0, one with something elsewhere in its 512 GB at level 1, in
 * its 1 GB at level 2, in its 2 MB at level 3. A page that is mapped but may
 * not be accessed so is a permission fault only once it has an entry of its
 * own -- once it has been faulted in -- and a translation fault before; and a
 * PROT_NONE page has no valid entry ever, so it is a translation fault
 * always (at level 3 once it had a page). The signal is the mapping's, not
 * the status's: SEGV_ACCERR wherever there is a mapping, SEGV_MAPERR where
 * there is none.
 *
 * The cases build that up in an area of their own, 0x500000000000 upwards
 * (512 GB-aligned, far from a program, its libraries and its stack), from
 * nothing, so that each answer is the kernel's whatever ran before; every
 * page is anonymous (no fault-around) and small (no huge page).
 *
 * Self-checking: qemu-user keeps no esr_context. The expected block is what
 * a 6.8 kernel gave this program (qemu-system, neoverse-n1). */
#define _GNU_SOURCE
#undef _FORTIFY_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

__asm__(".text\n.balign 4\n"
        ".global fl_br\n.type fl_br, %function\nfl_br:\n    br x0\n");
void fl_br(uint64_t);

#define ESR_MAGIC_ 0x45535201u
#define A  0x500000000000ULL          /* the area */
#define B  0x580000000000ULL          /* another, left empty */
#define MB2 (2ULL << 20)
#define GB  (1ULL << 30)

static sigjmp_buf jb;
static int got_sig, got_code;
static uint64_t got_addr, got_esr;

static void on_sig(int sig, siginfo_t *si, void *ucv) {
    ucontext_t *uc = ucv;
    got_sig = sig;
    got_code = si->si_code;
    got_addr = (uint64_t)(uintptr_t)si->si_addr;
    got_esr = 0;
    unsigned char *r = (unsigned char *)uc->uc_mcontext.__reserved;
    for (unsigned off = 0; off + 8 <= 4096; ) {
        uint32_t magic, size;
        memcpy(&magic, r + off, 4);
        memcpy(&size, r + off + 4, 4);
        if (!magic || size < 8) break;
        if (magic == ESR_MAGIC_) memcpy(&got_esr, r + off + 8, 8);
        off += size;
    }
    siglongjmp(jb, 1);
}

static void *map(uint64_t at, size_t len, int prot) {
    void *p = mmap((void *)(uintptr_t)at, len, prot,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != (void *)(uintptr_t)at) {
        printf("cannot map %#llx\n", (unsigned long long)at);
        _exit(1);
    }
    return p;
}

static void touch(uint64_t at) { *(volatile char *)(uintptr_t)at = 1; }

enum { RD, WR, EX };

static void probe(const char *name, uint64_t at, int how) {
    got_sig = 0;
    if (sigsetjmp(jb, 1) == 0) {
        if (how == RD) (void)*(volatile char *)(uintptr_t)at;
        else if (how == WR) *(volatile char *)(uintptr_t)at = 2;
        else fl_br(at);
        printf("%-12s no fault\n", name);
        return;
    }
    printf("%-12s %s code %d addr %s esr %#llx\n", name,
           got_sig == SIGSEGV ? "SIGSEGV" : "other", got_code,
           got_addr == at ? "ok" : "other", (unsigned long long)got_esr);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_sig;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);

    long pg = sysconf(_SC_PAGESIZE);
    if (pg != 4096) {
        printf("page size %ld\n", pg);
        return 1;
    }

    /* The walk's level, as the area fills in around A + 4K. */
    probe("level0", A + 0x1000, RD);
    probe("level0-x", B + 0x1000, EX);
    map(A + 3 * GB, 4096, PROT_READ | PROT_WRITE);
    touch(A + 3 * GB);
    probe("level1", A + 0x1000, RD);
    map(A + 5 * MB2, 4096, PROT_READ | PROT_WRITE);
    touch(A + 5 * MB2);
    probe("level2", A + 0x1000, RD);
    probe("level2-w", A + 0x1000, WR);
    map(A + 0x2000, 4096, PROT_READ | PROT_WRITE);
    touch(A + 0x2000);
    probe("level3", A + 0x1000, RD);
    probe("level3-w", A + 0x1000, WR);
    probe("level3-x", A + 0x1000, EX);

    /* PROT_NONE: a translation fault, of a mapping. */
    map(A + 0x3000, 4096, PROT_NONE);
    probe("none", A + 0x3000, RD);
    map(A + 9 * MB2, 65536, PROT_NONE);
    probe("none-alone", A + 9 * MB2 + 0x4000, RD);
    map(A + 11 * MB2, 4096, PROT_READ | PROT_WRITE);
    touch(A + 11 * MB2);
    mprotect((void *)(uintptr_t)(A + 11 * MB2), 4096, PROT_NONE);
    probe("none-was-rw", A + 11 * MB2, WR);

    /* Read-only and not executable: permission once faulted in. */
    map(A + 0x4000, 4096, PROT_READ);
    probe("ro-fresh", A + 0x4000, WR);
    map(A + 0x5000, 4096, PROT_READ);
    (void)*(volatile char *)(uintptr_t)(A + 0x5000);
    probe("ro-read", A + 0x5000, WR);
    map(A + 0x6000, 4096, PROT_READ | PROT_WRITE);
    probe("nx-fresh", A + 0x6000, EX);
    map(A + 0x7000, 4096, PROT_READ | PROT_WRITE);
    touch(A + 0x7000);
    probe("nx-touched", A + 0x7000, EX);

    /* A hole between pages that are there. */
    probe("hole", A + 0x8000, RD);
    printf("done\n");
    return 0;
}
