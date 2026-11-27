/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* The native tier's preflight (cng/preflight.h): can it run the guest in this
 * process? Asked while the answer can still be "no, the emulator runs it":
 * before the tier takes the process's dispositions, threads and address space
 * over. Ordinary libc code, which is what this process still is -- none of the
 * monitor's machinery is up yet -- and what the forked child that answers the
 * questions a filter would settle for good stays: it ends with _exit, whatever
 * it finds. The one rule kept is the monitor's: no libc signal wrapper (an
 * app's libsigchain interposes them), so the child resets SIGSYS raw. */
#include "cng/preflight.h"

#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

/* The seccomp UAPI, the few values the probe filter needs (not every
 * toolchain's sysroot carries <linux/seccomp.h>). */
#define PF_PR_SET_NO_NEW_PRIVS 38
#define PF_PR_SET_SECCOMP 22
#define PF_SECCOMP_MODE_FILTER 2
#define PF_RET_ALLOW 0x7fff0000u
#define PF_RET_TRAP 0x00030000u
#define PF_AUDIT_ARCH_AARCH64 0xc00000b7u
struct pf_insn {
    uint16_t code;
    uint8_t jt, jf;
    uint32_t k;
};
struct pf_prog {
    unsigned short len;
    const struct pf_insn *filter;
};
#define PF_LD_ABS 0x20 /* BPF_LD | BPF_W | BPF_ABS */
#define PF_JEQ_K 0x15  /* BPF_JMP | BPF_JEQ | BPF_K */
#define PF_RET_K 0x06  /* BPF_RET | BPF_K */

/* An errno as its name, for a message a person reads ("(EINVAL)"). */
static const char *ename(int e) {
    switch (e) {
    case EPERM: return "EPERM";
    case ENOENT: return "ENOENT";
    case EIO: return "EIO";
    case ENOMEM: return "ENOMEM";
    case EACCES: return "EACCES";
    case EFAULT: return "EFAULT";
    case EEXIST: return "EEXIST";
    case ENODEV: return "ENODEV";
    case EINVAL: return "EINVAL";
    case ENOSYS: return "ENOSYS";
    case ETXTBSY: return "ETXTBSY";
    default: {
        static char buf[24];
        snprintf(buf, sizeof buf, "errno %d", e);
        return buf;
    }
    }
}

int cng_preflight_threads(char *why, size_t n) {
    DIR *d = opendir("/proc/self/task");
    if (!d)
        return 0; /* no /proc to ask: taken as the one thread it should be */
    int k = 0;
    struct dirent *e;
    while ((e = readdir(d)))
        if (e->d_name[0] != '.')
            k++;
    closedir(d);
    if (k <= 1)
        return 0;
    snprintf(why, n, "this process has %d threads, and the native tier needs "
                     "it to itself", k);
    return 1;
}

/* What the child found, written to the pipe before it goes: the stage that
 * failed and its errno, two for the code test (the anonymous strategy's, the
 * file's). Nothing written is a pass of everything it got to. */
enum { PF_NNP = 1, PF_EXEC, PF_FILTER, PF_NOTRAP };
struct pf_res {
    int stage, err, err2;
};

static _Noreturn void pf_report(int fd, int stage, int err, int err2) {
    struct pf_res r = {stage, err, err2};
    if (write(fd, &r, sizeof r) < 0) { /* nothing more to say it with */ }
    _exit(0);
}

