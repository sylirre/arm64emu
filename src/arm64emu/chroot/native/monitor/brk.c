/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* The guest's program break.
 *
 * The kernel keeps one break per process, and in this process it is not the
 * guest's to move: the native tier runs on the host's libc, whose allocator grows
 * its heap with it (glibc's main arena, musl's, a static glibc's TLS block at
 * startup) and caches where it left it. A guest moving the same break would
 * grow its heap from wherever the host's ended, and the host's next sbrk
 * would set the break back below the guest's heap -- unmapping it -- or past
 * it, onto memory the guest already owns. So brk is trapped (seccomp.c) and
 * the guest's break is kept here, in a region of its own, by the kernel's own
 * rules (mm/mmap.c, SYSCALL_DEFINE1(brk)):
 *
 *  - the break is any address from start_brk up, returned exactly as asked;
 *    the pages behind it run to the page-aligned break, and anything below
 *    start_brk -- brk(0) among them -- answers with the break unchanged;
 *  - a move is refused, returning the old break, if the heap and the data
 *    segment together would exceed RLIMIT_DATA (check_data_rlimit);
 *  - a shrink always succeeds and unmaps what it gives back;
 *  - a growth needs the pages it adds free, and one page past them: it may
 *    not bring the heap up against the next mapping. Mapped MAP_FIXED_NOREPLACE,
 *    which a kernel before 4.17 takes as a hint: a mapping that lands
 *    elsewhere is given back and the growth refused.
 *
 * Where start_brk is, is load_elf_binary's: past the end of the program's
 * memory (elf_brk), a page beyond it and up to 1 GiB further at random
 * (arch_randomize_brk) unless randomization is off -- for a program at the
 * addresses it was linked at. An ET_DYN program is the loader's to place,
 * and it lies in the mmap region with the next library directly above it,
 * where a heap has no room; the kernel puts such a program's heap above
 * ELF_ET_DYN_BASE, which in this process is where arm64emu's own image and
 * the host's heap are. So the guest's goes there too, past the host's break
 * by BRK_HOST_ROOM -- what the host's heap may grow into before it meets the
 * guest's, where its allocator falls back to mmap as it does at any other
 * mapping -- and the same random offset after that.
 *
 * Per process, as the kernel's is: a fork child carries a copy with the
 * memory it describes, threads share it under one lock, and an emulated
 * execve places the next program's (cng_brk_place) after the exec sweep has
 * had the old heap back with the rest of the old program's memory. */
#include "cng/loader.h"
#include "cng/monitor.h"
#include "cng/syscall.h"
#include "cng/uapi.h"

#define BRK_HOST_ROOM   (256UL << 20)
#define BRK_RANDOM_SPAN (1UL << 30) /* arch_randomize_brk, 64-bit task */
#define ADDR_NO_RANDOMIZE 0x0040000

static unsigned long g_start; /* start_brk; 0 until a program is placed */
static unsigned long g_cur;   /* the break, as last set */
static unsigned long g_data;  /* end_data - start_data */

/* Held by tid. A fork child copies the word as the fork found it, naming a
 * thread the child does not have, and the probe below takes it over. Every
 * other signal is masked while the handler runs, so the holder cannot be
 * interrupted into a brk of its own. */
static long g_owner;

static void brk_lock(void) {
    long me = sys_gettid();
    for (int spin = 0;; spin++) {
        long cur = 0;
        if (__atomic_compare_exchange_n(&g_owner, &cur, me, 0, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED))
            return;
        if (spin > 100 &&
            CNG_SYS(__NR_tgkill, sys_getpid(), cur, 0, 0, 0, 0) == -ESRCH)
            __atomic_compare_exchange_n(&g_owner, &cur, 0, 0, __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED);
        CNG_SYS(__NR_sched_yield, 0, 0, 0, 0, 0, 0);
    }
}

static void brk_unlock(void) { __atomic_store_n(&g_owner, 0, __ATOMIC_RELEASE); }

