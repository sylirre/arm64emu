/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* native_run() -- the native tier's entry (native.h): settle the guest's view
 * of the rootfs, ask whether the tier can run the guest here at all (the
 * preflight), then load it with the userland loader, install the monitor and
 * transfer control. The command line is chroot mode's (chroot/main.c), which
 * hands it over parsed and checked:
 *
 *   rootfs     the guest rootfs (canonical host path); "/" means identity (no
 *              translation)
 *   workdir    -w/--work-dir: the guest's initial cwd, a guest path (NULL for
 *              the default; see set_workdir)
 *   binds      -b mounts: guest path `guest` backed by host path `host`,
 *              read-only when `ro` (the CLI spells this SRC:DST[:ro], host
 *              first)
 *   prog/argv/envp  the guest program (a guest path, resolved through the
 *              rootfs and the binds), its argv and its whole environment, which
 *              chroot mode builds the same way for both of its tiers
 *
 * The monitor traps the guest's own path syscalls and translates them (see
 * monitor.h); it needs a kernel that applies the guest's seccomp filter, which
 * the preflight checks (preflight.c) -- an emulator such as qemu-user accepts
 * one and ignores it.
 */
#include "cng/broker.h"
#include "cng/execmap.h"
#include "cng/l2s.h"
#include "cng/loader.h"
#include "cng/monitor.h"
#include "cng/netlink.h"
#include "cng/ownmap.h"
#include "cng/path.h"
#include "cng/pin.h"
#include "cng/procfs.h"
#include "cng/procreg.h" /* cng_g_shared_proc */
#include "cng/ptrace.h"
#include "cng/rt.h"
#include "cng/syscall.h"
#include "cng/tty.h"
#include "cng/uapi.h"
#include "cng/unixsock.h"
#include "native.h"
#include "cng/preflight.h"

#include <stdio.h>
#include <stdlib.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif

/* The version is the repository's (cng/monitor.h). */
#ifndef PROJECT_VERSION
#error "PROJECT_VERSION is undefined: build with the Makefile, or define it from the VERSION file"
#endif

extern char **environ;

static struct cng_fs g_fs; /* static: the monitor keeps a pointer after we jump */

static const char *load_err(int rc) {
    switch (rc) {
    case CNG_LOAD_EOPEN:
        return "cannot open (missing or unreadable)";
    case CNG_LOAD_EFORMAT:
        return "not an AArch64 ELF64 executable";
    case CNG_LOAD_EIO:
        return "short/failed read";
    case CNG_LOAD_EMAP:
        return "mmap/mprotect failed (execmem denied?)";
    case CNG_LOAD_ETOOBIG:
        return "too many program headers / interp too long";
    case CNG_LOAD_EACCES:
        return "not a regular file, or not executable";
    case CNG_LOAD_ECLOBBER:
        return "fixed load address overlaps arm64emu's own image";
    case CNG_LOAD_EINTERP:
        return "PT_INTERP names a path the file does not hold";
    case CNG_LOAD_EINVAL:
        return "a PT_LOAD the kernel refuses too (file part past its memory "
               "part, or unmappable at this page size)";
    default:
        return "unknown error";
    }
}

/* AArch64 struct stat: the st_mode field, for the -w directory check below. */
#define ST_MODE_OFF 16

/* -w/--work-dir: make the guest path `wd` the initial cwd.
 *
 * `wd` is a guest path resolved through the rootfs and its binds like any other,
 * following symlinks — an absolute one from the guest root, a relative one
 * against the default cwd the caller has just set — so the guest lands exactly
 * where chdir(wd) would have put it, and getcwd() reports the resolved name a
 * real chdir leaves behind rather than the symlink that was typed.
 *
 * A path that does not name a directory is fatal instead of a quiet fallback to
 * "/": the point of the option is to say where the guest starts, and a program
 * started in the wrong directory misbehaves silently — a build in the wrong
 * tree, a relative <program> resolved somewhere else.
 *
 * Returns 0, or -1 with a diagnostic. */
