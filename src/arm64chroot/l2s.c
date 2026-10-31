/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* arm64chroot's side of link2symlink (lib/link2symlink, link2symlink.h): the
 * host the engine runs on. The engine works on host path strings; every one
 * it hands back here is pinned as a guest syscall's own path is (path.c,
 * PathPin) -- mapped back to the guest name it has (the bind it lies under,
 * or the rootfs), its directory walked descriptor by descriptor, and the call
 * made on the last component without following it. A path that lies in no
 * place of the guest's is refused (EACCES), except the host /proc zone, where
 * a descriptor's magic link is the one path meant to be followed.
 *
 * Compiled into every build: the engine is called only where the scheme is
 * (L2S_ENABLED, and --link2symlink given), and the table costs the others
 * nothing. The descriptors the engine holds across calls (a group's lock, a
 * directory being scanned) are the emulator's own, registered against fork
 * like a pin's (machine.h, fdheld). */
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "machine.h"
#include "link2symlink.h"

/* A host path in the host's /proc: a descriptor's magic link. */
static int hp_proc(const char *host) {
    return !strncmp(host, "/proc", 5) && (host[5] == '/' || !host[5]);
}

/* The namespace-absolute guest name of `host`: under a bind (its guest side),
 * or under the rootfs. 0, or -EACCES for a host path in neither. */
static int hp_canon(struct Machine *m, const char *host, char *canon) {
    if (bind_of_host(m, host, canon) >= 0) return 0;
    size_t rl = strlen(m->rootfs);
    if (strncmp(host, m->rootfs, rl) || (host[rl] && host[rl] != '/'))
        return -EACCES;
    if (strlen(host + rl) + 2 > PATH_MAX) return -ENAMETOOLONG;
    strcpy(canon, host[rl] ? host + rl : "/");
    return 0;
}

/* Pin the host path `host` as a guest syscall's own (see the top). 0 or
 * -errno; path_unpin either way. */
int l2s_host_pin(const char *host, PathPin *p) {
    struct Machine *m = &g_machine;
    char canon[PATH_MAX];
    if (hp_proc(host)) {
        p->dfd = AT_FDCWD;
        p->pinned = 0;
        p->lowfd = -1;
        p->base[0] = 0;
        if (strlen(host) + 1 > sizeof p->host) return -ENAMETOOLONG;
        strcpy(p->host, host);
        p->name = p->host;
        return 0;
    }
    int r = hp_canon(m, host, canon);
    if (r < 0) {
        path_pin(m, "/", "/", p);   /* a pin that unpins as a no-op */
        return r;
    }
    return path_pin(m, canon, host, p);
}

/* The flag that keeps the host from following the last component: always,
 * unless the name is the unpinned /proc zone, where the caller says. */
static int hp_nofollow(const PathPin *p, int follow) {
    return p->pinned || !follow ? AT_SYMLINK_NOFOLLOW : 0;
}

static void to_l2s(const struct stat *st, struct l2s_stat *out) {
    out->mode = st->st_mode;
    out->ino = st->st_ino;
    out->atime_sec = st->st_atim.tv_sec;
    out->atime_nsec = st->st_atim.tv_nsec;
    out->mtime_sec = st->st_mtim.tv_sec;
    out->mtime_nsec = st->st_mtim.tv_nsec;
}

/* stat(2) of a host path the engine named: 0 or -errno. */
int l2s_host_stat(const char *host, struct stat *st, int follow) {
    PathPin p;
    int r = l2s_host_pin(host, &p);
    if (r == 0 && fstatat(p.dfd, p.name, st, hp_nofollow(&p, follow)) < 0) r = -errno;
    path_unpin(&p);
    return r;
}

static long h_fstatat(const char *host, struct l2s_stat *out, int follow) {
    struct stat st;
    int r = l2s_host_stat(host, &st, follow);
    if (r == 0) to_l2s(&st, out);
    return r;
}
static long h_lstat(const char *host, struct l2s_stat *st) { return h_fstatat(host, st, 0); }
static long h_stat(const char *host, struct l2s_stat *st) { return h_fstatat(host, st, 1); }