/* Whether the kernel would randomize this program's break: the personality
 * the exec runs under, and kernel.randomize_va_space at 2. Where either
 * cannot be read the default is assumed, which is randomization. */
static int brk_randomized(void) {
    long pers = CNG_SYS(__NR_personality, 0xffffffffUL, 0, 0, 0, 0, 0);
    if (pers >= 0 && (pers & ADDR_NO_RANDOMIZE))
        return 0;
    long fd = sys_openat(CNG_AT_FDCWD, "/proc/sys/kernel/randomize_va_space",
                         CNG_O_RDONLY | CNG_O_CLOEXEC, 0);
    if (fd < 0)
        return 1;
    char c = '2';
    long n = sys_read((int)fd, &c, 1);
    sys_close((int)fd);
    return n != 1 || c >= '2';
}

/* randomize_page(start, range). */
static unsigned long brk_random(unsigned long start, unsigned long range) {
    unsigned long pages = range / cng_page_size, r = 0;
    if (sys_getrandom(&r, sizeof r, 1 /* GRND_NONBLOCK */) != sizeof r) {
        unsigned long t;
        __asm__ volatile("mrs %0, cntvct_el0" : "=r"(t));
        r = t * 0x9e3779b97f4a7c15UL;
    }
    return pages ? start + (r % pages) * cng_page_size : start;
}

void cng_brk_place(const struct cng_loaded *prog) {
    int rnd = brk_randomized();
    unsigned long start;
    if (prog->is_dyn) {
        long host = CNG_SYS(__NR_brk, 0, 0, 0, 0, 0, 0);
        start = cng_page_up(host > 0 ? (unsigned long)host : prog->elf_brk) +
                BRK_HOST_ROOM;
    } else {
        start = cng_page_up(prog->elf_brk);
        if (rnd)
            start += cng_page_size; /* the gap the kernel leaves past .bss */
    }
    if (rnd)
        start = brk_random(start, BRK_RANDOM_SPAN);
    brk_lock();
    g_start = g_cur = start;
    g_data = prog->end_data - prog->start_data;
    brk_unlock();
}

int cng_brk_extent(unsigned long *start, unsigned long *cur) {
    brk_lock();
    *start = g_start;
    *cur = g_cur;
    brk_unlock();
    return *start != 0;
}

long cng_brk(unsigned long brk) {
    brk_lock();
    unsigned long orig = g_cur;
    if (!g_start || brk < g_start)
        goto out;
    struct cng_rlimit rl;
    if (sys_prlimit64(0, CNG_RLIMIT_DATA, 0, &rl) == 0 &&
        rl.cur != CNG_RLIM_INFINITY && (brk - g_start) + g_data > rl.cur)
        goto out;
    unsigned long newa = cng_page_up(brk), olda = cng_page_up(g_cur);
    if (newa == olda)
        goto set;
    if (brk <= g_cur) {
        if (sys_munmap((void *)newa, olda - newa) != 0)
            goto out;
        goto set;
    }
    if (newa < olda)
        goto out; /* the alignment wrapped: past every address there is */
    /* The page past the growth: mincore answers ENOMEM for a range with an
     * unmapped page in it. Any other refusal says nothing about the page, and
     * the growth is not held back on it. */
    unsigned char vec;
    if (CNG_SYS(__NR_mincore, newa, cng_page_size, &vec, 0, 0, 0) == 0)
        goto out;
    long m = (long)sys_mmap((void *)olda, newa - olda,
                            CNG_PROT_READ | CNG_PROT_WRITE,
                            CNG_MAP_PRIVATE | CNG_MAP_ANONYMOUS |
                                CNG_MAP_FIXED_NOREPLACE,
                            -1, 0);
    if (m < 0 && m > -4096)
        goto out;
    if ((unsigned long)m != olda) {
        sys_munmap((void *)m, newa - olda);
        goto out;
    }
set:
    g_cur = brk;
    brk_unlock();
    return (long)brk;
out:
    brk_unlock();
    return (long)orig;
}