static int set_workdir(struct cng_fs *fs, const char *wd) {
    if (!*wd) {   /* "" would join to the cwd; chdir("") is ENOENT */
        cng_dprintf(2, "arm64emu: --work-dir: empty path\n");
        return -1;
    }
    char host[CNG_PATH_MAX];
    if (cng_resolve(wd, 1, host, sizeof host) != 0) {
        cng_dprintf(2, "arm64emu: --work-dir '%s': cannot resolve\n", wd);
        return -1;
    }
    char st[128];   /* struct stat, aarch64 */
    long r = cng_pin_fstatat(host, st, 0);
    if (r == -ENOENT) {
        cng_dprintf(2, "arm64emu: --work-dir '%s': not found\n", wd);
        return -1;
    }
    if (r != 0) {
        cng_dprintf(2, "arm64emu: --work-dir '%s': cannot stat (errno %d)\n",
                    wd, (int)-r);
        return -1;
    }
    if ((*(unsigned *)(st + ST_MODE_OFF) & CNG_S_IFMT) != CNG_S_IFDIR) {
        cng_dprintf(2, "arm64emu: --work-dir '%s': not a directory\n", wd);
        return -1;
    }
    /* The guest-side name of what we resolved: absolute and symlink-free by
     * construction. A host path outside the guest view has no such name — only
     * possible for a resolution that left the map, e.g. through /proc/self/fd —
     * so fall back to the lexical form there. */
    char guest[CNG_PATH_MAX];
    if (cng_fs_untranslate(fs, host, guest, sizeof guest) != 0 &&
        cng_fs_abscanon(fs, wd, guest, sizeof guest) != 0) {
        cng_dprintf(2, "arm64emu: --work-dir '%s': path too long\n", wd);
        return -1;
    }
    /* The real cwd first, and its failure is the whole answer: a relative path
     * the monitor never sees — anything untrapped, and everything when no
     * monitor installs at all — resolves against this one, so leaving it at the
     * launch directory while telling the guest it is somewhere else resolves
     * those names outside the guest view entirely. It can genuinely fail after
     * the stat above: a directory with no search permission is EACCES, and
     * under --fake-id the guest believes it is root while the kernel does not. */
    long r2 = sys_chdir(host);
    if (r2 != 0) {
        cng_dprintf(2, "arm64emu: --work-dir '%s': cannot enter (errno %d)\n",
                    wd, (int)-r2);
        return -1;
    }
    cng_fs_set_cwd(fs, guest);
    return 0;
}

/* Is the environment entry `e` the switch `name` turned on: NAME=<value> with a
 * value that is neither empty nor "0"? An empty value is off, as it is for
 * cng_broker_env's switches: `NAME= arm64emu ...` is how a shell clears a
 * variable for one command. */
static int env_switch_on(const char *e, const char *name) {
    size_t n = strlen(name);
    return !strncmp(e, name, n) && e[n] == '=' && e[n + 1] != '\0' &&
           e[n + 1] != '0';
}

/* The first program may be a #! script, as an execve's may: its chain is
 * followed as the emulated execve follows one (execve.c, execve_plan) -- each
 * level a regular file with an execute bit, its line parsed as the kernel
 * parses it (cng_shebang), its interpreter resolved through the view -- and
 * what is loaded is the ELF at the end of it, `host` on return. Its argv is
 * the kernel's: each level's interpreter and optional argument, the deepest
 * first, then the program as it was named, then the arguments past argv[0]
 * (*argvp; argv itself for no script). 0, or -errno as an execve answers;
 * a program that is not there or not loadable is the load's to say. */
static char g_sheb_i[CNG_SHEB_MAX][CNG_SHEB_WORD], g_sheb_a[CNG_SHEB_MAX][CNG_SHEB_WORD];
static int g_sheb_h[CNG_SHEB_MAX];