static _Noreturn void pf_child(int fd, const char *host_prog, int file_backed) {
    /* A core of the probe is not wanted, and the trap below is its end. */
    struct rlimit z = {0, 0};
    setrlimit(RLIMIT_CORE, &z);
    if (prctl(PF_PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
        pf_report(fd, PF_NNP, errno, 0);

    /* After no_new_privs, which is what can revoke it (Android's execmem). */
    long pg = sysconf(_SC_PAGESIZE);
    int anon_ok = 0, aerr = 0, ferr = 0;
    if (!file_backed) {
        void *p = mmap(0, (size_t)pg, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED)
            aerr = errno;
        else {
            if (mprotect(p, (size_t)pg, PROT_READ | PROT_EXEC) == 0)
                anon_ok = 1;
            else
                aerr = errno;
            munmap(p, (size_t)pg);
        }
    }
    if (!anon_ok) {
        int f = open(host_prog, O_RDONLY | O_CLOEXEC);
        if (f < 0)
            ferr = errno;
        else {
            void *p = mmap(0, (size_t)pg, PROT_READ | PROT_EXEC, MAP_PRIVATE, f, 0);
            if (p == MAP_FAILED)
                ferr = errno;
            else
                munmap(p, (size_t)pg);
            close(f);
        }
        if (ferr)
            pf_report(fd, PF_EXEC, aerr, ferr);
    }

    /* The filter: getppid traps, the rest is let through. SIGSYS at its
     * default and unblocked, so the trap ends the child by it -- which is the
     * pass. A kernel that refuses the filter says so; one that accepts it and
     * lets getppid through is an emulator that does not apply a guest's. */
    unsigned long dfl[4] = {0, 0, 0, 0}, sys = 1UL << (SIGSYS - 1);
    syscall(SYS_rt_sigaction, SIGSYS, dfl, 0, 8);
    syscall(SYS_rt_sigprocmask, SIG_UNBLOCK, &sys, 0, 8);
    static const struct pf_insn f[] = {
        {PF_LD_ABS, 0, 0, 4}, /* arch */
        {PF_JEQ_K, 0, 3, PF_AUDIT_ARCH_AARCH64},
        {PF_LD_ABS, 0, 0, 0}, /* nr */
        {PF_JEQ_K, 0, 1, SYS_getppid},
        {PF_RET_K, 0, 0, PF_RET_TRAP},
        {PF_RET_K, 0, 0, PF_RET_ALLOW},
    };
    struct pf_prog prog = {sizeof f / sizeof *f, f};
    if (prctl(PF_PR_SET_SECCOMP, PF_SECCOMP_MODE_FILTER, &prog, 0, 0) != 0)
        pf_report(fd, PF_FILTER, errno, 0);
    syscall(SYS_getppid);
    pf_report(fd, PF_NOTRAP, 0, 0);
}

/* Is [lo, hi) free of mappings? A probe mapping asked for exactly there: a
 * kernel with MAP_FIXED_NOREPLACE (4.17) refuses it EEXIST over anything
 * mapped, and one without takes the flag for none and the address for a
 * hint, which it honors only where the range is free. */
static int range_free(unsigned long lo, unsigned long hi) {
    void *p = mmap((void *)lo, hi - lo, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE,
                   -1, 0);
    if (p == MAP_FAILED)
        return errno != EEXIST; /* anything else is the load's to report */
    munmap(p, hi - lo);
    return p == (void *)lo;
}

/* A program linked at a fixed address loads there or not at all, and this
 * process -- arm64emu itself, where it is not position-independent, its heap,
 * a library -- may already be there. */
static int fixed_clash(const char *host_prog, char *why, size_t n) {
    int fd = open(host_prog, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0; /* the load says why */
    Elf64_Ehdr eh;
    Elf64_Phdr ph[64];
    int clash = 0;
    if (pread(fd, &eh, sizeof eh, 0) == (ssize_t)sizeof eh &&
        !memcmp(eh.e_ident, ELFMAG, SELFMAG) && eh.e_ident[EI_CLASS] == ELFCLASS64 &&
        eh.e_type == ET_EXEC && eh.e_phentsize == sizeof(Elf64_Phdr) &&
        eh.e_phnum && eh.e_phnum <= 64 &&
        pread(fd, ph, eh.e_phnum * sizeof *ph, (off_t)eh.e_phoff) ==
            (ssize_t)(eh.e_phnum * sizeof *ph)) {
        unsigned long pg = (unsigned long)sysconf(_SC_PAGESIZE);
        for (int i = 0; i < eh.e_phnum && !clash; i++) {
            if (ph[i].p_type != PT_LOAD || !ph[i].p_memsz)
                continue;
            unsigned long lo = ph[i].p_vaddr & ~(pg - 1);
            unsigned long hi = (ph[i].p_vaddr + ph[i].p_memsz + pg - 1) & ~(pg - 1);
            if (hi > lo && !range_free(lo, hi)) {
                snprintf(why, n, "%s is linked at a fixed address (%#lx-%#lx) "
                                 "that this process already uses",
                         host_prog, lo, hi);
                clash = 1;
            }
        }
    }
    close(fd);
    return clash;
}

int cng_preflight(const char *host_prog, int file_backed, char *why, size_t n) {
    int p[2];
    if (pipe(p) != 0) {
        snprintf(why, n, "no pipe for the probe (%s)", ename(errno));
        return 1;
    }
    fcntl(p[0], F_SETFD, FD_CLOEXEC);
    fcntl(p[1], F_SETFD, FD_CLOEXEC);
    pid_t c = fork();
    if (c < 0) {
        int e = errno;
        close(p[0]);
        close(p[1]);
        snprintf(why, n, "the probe cannot be forked (%s)", ename(e));
        return 1;
    }
    if (c == 0) {
        close(p[0]);
        pf_child(p[1], host_prog, file_backed);
    }
    close(p[1]);
    struct pf_res r = {0, 0, 0};
    ssize_t got;
    do
        got = read(p[0], &r, sizeof r);
    while (got < 0 && errno == EINTR);
    close(p[0]);
    int st = 0;
    while (waitpid(c, &st, 0) < 0 && errno == EINTR) {
    }
    if (got != (ssize_t)sizeof r) {
        if (WIFSIGNALED(st) && WTERMSIG(st) == SIGSYS)
            return fixed_clash(host_prog, why, n); /* the filter trapped */
        snprintf(why, n, "the probe ended unexpectedly (wait status %#x)", st);
        return 1;
    }
    switch (r.stage) {
    case PF_NNP:
        snprintf(why, n, "no_new_privs cannot be set (%s)", ename(r.err));
        break;
    case PF_EXEC:
        if (file_backed)
            snprintf(why, n, "with -F, %s cannot be mapped executable (%s)",
                     host_prog, ename(r.err2));
        else
            snprintf(why, n, "guest code cannot be made executable: anonymous "
                             "memory may not be (%s), nor can %s be mapped so (%s)",
                     ename(r.err), host_prog, ename(r.err2));
        break;
    case PF_FILTER:
        snprintf(why, n, "the kernel refused the seccomp filter (%s)",
                 ename(r.err));
        break;
    default:
        snprintf(why, n, "the seccomp filter was accepted but not applied "
                         "(an emulator, such as qemu-user, underneath?)");
        break;
    }
    return 1;
}

unsigned long *cng_preflight_auxv(void) {
    static const unsigned long keys[] = {
        AT_PAGESZ, AT_RANDOM, AT_UID,    AT_EUID,         AT_GID, AT_EGID,
        AT_CLKTCK, AT_HWCAP,  26 /* AT_HWCAP2 */, 33 /* AT_SYSINFO_EHDR */,
        51 /* AT_MINSIGSTKSZ */,
    };
    static unsigned long av[2 * (sizeof keys / sizeof *keys) + 2];
    int k = 0;
    for (unsigned i = 0; i < sizeof keys / sizeof *keys; i++) {
        errno = 0;
        unsigned long v = getauxval(keys[i]);
        if (v || errno != ENOENT) {
            av[k++] = keys[i];
            av[k++] = v;
        }
    }
    av[k] = av[k + 1] = 0;
    return av;
}
