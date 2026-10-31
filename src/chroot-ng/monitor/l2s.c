/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* chroot-ng's side of link2symlink (lib/link2symlink, cng/l2s.h): the host the
 * engine runs on — every path call pinned (cng/pin.h), raw syscalls for the
 * rest — and what the engine leaves to the program: the stat and statx a name
 * of a group answers, in the AArch64 layouts, a listing's record for one, and
 * the guest paths that name the emulation's own files. */
#include "cng/l2s.h"
#include "cng/monitor.h"
#include "cng/path.h"
#include "cng/pin.h"
#include "cng/rt.h"
#include "cng/syscall.h"
#include "cng/uapi.h"

#include <asm-generic/errno.h>

int cng_g_l2s = 0;
int cng_g_l2s_force = 0;

/* aarch64 struct stat field offsets (see dispatch.c). */
#define ST_INO_OFF   8
#define ST_MODE_OFF  16
#define ST_NLINK_OFF 20
#define ST_ATIM_OFF  72
#define ST_MTIM_OFF  88
#define ST_SIZE      128
/* struct statx field offsets. */
#define STX_MASK_OFF  0
#define STX_NLINK_OFF 16

/* ---- the host (struct l2s_ops) -------------------------------------------- */

static void from_stat(const char *st, struct l2s_stat *out) {
    long t[4];
    memcpy(&out->mode, st + ST_MODE_OFF, sizeof out->mode);
    memcpy(&out->ino, st + ST_INO_OFF, sizeof out->ino);
    memcpy(t, st + ST_ATIM_OFF, sizeof t / 2);
    memcpy(t + 2, st + ST_MTIM_OFF, sizeof t / 2);
    out->atime_sec = t[0];
    out->atime_nsec = t[1];
    out->mtime_sec = t[2];
    out->mtime_nsec = t[3];
}

static long h_fstatat(const char *p, struct l2s_stat *out, int atflags) {
    char st[ST_SIZE];
    long r = cng_pin_fstatat(p, st, atflags);
    if (r == 0)
        from_stat(st, out);
    return r;
}
static long h_lstat(const char *p, struct l2s_stat *st) {
    return h_fstatat(p, st, CNG_AT_SYMLINK_NOFOLLOW);
}
static long h_stat(const char *p, struct l2s_stat *st) {
    return h_fstatat(p, st, 0);
}
static long h_readlink(const char *p, char *b, size_t n) {
    return cng_pin_readlink(p, b, n);
}
static long h_symlink(const char *target, const char *p) {
    return cng_pin_symlink(target, p);
}
/* Never issued with flags where the ambient filter refuses renameat2: in the
 * SIGSYS handler that refusal is a nested SIGSYS, and the process dies of it. */
static long h_rename(const char *from, const char *to, unsigned flags) {
    if (flags && cng_blocked[__NR_renameat2])
        return -ENOSYS;
    return cng_pin_rename(from, to, flags);
}
static long h_unlink(const char *p) { return cng_pin_unlink(p, 0); }
static long h_mkdir(const char *p, unsigned mode) {
    return cng_pin_mkdir(p, (int)mode);
}
static long h_open(const char *p, enum l2s_open how, unsigned mode) {
    static const long fl[] = {
        [L2S_OPEN_DIR] = CNG_O_RDONLY | CNG_O_DIRECTORY,
        [L2S_OPEN_READ] = CNG_O_RDONLY,
        [L2S_OPEN_WRITE] = CNG_O_WRONLY,
        [L2S_OPEN_SOURCE] = CNG_O_RDONLY | CNG_O_NONBLOCK | CNG_O_NOCTTY,
        [L2S_OPEN_TOUCH] = CNG_O_WRONLY | CNG_O_CREAT,
        [L2S_OPEN_NEW] = CNG_O_WRONLY | CNG_O_CREAT | CNG_O_EXCL,
    };
    return cng_pin_open(p, fl[how] | CNG_O_CLOEXEC, (long)mode);
}