static long first_chain(const char *prog, char *host, size_t hsz, char **argv,
                        char ***argvp) {
    int depth = 0;
    *argvp = argv;
    for (;; depth++) {
        long fd = cng_pin_open(host, CNG_O_RDONLY | CNG_O_CLOEXEC, 0);
        if (fd < 0)
            return depth ? fd : 0;
        char st[128]; /* AArch64 struct stat: mode@16 */
        unsigned mode = 0;
        if (CNG_SYS(__NR_fstat, fd, st, 0, 0, 0, 0) == 0)
            mode = *(unsigned *)(st + 16);
        char hdr[257];
        long n = 0;
        for (;;) {
            long r = sys_pread64((int)fd, hdr + n, (size_t)(256 - n), n);
            if (r == -EINTR)
                continue;
            if (r <= 0 || (n += r) >= 256)
                break;
        }
        sys_close((int)fd);
        char spare_i[CNG_SHEB_WORD], spare_a[CNG_SHEB_WORD];
        int spare_h;
        int sr = depth < CNG_SHEB_MAX
                     ? cng_shebang(hdr, n, g_sheb_i[depth], g_sheb_a[depth],
                                   &g_sheb_h[depth])
                     : cng_shebang(hdr, n, spare_i, spare_a, &spare_h);
        if (sr == 0)
            break; /* an ELF, or what the loader will refuse */
        if ((mode & 0170000) != 0100000 || !(mode & 0111))
            return -EACCES; /* open_exec's own checks, as for the ELF */
        if (sr < 0)
            return sr;
        if (depth == CNG_SHEB_MAX)
            return -ELOOP;
        long dd = cng_dotdot_verdict(CNG_AT_FDCWD, g_sheb_i[depth]);
        if (dd)
            return dd;
        if (cng_resolve(g_sheb_i[depth], 1, host, hsz) != 0)
            return -ENOENT;
    }
    if (!depth)
        return 0;
    int argc = 0;
    while (argv[argc])
        argc++;
    char **v = malloc(sizeof *v * (size_t)(2 * depth + 1 + argc + 1));
    if (!v)
        return -ENOMEM;
    int k = 0;
    for (int d = depth - 1; d >= 0; d--) {
        v[k++] = g_sheb_i[d];
        if (g_sheb_h[d])
            v[k++] = g_sheb_a[d];
    }
    v[k++] = (char *)prog; /* the script, as the command line named it */
    for (int i = 1; i < argc; i++)
        v[k++] = argv[i];
    v[k] = 0;
    *argvp = v;
    return 0;
}

/* The run's knobs, read from the host environment: test aids every one, but
 * ARM64EMU_NATIVE_DEBUG. An empty value is off (env_switch_on). */
static void read_knobs(char **envp) {
    for (char **e = envp; e && *e; e++) {
        /* Without the empty-is-off rule, `ARM64EMU_NATIVE_DEBUG= arm64emu ...`
         * turned verbose logging ON, onto the guest's own stderr, which is a
         * stream package managers capture and log. */
        if (env_switch_on(*e, "ARM64EMU_NATIVE_DEBUG"))
            cng_g_debug = 1;
        /* Every linkat through the -l emulation (a host whose filesystem allows
         * real hardlinks). */
        if (env_switch_on(*e, "ARM64EMU_L2S_FORCE"))
            cng_g_l2s_force = 1;
        /* The synthesized /proc/stat even where the host file is readable
         * (Android denies it; test hosts do not). */
        if (env_switch_on(*e, "ARM64EMU_PROCSTAT_FORCE_SYNTH"))
            cng_g_procstat_synth = 1;
        /* The same for /proc/sys/kernel/overflow{u,g}id. */
        if (env_switch_on(*e, "ARM64EMU_OVERFLOWID_FORCE_SYNTH"))
            cng_g_overflowid_synth = 1;
        /* stat/access of a synthesized /proc name answered as the host does on
         * Android, where SELinux refuses them. */
        if (env_switch_on(*e, "ARM64EMU_PROCSYNTH_FORCE_STAT_DENY"))
            cng_g_proc_deny_stat = 1;
        /* The memfd reopen that makes a synthesized view read-only taken as
         * refused, as Android's policy does. */
        if (env_switch_on(*e, "ARM64EMU_PROCFS_REOPEN_FORCE_DENY"))
            cng_g_procfs_reopen_deny = 1;
        /* The termios2 ioctls refused with EACCES before the host is asked, as
         * Android's SELinux policy does; then TIOCGSID, then TCSETSF. */
        if (env_switch_on(*e, "ARM64EMU_TERMIOS2_FORCE_DENY"))
            cng_g_termios2_deny = 1;
        if (env_switch_on(*e, "ARM64EMU_TIOCGSID_FORCE_DENY"))
            cng_g_tiocgsid_deny = 1;
        if (env_switch_on(*e, "ARM64EMU_TCSETSF_FORCE_DENY"))
            cng_g_tcsetsf_deny = 1;
        /* The uid and gid setters taken as refused by an ambient filter, as
         * Android's refuses them (cred.c). */
        if (env_switch_on(*e, "ARM64EMU_SETID_FORCE_DENY"))
            cng_g_setid_deny = 1;
        /* The same for openat2, which a guest then gets emulated. */
        if (env_switch_on(*e, "ARM64EMU_OPENAT2_FORCE_DENY"))
            cng_g_openat2_deny = 1;
        /* PR_SET_MM_MAP taken as refused, as Android's kernels refuse it, so
         * cmdline/environ come from guest memory. */
        if (env_switch_on(*e, "ARM64EMU_MM_MAP_FORCE_DENY"))
            cng_g_mm_map_deny = 1;
    }
}