static long h_readlink(const char *host, char *buf, size_t n) {
    PathPin p;
    long r = l2s_host_pin(host, &p);
    if (r == 0) {
        ssize_t k = readlinkat(p.dfd, p.name, buf, n);
        r = k < 0 ? -errno : (long)k;
    }
    path_unpin(&p);
    return r;
}

static long h_symlink(const char *target, const char *host) {
    PathPin p;
    long r = l2s_host_pin(host, &p);
    if (r == 0 && symlinkat(target, p.dfd, p.name) < 0) r = -errno;
    path_unpin(&p);
    return r;
}

static long h_rename(const char *from, const char *to, unsigned flags) {
    PathPin p1, p2;
    long r = l2s_host_pin(from, &p1);
    if (r < 0) { path_unpin(&p1); return r; }
    r = l2s_host_pin(to, &p2);
    if (r == 0) {
#ifdef SYS_renameat2
        long k = flags ? syscall(SYS_renameat2, p1.dfd, p1.name, p2.dfd, p2.name, flags)
                       : renameat(p1.dfd, p1.name, p2.dfd, p2.name);
        r = k < 0 ? -errno : 0;
#else
        r = flags ? -ENOSYS
                  : renameat(p1.dfd, p1.name, p2.dfd, p2.name) < 0 ? -errno : 0;
#endif
    }
    path_unpin(&p1); path_unpin(&p2);
    return r;
}

static long h_unlink(const char *host) {
    PathPin p;
    long r = l2s_host_pin(host, &p);
    if (r == 0 && unlinkat(p.dfd, p.name, 0) < 0) r = -errno;
    path_unpin(&p);
    return r;
}

static long h_mkdir(const char *host, unsigned mode) {
    PathPin p;
    long r = l2s_host_pin(host, &p);
    if (r == 0 && mkdirat(p.dfd, p.name, (mode_t)mode) < 0) r = -errno;
    path_unpin(&p);
    return r;
}

/* A descriptor of the emulator's own, registered against fork (fdheld): it
 * outlives the window it was opened in. */
static long hp_open(const PathPin *p, int flags, mode_t mode) {
    fdwin_enter();
    int fd = openat(p->dfd, p->name, flags | O_CLOEXEC, mode);
    long r = fd < 0 ? -errno : fdheld_add(fd);
    fdwin_leave();
    return r;
}

static long h_open(const char *host, enum l2s_open how, unsigned mode) {
    static const int fl[] = {
        [L2S_OPEN_DIR] = O_RDONLY | O_DIRECTORY,
        [L2S_OPEN_READ] = O_RDONLY,
        [L2S_OPEN_WRITE] = O_WRONLY,
        [L2S_OPEN_SOURCE] = O_RDONLY | O_NONBLOCK | O_NOCTTY,
        [L2S_OPEN_TOUCH] = O_WRONLY | O_CREAT,
        [L2S_OPEN_NEW] = O_WRONLY | O_CREAT | O_EXCL,
    };
    PathPin p;
    long r = l2s_host_pin(host, &p);
    /* Pinned, nothing at the last component is to be followed: a symlink
     * standing there now is the race (machine.h, PathPin). */
    if (r == 0) r = hp_open(&p, fl[how] | (p.pinned ? O_NOFOLLOW : 0), (mode_t)mode);
    path_unpin(&p);
    return r;
}

/* linux_dirent64: d_ino(8) d_off(8) d_reclen(2 @16) d_type(1 @18) name(@19).
 * The names go to `each` with no window open: it calls back into the engine,
 * which pins paths of its own. */
static long h_scan(const char *dir, int (*each)(void *arg, const char *name), void *arg) {
    PathPin p;
    long fd = l2s_host_pin(dir, &p);
    if (fd == 0) fd = hp_open(&p, O_RDONLY | O_DIRECTORY | (p.pinned ? O_NOFOLLOW : 0), 0);
    path_unpin(&p);
    if (fd < 0) return fd;
    char buf[4096];
    int stop = 0;
    while (!stop) {
        long n = syscall(SYS_getdents64, (int)fd, buf, sizeof buf);
        if (n <= 0) break;
        for (long o = 0; o + 19 <= n && !stop;) {
            unsigned short reclen;
            memcpy(&reclen, buf + o + 16, 2);
            if (reclen == 0 || o + reclen > n) break;
            stop = each(arg, buf + o + 19);
            o += reclen;
        }
    }
    fdheld_close((int)fd);
    return 0;
}