/* linux_dirent64: d_ino(8) d_off(8) d_reclen(2 @16) d_type(1 @18) name(@19). */
static long h_scan(const char *dir, int (*each)(void *arg, const char *name),
                   void *arg) {
    long fd = cng_pin_open(dir, CNG_O_RDONLY | CNG_O_DIRECTORY | CNG_O_CLOEXEC,
                           0);
    if (fd < 0)
        return fd;
    char buf[4096];
    int stop = 0;
    while (!stop) {
        long n = CNG_SYS(__NR_getdents64, (int)fd, buf, sizeof buf, 0, 0, 0);
        if (n <= 0)
            break;
        for (long o = 0; o + 19 <= n && !stop;) {
            unsigned short reclen;
            memcpy(&reclen, buf + o + 16, 2);
            if (reclen == 0 || o + reclen > n)
                break;
            stop = each(arg, buf + o + 19);
            o += reclen;
        }
    }
    sys_close((int)fd);
    return 0;
}

static long h_utimes(const char *p, const struct l2s_stat *t) {
    if (cng_blocked[__NR_utimensat])
        return -ENOSYS;
    struct cng_pin pn;
    long ts[4] = {(long)t->atime_sec, t->atime_nsec, (long)t->mtime_sec,
                  t->mtime_nsec};
    long r = cng_pin_at(CNG_AT_FDCWD, p, &pn);
    if (r == 0)
        r = CNG_SYS(__NR_utimensat, pn.dfd, (long)pn.name, (long)ts,
                    CNG_AT_SYMLINK_NOFOLLOW, 0, 0);
    cng_unpin(&pn);
    return r;
}

static long h_fstat(long fd, struct l2s_stat *out) {
    char st[ST_SIZE];
    long r = CNG_SYS(__NR_fstat, (int)fd, st, 0, 0, 0, 0);
    if (r == 0)
        from_stat(st, out);
    return r;
}
static long h_read(long fd, void *b, size_t n) {
    return sys_read((int)fd, b, n);
}
static long h_write(long fd, const void *b, size_t n) {
    return sys_write((int)fd, b, n);
}
static long h_fchmod(long fd, unsigned mode) {
    return CNG_SYS(__NR_fchmod, (int)fd, mode, 0, 0, 0, 0);
}
static long h_flock(long fd, int op) {
    return CNG_SYS(__NR_flock, (int)fd, op, 0, 0, 0, 0);
}
static void h_close(long fd) { sys_close((int)fd); }

/* Every signal blocked for an update, as the view's and the credentials'
 * writers block theirs (path.c, cred.c): see l2s_lock. */
static void h_hold(unsigned long long *saved) {
    unsigned long all = ~0UL;
    CNG_SYS(__NR_rt_sigprocmask, 2 /*SIG_SETMASK*/, &all, saved, 8, 0, 0);
}
static void h_release(const unsigned long long *saved) {
    CNG_SYS(__NR_rt_sigprocmask, 2 /*SIG_SETMASK*/, saved, 0, 8, 0, 0);
}
static void h_yield(void) {
    if (!cng_blocked[__NR_sched_yield])
        CNG_SYS(__NR_sched_yield, 0, 0, 0, 0, 0, 0);
}

static size_t h_rootfs(char *out, size_t sz) {
    return cng_g_fs ? cng_fs_rootfs(out, sz) : cng_strlcpy(out, "", sz);
}

/* The view chroot-ng was started with: run.c's, never written after it is
 * published. A guest chroot narrows the view, and a group linked before it
 * keeps its data where the wider view put it. 0 in the unit harness. */
static const struct cng_fs *g_home;

void cng_l2s_home(const struct cng_fs *fs) { g_home = fs; }

/* A place of the guest's own: somewhere the view names (the rootfs, a bind,
 * the /dev/shm stand-in — cng_host_dir_guest), or somewhere the view it was
 * started with named. */