int native_run(const struct native_opts *o, char *why, size_t whysz) {
    const char *rootfs = o->rootfs;
    const char *prog_guest = o->prog;

    /* What this process is: the thread pointer and x18 (Android's shadow call
     * stack) its libc gave this, the main thread, which every handler of ours
     * switches to (monitor/hentry.S); the page size; the host's auxiliary
     * vector, which the guest's is made from (loader/stack.c); the guest's
     * exe path (a placeholder for the /proc/self/exe fixups until the program
     * is resolved below, which makes it absolute and symlink-resolved); and
     * the host environment, for the knobs and for the lookups made after the
     * command line is out of reach (procreg.c's shared_dir). The guest's own
     * environment is o->envp. Nothing here changes anything. */
    unsigned long tp, x18;
    __asm__ volatile("mrs %0, tpidr_el0\n\tmov %1, x18" : "=r"(tp), "=r"(x18));
    cng_host_tp = tp;
    cng_thread_main(tp, x18);
    cng_host_auxv = cng_preflight_auxv();
    for (unsigned long *a = cng_host_auxv; a[0]; a += 2)
        if (a[0] == 6 /* AT_PAGESZ */ && a[1])
            cng_page_size = a[1];
    cng_g_exe_guest = prog_guest;
    cng_g_host_envp = environ;
    l2s_host = &cng_l2s_ops;

    /* The tier takes the process over, its signal dispositions, its threads
     * and its address space with it: one with a thread of somebody else's
     * (an app that embeds us and called in from one of several) is not one it
     * can have. Asked first, before anything is set up to be undone. */
    if (cng_preflight_threads(why, whysz))
        return NATIVE_FALLBACK;

    /* The command line's flags, onto the monitor's switches. */
    cng_g_fake_id = o->fake_id;
    cng_g_fake_id_explicit = o->fake_explicit;
    cng_g_fake_uid = o->fake_uid;
    cng_g_fake_gid = o->fake_gid;
    cng_g_setuid_root = o->setuid_root;
    cng_g_setgid_root = o->setgid_root;
    cng_g_l2s = o->link2symlink;
    cng_g_loader_file = o->file_backed;
    cng_g_no_proc = o->no_proc;
    cng_g_no_dev = o->no_dev;
    cng_g_keep_fds = o->keep_fds;
    cng_g_host_keyring = o->host_keyring;
    cng_g_share_abstract = o->share_abstract;
    cng_g_shared_proc = o->shared_proc;
    read_knobs(environ);

    /* Key the System V shm namespace to this invocation (unless --shared-proc
     * widens it to the rootfs). Seeded here, in the root process while we are
     * still single-threaded, and fork-inherited — so one launch's whole process
     * tree shares one namespace and separate launches stay isolated. */
    cng_broker_seed_session();

    /* And what the guest's /dev/shm is, which on a host that has none is a
     * directory of ours under $TMPDIR rather than a name that does not resolve.
     * Needs the host environment on record (just above) and has to be settled
     * before anything asks the path layer, which the workdir and the program
     * lookup below both do. */
    cng_dev_shm_init();

    /* Stamp the build: this tree is copied to test devices by hand, so a trace
     * has to be able to say whether it came from the build you just made. */
    if (cng_g_debug)
        cng_dprintf(2, "[cng] arm64emu %s (built %s %s)\n", PROJECT_VERSION,
                    __DATE__, __TIME__);

    /* Fake identity (--fake-id): establish it from the real invoking ids (which
     * are also the stat-remap source). See cng_cred_setup for the implied-vs-
     * explicit identity default. */
    if (cng_g_fake_id)
        cng_cred_setup((unsigned)sys_getuid(), (unsigned)sys_getgid());

    /* Where the launch left this process, for a fallback to put it back: the
     * view's setup below moves it into the rootfs, and the emulator settles
     * its own cwd from where it was launched. */
    long back = sys_openat(CNG_AT_FDCWD, ".", CNG_O_RDONLY | CNG_O_DIRECTORY |
                           CNG_O_CLOEXEC, 0);

    /* Filesystem view. A prefix that does not fit is refused, not shortened:
     * every guest path is joined to it, so a truncated one would silently root
     * the guest at an ancestor of the tree that was named — or, since the cut
     * lands mid-component, at a path that is not a directory at all. */
    if (cng_fs_init(&g_fs, rootfs) != 0) {
        cng_dprintf(2, "arm64emu: rootfs '%s': path too long (max %d)\n",
                    rootfs, (int)sizeof g_fs.rootfs - 1);
        return 2;
    }
    for (int j = 0; j < o->nbinds; j++)
        if (cng_fs_add_bind(&g_fs, o->binds[j].guest, o->binds[j].host,
                            o->binds[j].ro) != 0) {
            cng_dprintf(2, "arm64emu: --bind '%s:%s': path too long\n",
                        o->binds[j].host, o->binds[j].guest);
            return 2;
        }
    /* The dispatcher needs the fs view, and so does cng_resolve, which
     * -w/--work-dir and the program lookup below both go through. */
    cng_g_fs = &g_fs;

    /* Initial guest cwd. With a real rootfs, default to "/" (never leak the host
     * launch dir) and chdir the real process into the rootfs so untranslated
     * relative access stays contained. With an identity rootfs, the host cwd is
     * the guest cwd.
     *
     * The chdir's failure is the whole answer, as it is for --work-dir: a
     * relative name the monitor never sees — anything untrapped — resolves
     * against the real cwd, and one left at the launch directory while the
     * guest is told it is at "/" resolves those names outside the guest view
     * entirely. It used to be ignored, so a rootfs that was not enterable ran
     * the guest (from a bind, say) with its containment fallback missing and
     * not a word said. */
    if (strcmp(rootfs, "/") != 0) {
        cng_fs_set_cwd(&g_fs, "/");
        char rhost[CNG_PATH_MAX];
        long r = cng_fs_translate(&g_fs, "/", rhost, sizeof rhost) == 0
                     ? sys_chdir(rhost)
                     : -ENAMETOOLONG;
        if (r != 0) {
            cng_dprintf(2, "arm64emu: rootfs '%s': cannot enter (errno %d)\n",
                        rootfs, (int)-r);
            return 1;
        }
    } else {
        char cwd[CNG_PATH_MAX];
        if (sys_getcwd(cwd, sizeof cwd) > 0)
            cng_fs_set_cwd(&g_fs, cwd);
    }
    /* -w/--work-dir overrides that default. It has to land here: <program> is
     * resolved against the cwd just below, so a relative one follows -w, exactly
     * as it would follow the cwd of a shell that ran the same command. */
    if (o->workdir && set_workdir(&g_fs, o->workdir) < 0)
        return 1;

    if (cng_broker_env("ARM64EMU_NETLINK_FORCE_BLOCK"))
        cng_nl_force_block = 1;
    if (cng_broker_env("ARM64EMU_NETLINK_FORCE_NO_RELAY")) {
        cng_nl_force_block = 1;
        cng_nl_no_relay = 1;
    }
    if (cng_broker_env("ARM64EMU_NETLINK_FORCE_DENY_GETLINK")) {
        cng_nl_force_block = 1;
        cng_nl_deny_getlink = 1;
    }
    /* Stands alone: the audit refusal has nothing to do with the rtnetlink
     * emulation, so forcing one must not force the other. */
    if (cng_broker_env("ARM64EMU_NETLINK_FORCE_DENY_AUDIT"))
        cng_nl_deny_audit = 1;

    /* No host here has a true noexec mount to offer, so the one path that only
     * such a mount reaches has a knob of its own: take the anonymous route for
     * every executable file mapping without asking the kernel first. */
    if (cng_broker_env("ARM64EMU_MMAP_FORCE_ANON"))
        cng_g_execmap_force = 1;

    /* Resolve the program itself through the map (following symlinks) to find
     * the host file. */
    char host_prog[CNG_PATH_MAX];
    if (cng_resolve(prog_guest, 1, host_prog, sizeof host_prog) != 0) {
        cng_dprintf(2, "arm64emu: cannot resolve %s\n", prog_guest);
        return 1;
    }
    /* ...and a #! chain to the ELF it ends in (`host_prog` from here). */
    char **gargv;
    long cr = first_chain(prog_guest, host_prog, sizeof host_prog, o->argv, &gargv);
    if (cr < 0) {
        cng_dprintf(2, "arm64emu: cannot execute %s (errno %d)\n", prog_guest,
                    (int)-cr);
        return 126;
    }

    /* The preflight's own questions, the ones a forked child answers and the
     * program's file: the last thing before the point of no return. A no puts
     * the process back where it was launched, for the emulator. */
    if (cng_preflight(host_prog, cng_g_loader_file, why, whysz)) {
        if (back >= 0) {
            CNG_SYS(__NR_fchdir, back, 0, 0, 0, 0, 0);
            sys_close((int)back);
        }
        return NATIVE_FALLBACK;
    }
    if (back >= 0)
        sys_close((int)back);

    /* The point of no return: from here the process is the guest's, and a
     * failure is the guest's failure to start, as it is for an exec. What the
     * libc's startup left that an exec would not is undone first (execve.c):
     * the dispositions, an alternate stack, an rseq area -- after whatever
     * stdio holds is written out, as an exec would have had it written. */
    fflush(0);
    /* glibc's malloc gives every thread that allocates an arena of its own,
     * mmap'd -- memory the exec sweep, which keeps the heap and what we
     * record, would take for the guest's. Our threads (monitor/thread.c)
     * allocate through pthread_create, so the arena is the main one, the brk
     * heap, for all. */
#ifdef __GLIBC__
    mallopt(M_ARENA_MAX, 1);
#endif
    cng_entry_reset();

    /* /proc/self/exe (and the comm derived from it) for the initial program. It
     * was <program> verbatim, while a real kernel reports the absolute,
     * symlink-resolved path of the image it actually loaded — and glibc does not
     * merely tolerate that, it asserts the leading '/' in _dl_get_origin, so a
     * relative <program> aborted the guest before main. The host path we just
     * resolved, untranslated back into the guest view, is absolute and
     * symlink-resolved by construction, and it is the same derivation the
     * emulated execve uses for every program after this one (execve.c) — so
     * the first program in a session stops answering differently from its own
     * children. */
    static char exe_guest[CNG_PATH_MAX];
    if (cng_fs_untranslate(&g_fs, host_prog, exe_guest, sizeof exe_guest) == 0 ||
        cng_fs_abscanon(&g_fs, prog_guest, exe_guest, sizeof exe_guest) == 0)
        cng_g_exe_guest = exe_guest;

    /* setuid/setgid-on-exec for the initial program (e.g. running /bin/su
     * directly), mirroring the emulated-execve path. */
    cng_cred_exec(host_prog);

    /* The view is complete: publish it. From here every reader sees it through
     * the sequence protocol and every chdir/chroot replaces it whole rather
     * than editing it under them (path.c). g_fs itself is not written again,
     * which is what lets the l2s emulation keep it as the view the run was
     * started with: a guest chroot narrows the published one, and a hardlink
     * group made before it still has its data where this one put it. */
    cng_g_fs = cng_fs_publish(&g_fs);
    cng_l2s_home(&g_fs);

    /* What the launcher handed down, judged against that view: a descriptor on
     * a directory the guest has no name for is closed here rather than left
     * for the guest to resolve names from — the one way to reach outside the
     * view that no path translation can see (dispatch.c, cng_fd_admit). */
    cng_fds_sanitize();

    /* The floor: every mapping that exists at this moment is ours or the
     * kernel's, because nothing of the guest has been mapped yet. An emulated
     * execve gives back what is NOT in it (the sweep in execve.c), so where
     * this stands is the whole of its accuracy — and it has to stand HERE,
     * in front of the first program, not merely in front of the first guest
     * instruction. Taken after the load, the floor would claim that program's
     * image and its 64 MiB stack, and once those are given back the kernel
     * hands the same addresses to the next generation's allocator, which the
     * sweep would then read as ours and keep. Measured that way: the reclaim
     * held on to 16 MB per generation and could not say why.
     *
     * Everything the monitor maps after this point records itself
     * (cng_own_map); everything the loader maps is a generation range. */
    cng_own_floor();

    struct cng_loaded prog;
    int rc = cng_load_elf(host_prog, 0, &prog);
    if (rc != CNG_LOAD_OK) {
        cng_dprintf(2, "arm64emu: cannot load %s (%s): %s\n", prog_guest,
                    host_prog, load_err(rc));
        return 1;
    }

    unsigned long sp, entry;
    struct cng_loaded interp;
    int gargc = 0;
    while (gargv[gargc])
        gargc++;
    if (prog.has_interp) {
        char ipath[CNG_PATH_MAX];
        const char *ip;
        if (cng_resolve(prog.interp, 1, ipath, sizeof ipath) == 0)
            ip = ipath;
        else
            ip = prog.interp;

        int rc2 = cng_load_elf(ip, 0, &interp);
        if (rc2 != CNG_LOAD_OK) {
            cng_dprintf(2, "arm64emu: cannot load interpreter %s: %s\n", ip,
                        load_err(rc2));
            return 1;
        }
        sp = cng_build_stack(gargc, gargv, o->envp, cng_host_auxv, &prog,
                             &interp, prog_guest);
        entry = interp.entry;
    } else {
        sp = cng_build_stack(gargc, gargv, o->envp, cng_host_auxv, &prog, 0,
                             prog_guest);
        entry = prog.entry;
    }
    if (!sp) {
        cng_dprintf(2, "arm64emu: argument list too long\n");
        return 1;
    }
    /* The first generation. Nothing is retired here — there is no program being
     * replaced — but an exec has to have something to retire when it comes. */
    cng_exec_generation(&prog, prog.has_interp ? &interp : 0, cng_g_stack_lo,
                        cng_g_stack_len);
    cng_brk_place(&prog);

    /* /proc emulation: bring up the PID registry and reserve the synthesized fd
     * range (both must be settled before the seccomp filter is built, which
     * bakes the range in), then publish this process's guest identity from the
     * stack we just built. */
    if (!cng_g_no_proc) {
        cng_procfs_init();
        cng_procfs_publish_stack(sp);
    }

    /* The ptrace link registry: one MAP_SHARED region that every guest process
     * must inherit, so it has to exist before the guest can fork — and before
     * the filter, whose install is the last thing that happens here. */
    cng_pt_init();

    /* Install the monitor last, after all of our own path syscalls are done.
     * Only when it has something to do: an identity view needs no translation.
     *
     * One option needs a trap of ours whatever the view is, and was inert
     * without this: --shared-proc has to publish this invocation's processes into
     * the shared registry as they fork, and that hook hangs off the trapped
     * clone; without it a --shared-proc invocation over `/` published its root
     * process and nothing else, so another invocation's `ps` saw one process
     * where there were dozens.
     *
     * --no-proc, --no-dev and --share-abstract-sockets are the opposite case:
     * each turns an emulation of ours off, and with no monitor installed there
     * is none to turn off, so they are already honored by not being here. */
    int want_xlate = (strcmp(rootfs, "/") != 0) || o->nbinds > 0 ||
                     cng_g_fake_id || cng_g_l2s || cng_g_shared_proc;
    if (want_xlate) {
        int mrc = cng_install_monitor(cng_g_fs);
        /* The preflight's child installed a filter, so this one failing is a
         * policy that tells the two apart, or a resource running out -- and
         * not a warning to print on the way into the guest. Without a monitor
         * nothing intercepts: a rootfs that is not `/` runs against the host's
         * own paths, -u reports the real identity, -l makes no hardlink and
         * --shared-proc publishes nothing -- every one of them the thing the
         * invocation asked for, and none of them delivered. It stops here
         * instead, with the errno. */
        if (mrc < 0) {
            cng_dprintf(2,
                        "arm64emu: --native: cannot install the seccomp monitor"
                        " (errno %d) and nothing else would intercept the"
                        " guest's syscalls: not entering it\n",
                        -mrc);
            return 1;
        }

        /* A host with no /dev/shm whose temp directories all failed the
         * stand-in's checks (cng_dev_shm_init) leaves the guest without one,
         * which it can run without — but its shm_open and sem_open then fail
         * ENOENT, nowhere near the cause, which is the launcher's: an Android
         * app that hands over its 0771 files directory as $TMPDIR, say. So
         * this says which candidates were passed over and why. Only here,
         * with the monitor in: without it the guest has the host's /dev,
         * which is what it would have had anyway. */
        const char *shm_why = cng_dev_shm_missing();
        if (shm_why)
            cng_dprintf(2,
                        "arm64emu: warning: no temp directory can stand in for"
                        " the /dev/shm this host lacks, so the guest has none"
                        " (shm_open and sem_open fail)\n"
                        "          %s%s\n"
                        "          (point $TMPDIR at a directory nobody else"
                        " can write: mode 0700, or sticky like 1777)\n",
                        *shm_why ? "passed over: " : "no candidate is set",
                        shm_why);
    }

    cng_enter(sp, entry);
}