static long h_utimes(const char *host, const struct l2s_stat *t) {
    PathPin p;
    struct timespec ts[2] = {{(time_t)t->atime_sec, t->atime_nsec},
                             {(time_t)t->mtime_sec, t->mtime_nsec}};
    long r = l2s_host_pin(host, &p);
    if (r == 0 && utimensat(p.dfd, p.name, ts, AT_SYMLINK_NOFOLLOW) < 0) r = -errno;
    path_unpin(&p);
    return r;
}

static long h_fstat(long fd, struct l2s_stat *out) {
    struct stat st;
    if (fstat((int)fd, &st) < 0) return -errno;
    to_l2s(&st, out);
    return 0;
}
static long h_read(long fd, void *buf, size_t n) {
    ssize_t k = read((int)fd, buf, n);
    return k < 0 ? -errno : (long)k;
}
static long h_write(long fd, const void *buf, size_t n) {
    ssize_t k = write((int)fd, buf, n);
    return k < 0 ? -errno : (long)k;
}
static long h_fchmod(long fd, unsigned mode) {
    return fchmod((int)fd, (mode_t)mode) < 0 ? -errno : 0;
}
static long h_flock(long fd, int op) {
    return flock((int)fd, op) < 0 ? -errno : 0;
}
static void h_close(long fd) { fdheld_close((int)fd); }

/* Nothing to hold off: a guest's signal handler runs at an instruction
 * boundary of its own thread, never inside the emulation of a syscall, so it
 * cannot ask for the lock its own thread is holding. */
static void h_hold(unsigned long long *saved) { *saved = 0; }
static void h_release(const unsigned long long *saved) { (void)saved; }
static void h_yield(void) { sched_yield(); }

static size_t h_rootfs(char *out, size_t sz) {
    size_t n = strlen(g_machine.rootfs);   /* "" for a rootfs of "/" */
    if (sz) {
        size_t c = n < sz - 1 ? n : sz - 1;
        memcpy(out, g_machine.rootfs, c);
        out[c] = 0;
    }
    return n;
}

static int h_owned(const char *host) {
    char canon[PATH_MAX];
    return hp_canon(&g_machine, host, canon) == 0;
}

/* The guest's own name for `host`: its namespace-absolute name, seen from
 * inside the guest's chroot, if it has one there. */
static int h_guest(const char *host, char *out, size_t sz) {
    struct Machine *m = &g_machine;
    char canon[PATH_MAX], croot[PATH_MAX];
    if (hp_canon(m, host, canon) < 0) return -1;
    croot_get(m, croot);
    const char *g = canon;
    if (strcmp(croot, "/")) {
        size_t cl = strlen(croot);
        if (strncmp(canon, croot, cl) || (canon[cl] && canon[cl] != '/')) return -1;
        g = canon[cl] ? canon + cl : "/";
    }
    if (strlen(g) + 1 > sz) return -1;
    strcpy(out, g);
    return 0;
}

static int h_resolve(const char *gpath, char *out, size_t sz) {
    char host[PATH_MAX];
    int r = path_resolve(&g_machine, G_AT_FDCWD, gpath, PATH_NOFOLLOW_LAST, host, NULL);
    if (r < 0) return r;
    if (strlen(host) + 1 > sz) return -ENAMETOOLONG;
    strcpy(out, host);
    return 0;
}

/* Failure-path diagnostics, with -strace. */
static void h_log(const char *fmt, ...) {
    if (!g_machine.strace) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

const struct l2s_ops l2s_host = {
    .lstat = h_lstat,
    .stat = h_stat,
    .readlink = h_readlink,
    .symlink = h_symlink,
    .rename = h_rename,
    .unlink = h_unlink,
    .mkdir = h_mkdir,
    .open = h_open,
    .scan = h_scan,
    .utimes = h_utimes,
    .fstat = h_fstat,
    .read = h_read,
    .write = h_write,
    .fchmod = h_fchmod,
    .flock = h_flock,
    .close = h_close,
    .hold = h_hold,
    .release = h_release,
    .yield = h_yield,
    .rootfs = h_rootfs,
    .owned = h_owned,
    .guest = h_guest,
    .resolve = h_resolve,
    .log = h_log,
};