static int h_owned(const char *p) {
    if (!cng_g_fs)
        return 1; /* no view at all: the host root is the rootfs */
    char g[CNG_PATH_MAX];
    if (cng_host_dir_guest(p, g, sizeof g) == 0)
        return 1;
    return g_home && cng_fs_untranslate(g_home, p, g, sizeof g) == 0;
}
static int h_guest(const char *p, char *out, size_t sz) {
    if (!cng_g_fs || cng_host_dir_guest(p, out, sz) != 0)
        return -1;
    return 0;
}
static int h_resolve(const char *g, char *out, size_t sz) {
    return cng_resolve_at(CNG_AT_FDCWD, g, 0, out, sz) == 0 ? 0 : -ENOENT;
}

/* Failure-path diagnostics (CHROOTNG_DEBUG=1). */
static void h_log(const char *fmt, ...) {
    if (!cng_g_debug)
        return;
    va_list ap;
    va_start(ap, fmt);
    cng_dprintf(2, "[cng] ");
    cng_vdprintf(2, fmt, ap);
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

/* ---- what a name of a group answers ---------------------------------------- */

int cng_l2s_stat(const char *host, void *statbuf) {
    char data[CNG_PATH_MAX];
    unsigned long count = 0;
    int r = l2s_target(host, data, sizeof data, &count);
    if (r != 1)
        return r;
    long s = cng_pin_fstatat(data, statbuf, 0);
    if (s < 0)
        return (int)s;
    *(unsigned *)((char *)statbuf + ST_NLINK_OFF) = count ? count : 1;
    return 1;
}

int cng_l2s_statx(const char *host, void *statxbuf, unsigned mask,
                  unsigned flags) {
    char data[CNG_PATH_MAX];
    unsigned long count = 0;
    int r = l2s_target(host, data, sizeof data, &count);
    if (r != 1)
        return r;
    /* The data path is never a symlink: force a follow so the guest's
     * NOFOLLOW cannot expose the emulation. Sync flags pass through. */
    flags &= ~(unsigned)(CNG_AT_SYMLINK_NOFOLLOW | CNG_AT_EMPTY_PATH);
    long s = cng_pin_statx(data, (int)flags, mask, statxbuf);
    if (s < 0)
        return (int)s;
    *(unsigned *)((char *)statxbuf + STX_NLINK_OFF) = count ? count : 1;
    *(unsigned *)((char *)statxbuf + STX_MASK_OFF) |= CNG_STATX_NLINK;
    return 1;
}

/* "/proc/self/fd/<fd>" into `link` (>= 32). */
static void fd_link(long fd, char *link, size_t sz) {
    char *p = link, *end = link + sz - 1;
    p += cng_strlcpy(p, "/proc/self/fd/", (size_t)(end - p) + 1);
    /* int arg: the x-register's top half may be dirty (glibc). */
    unsigned v = (unsigned)(int)fd;
    char tmp[12];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n > 0 && p < end)
        *p++ = tmp[--n];
    *p = '\0';
}

/* If /proc/self/fd/<fd> names a data file, yield the group's live count.
 * Returns 1 (*count filled, floored to 1) or 0. */
static int l2s_fd_count(long fd, unsigned long *count) {
    char link[64], path[CNG_PATH_MAX];
    fd_link(fd, link, sizeof link);
    long n = cng_pin_readlink(link, path, sizeof path - 1);
    if (n < 0)
        return 0;
    path[n] = '\0';
    return l2s_path_count(path, count);
}

/* The host path of the directory behind an open fd, for the one place a
 * listing has to fall back to the path-based machinery. */
static int l2s_fd_dir(long fd, char *out, size_t sz) {
    char link[64];
    fd_link(fd, link, sizeof link);
    long n = cng_pin_readlink(link, out, sz - 1);
    if (n <= 0)
        return -1;
    out[n] = '\0';
    return 0;
}

int cng_l2s_dirent(long dirfd, const char *name, unsigned long long *ino,
                   unsigned *type) {
    /* One readlink tells an ordinary symlink from ours (l2s_link_text). A
     * name that is not a symlink at all answers EINVAL here, which is what
     * makes this cheap enough to ask about a DT_UNKNOWN record too. */
    char tgt[CNG_PATH_MAX];
    long n = sys_readlinkat((int)dirfd, name, tgt, sizeof tgt - 1);
    if (n <= 0)
        return 0;
    tgt[n] = '\0';
    if (!l2s_link_text(tgt))
        return 0;
    /* What stat(2) of the name answers is the data file — a follow lands on
     * it — so the record carries that inode and type. The target is asked
     * about directly rather than followed by the kernel: a data file is never
     * a symlink, and the link is the guest's to have pointed anywhere — which
     * is why an absolute one is asked about only where the engine would take
     * it as it stands, in a place of the guest's own. */
    char st[ST_SIZE];
    long sr = -1;
    unsigned mode = 0;
    if (tgt[0] != '/')
        sr = CNG_SYS(__NR_newfstatat, (int)dirfd, tgt, st,
                     CNG_AT_SYMLINK_NOFOLLOW, 0, 0);
    else if (l2s_in_place(tgt))
        sr = cng_pin_fstatat(tgt, st, CNG_AT_SYMLINK_NOFOLLOW);
    if (sr == 0)
        memcpy(&mode, st + ST_MODE_OFF, sizeof mode);
    if (sr < 0 || (mode & 0170000) == 0040000 || (mode & 0170000) == 0120000) {
        if (tgt[0] != '/')
            return 0; /* a bare name with no data beside it: not ours */
        /* Not a data file where it says: an absolute target whose rootfs tree
         * was moved or copied, which cng_l2s_stat self-heals onto the current
         * store, a member of proot's (the target is its indirection), or no
         * link of ours at all — and a listing must say what the stat after it
         * will. */
        char host[CNG_PATH_MAX];
        size_t dl;
        if (l2s_fd_dir(dirfd, host, sizeof host) < 0 ||
            (dl = strlen(host)) + 1 + strlen(name) >= sizeof host)
            return 0;
        if (dl && host[dl - 1] != '/')
            host[dl++] = '/';
        cng_strlcpy(host + dl, name, sizeof host - dl);
        if (cng_l2s_stat(host, st) != 1)
            return 0; /* still dangling: what the kernel said stands */
        memcpy(&mode, st + ST_MODE_OFF, sizeof mode);
    }
    memcpy(ino, st + ST_INO_OFF, sizeof *ino);
    *type = (mode & 0170000) >> 12; /* DT_* is S_IFMT >> 12 */
    return 1;
}

void cng_l2s_fix_fd(long fd, void *statbuf) {
    unsigned long c;
    if (l2s_fd_count(fd, &c))
        *(unsigned *)((char *)statbuf + ST_NLINK_OFF) = (unsigned)c;
}

void cng_l2s_fix_fd_statx(long fd, void *statxbuf) {
    unsigned long c;
    if (l2s_fd_count(fd, &c)) {
        *(unsigned *)((char *)statxbuf + STX_NLINK_OFF) = (unsigned)c;
        *(unsigned *)((char *)statxbuf + STX_MASK_OFF) |= CNG_STATX_NLINK;
    }
}

int cng_l2s_deny(long dirfd, const char *gp) {
    if (!gp || !gp[0])
        return 0;
    /* Backing data/marker names do not exist for the guest, wherever they
     * are named from. */
    const char *b = strrchr(gp, '/');
    if (l2s_hidden(b ? b + 1 : gp))
        return 1;
    /* The store dir itself: deny "/.l2s" and everything under it. Only for
     * absolute/cwd-relative paths — a ".l2s" entry elsewhere in the tree
     * stays usable, and a dirfd-relative walk can only reach the store
     * through ".." (accepted, documented). */
    if ((gp[0] == '/' || (int)dirfd == CNG_AT_FDCWD) && cng_g_fs) {
        char canon[CNG_PATH_MAX];
        if (cng_fs_abscanon(cng_g_fs, gp, canon, sizeof canon) == 0 &&
            l2s_store_name(canon))
            return 1;
    }
    return 0;
}
