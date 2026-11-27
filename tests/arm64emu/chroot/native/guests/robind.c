/* What a read-only bind refuses, and what it does not (M5).
 *
 * usage: robind ro|rw DIR FILE [SRC]    every mutating call on a plain file
 *        robind l2s-ro|l2s-rw DIR FILE  the same on a name of a link2symlink
 *                                       group (FILE and FILE.l2, made earlier)
 *        robind link-ro|link-rw DIR FILE  links out of the bind
 *        robind mklink DIR FILE         make FILE.l2 a link of FILE
 *
 * DIR is the bind, FILE a file in it. A real read-only mount answers EROFS to
 * every one of these calls, however the file is named: by path, relative to a
 * descriptor of the mount, through the fd link of a descriptor opened on it, or
 * by that descriptor alone. The rw runs are the control: the same calls must NOT
 * answer EROFS there, so a blanket refusal cannot pass both. SRC is a writable
 * name of another file with FILE's content, for FIDEDUPERANGE's source.
 *
 * Prints "robind ro|rw <leg>: rc=N -> OK|FAIL" per leg, N being 0 or -errno.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SYS_fchmodat2
#define SYS_fchmodat2 452
#endif
#ifndef SYS_setxattrat
#define SYS_setxattrat 463
#endif
#ifndef SYS_file_setattr
#define SYS_file_setattr 469
#endif
#define IOC_SETFLAGS   _IOW('f', 2, long)
#define IOC_FSSETXATTR 0x401c5820UL
#define IOC_DEDUPE     0xc0189436UL

static int ro, fails;
static const char *tag;

/* A syscall's answer as the kernel gives it: 0 or more, or -errno. */
#define K(...) ({ long r_ = syscall(__VA_ARGS__); r_ < 0 ? -errno : r_; })

static void leg(const char *name, long r, int ok) {
    printf("%s %s %s: rc=%ld -> %s\n", tag, ro ? "ro" : "rw", name, r < 0 ? r : 0L,
           ok ? "OK" : "FAIL");
    fails += !ok;
}

/* A mutator: EROFS exactly when the bind is read-only. */
static void mut(const char *name, long r) { leg(name, r, (r == -EROFS) == ro); }

/* An open that must be refused, or not: its descriptor is closed. */
static void mutfd(const char *name, long r) {
    mut(name, r);
    if (r >= 0)
        close((int)r);
}
static void readfd(const char *name, long r) {
    leg(name, r, r >= 0);
    if (r >= 0)
        close((int)r);
}
/* A call on a name that is not there: `want` under :ro, never EROFS under rw. */
static void miss(const char *name, long r, long want) {
    leg(name, r, ro ? r == want : r != -EROFS);
}

static long opn(const char *p, int fl) { return K(SYS_openat, AT_FDCWD, p, fl, 0644); }

static void plain(const char *dir, const char *f, const char *src) {
    char sib[512], absent[512], missc[512], missd[512], at[128];
    snprintf(sib, sizeof sib, "%s.x", f);
    snprintf(absent, sizeof absent, "%s.absent", f);
    snprintf(missc, sizeof missc, "%s.absentc", f);
    snprintf(missd, sizeof missd, "%s.absentd", f);

    /* Names that are not there. For the calls that must operate on an existing
     * name the kernel gets to the read-only mount after the path has resolved,
     * so a missing one is ENOENT; the create-and-remove family takes write
     * access on the parent first, and is EROFS even for it. */
    long r;
    miss("miss-open-w", opn(absent, O_WRONLY), -ENOENT);
    miss("miss-open-trunc", opn(absent, O_RDONLY | O_TRUNC), -ENOENT);
    miss("miss-truncate", K(SYS_truncate, absent, 0), -ENOENT);
    miss("miss-chmod", K(SYS_fchmodat, AT_FDCWD, absent, 0600), -ENOENT);
    miss("miss-chown", K(SYS_fchownat, AT_FDCWD, absent, -1, -1, 0), -ENOENT);
    miss("miss-utimensat", K(SYS_utimensat, AT_FDCWD, absent, 0, 0), -ENOENT);
    r = opn(missc, O_WRONLY | O_CREAT);
    miss("miss-open-creat", r, -EROFS);
    if (r >= 0)
        close((int)r);
    miss("miss-mkdir", K(SYS_mkdirat, AT_FDCWD, missd, 0755), -EROFS);
    miss("miss-unlink", K(SYS_unlinkat, AT_FDCWD, absent, 0), -EROFS);

    /* Relative to a directory descriptor inside the bind, which is how rm -rf,
     * find -delete, tar and git all work. */
    const char *base = strrchr(f, '/') + 1;
    snprintf(at, sizeof at, "%s.at", base);
    int d = (int)opn(dir, O_RDONLY | O_DIRECTORY);
    readfd("at-read", K(SYS_openat, d, base, O_RDONLY, 0));
    mutfd("at-open-w", K(SYS_openat, d, base, O_WRONLY, 0));
    mutfd("at-creat", K(SYS_openat, d, at, O_WRONLY | O_CREAT, 0644));
    mut("at-unlink", K(SYS_unlinkat, d, at, 0));

    /* Through the fd links of a descriptor opened read-only, and of one opened
     * O_PATH, under the bind. The read-only reopen is the control. */
    int rfd = (int)opn(f, O_RDONLY), pfd = (int)opn(f, O_PATH);
    char fl[48], dl[48], pl[48];
    snprintf(fl, sizeof fl, "/proc/self/fd/%d", rfd);
    snprintf(dl, sizeof dl, "/dev/fd/%d", rfd);
    snprintf(pl, sizeof pl, "/proc/self/fd/%d", pfd);
    readfd("fdlink-read", opn(fl, O_RDONLY));
    mutfd("fdlink-open-w", opn(fl, O_WRONLY));
    mutfd("fdlink-open-trunc", opn(fl, O_RDONLY | O_TRUNC));
    mutfd("devfd-open-w", opn(dl, O_WRONLY));
    mutfd("pathfd-open-w", opn(pl, O_WRONLY));
    mut("fdlink-truncate", K(SYS_truncate, fl, 0));
    mut("fdlink-chmod", K(SYS_fchmodat, AT_FDCWD, fl, 0644));

    /* By descriptor alone (mnt_want_write_file): no path to key on. */
    long ioflags = 0;
    unsigned char fsx[28] = {0};
    mut("fchmod", K(SYS_fchmod, rfd, 0644));
    mut("fchown", K(SYS_fchown, rfd, -1, -1));
    mut("fchmod-dir", K(SYS_fchmod, d, 0755));
    mut("futimens", K(SYS_utimensat, rfd, 0, 0, 0));
    mut("fsetxattr", K(SYS_fsetxattr, rfd, "user.cng", "1", 1, 0));
    mut("fremovexattr", K(SYS_fremovexattr, rfd, "user.cng"));
    mut("fchownat-empty", K(SYS_fchownat, rfd, "", -1, -1, AT_EMPTY_PATH));
    mut("ioctl-setflags", K(SYS_ioctl, rfd, IOC_SETFLAGS, &ioflags));
    mut("ioctl-fssetxattr", K(SYS_ioctl, rfd, IOC_FSSETXATTR, fsx));
    if (chdir(dir) == 0) {
        mut("cwd-empty", K(SYS_fchownat, AT_FDCWD, "", -1, -1, AT_EMPTY_PATH));
        if (chdir("/") != 0)
            exit(2);
    }
    /* FIDEDUPERANGE names its target in its argument: the source is a writable
     * name, and the destination has to come back with EROFS in its own status.
     * A filesystem with no dedupe fails the call whole, which says nothing. */
    if (src) {
        struct {
            unsigned long off, len;
            unsigned short n, r1;
            unsigned r2;
            long fd;
            unsigned long doff, done;
            int status;
            unsigned r3;
        } dd;
        memset(&dd, 0, sizeof dd);
        dd.n = 1;
        dd.fd = rfd;
        int s = (int)opn(src, O_RDONLY);
        long x = K(SYS_ioctl, s, IOC_DEDUPE, &dd);
        printf("%s %s dedupe: rc=%ld status=%d -> %s\n", tag, ro ? "ro" : "rw", x,
               x < 0 ? 0 : dd.status,
               x < 0 ? "UNSUPPORTED" : (dd.status == -EROFS) == ro ? "OK" : "FAIL");
        fails += x >= 0 && (dd.status == -EROFS) != ro;
        close(s);
    }
    close(rfd);
    close(pfd);
    close(d);

    readfd("read", opn(f, O_RDONLY));
    r = K(SYS_faccessat, AT_FDCWD, f, R_OK);
    leg("access-r", r, r == 0);
    mut("access-w", K(SYS_faccessat, AT_FDCWD, f, W_OK));
    mutfd("open-w", opn(f, O_WRONLY));
    mutfd("open-creat", opn(sib, O_RDONLY | O_CREAT));
    mut("mkdirat", K(SYS_mkdirat, AT_FDCWD, sib, 0755));
    mut("fchmodat", K(SYS_fchmodat, AT_FDCWD, f, 0600));
    mut("truncate", K(SYS_truncate, f, 0));
    mut("symlinkat", K(SYS_symlinkat, "/t", AT_FDCWD, sib));
    mut("utimensat", K(SYS_utimensat, AT_FDCWD, f, 0, 0));
    mut("fchownat", K(SYS_fchownat, AT_FDCWD, f, -1, -1, 0));
    mut("setxattr", K(SYS_setxattr, f, "user.cng", "1", 1, 0));
    mut("renameat", K(SYS_renameat, AT_FDCWD, f, AT_FDCWD, sib));
    mut("unlinkat", K(SYS_unlinkat, AT_FDCWD, ro ? f : sib, 0));
}

static void l2s(const char *f) {
    char sib[512], fl[48];
    long r;
    snprintf(sib, sizeof sib, "%s.l2", f);
    struct stat st;
    /* Both names of one file: the group the first run made. */
    leg("group", 0, lstat(f, &st) == 0 && S_ISREG(st.st_mode) && st.st_nlink == 2);
    readfd("read", opn(f, O_RDONLY));
    r = K(SYS_faccessat, AT_FDCWD, f, R_OK);
    leg("access-r", r, r == 0);
    mutfd("open-w", opn(f, O_WRONLY));
    mutfd("open-trunc", opn(f, O_RDONLY | O_TRUNC));
    mut("access-w", K(SYS_faccessat, AT_FDCWD, f, W_OK));
    mut("truncate", K(SYS_truncate, f, 0));
    mut("fchmodat", K(SYS_fchmodat, AT_FDCWD, f, 0600));
    mut("fchownat", K(SYS_fchownat, AT_FDCWD, f, -1, -1, 0));
    mut("utimensat", K(SYS_utimensat, AT_FDCWD, f, 0, 0));
    mut("setxattr", K(SYS_setxattr, f, "user.cng", "1", 1, 0));
    int rfd = (int)opn(f, O_RDONLY);
    snprintf(fl, sizeof fl, "/proc/self/fd/%d", rfd);
    mutfd("fdlink-open-w", opn(fl, O_WRONLY));
    close(rfd);
    mut("unlinkat", K(SYS_unlinkat, AT_FDCWD, sib, 0));
}

static void links(const char *f) {
    /* A link cannot span mounts, and the bind is one: EXDEV, after the new
     * name's own verdict. Out of a plain bind it is the host's to make. */
    long r, want = ro ? -EXDEV : 0;
    struct stat a, b;
    int had = lstat(f, &a) == 0;
    if (symlink(f, "/rolink.sym") != 0 && errno != EEXIST)
        exit(2);
    close(open("/rolink.taken", O_CREAT | O_WRONLY, 0644));
    int fd = (int)opn(f, O_RDONLY);
    r = K(SYS_linkat, AT_FDCWD, f, AT_FDCWD, "/rolink.a", 0);
    leg("name", r, r == want);
    r = K(SYS_linkat, AT_FDCWD, "/rolink.sym", AT_FDCWD, "/rolink.b", AT_SYMLINK_FOLLOW);
    leg("followed", r, r == want);
    r = K(SYS_linkat, AT_FDCWD, f, AT_FDCWD, "/rolink.taken", 0);
    leg("dst-taken", r, r == -EEXIST);
    r = K(SYS_linkat, AT_FDCWD, f, AT_FDCWD, "/rolink.none/x", 0);
    leg("dst-nodir", r, r == -ENOENT);
    /* By descriptor the source is the kernel's to judge first: a kernel before
     * 6.10 refuses AT_EMPTY_PATH without CAP_DAC_READ_SEARCH, ENOENT. */
    r = K(SYS_linkat, fd, "", AT_FDCWD, "/rolink.e", AT_EMPTY_PATH);
    leg("by-fd", r, r == want || r == -ENOENT);
    char l[48];
    snprintf(l, sizeof l, "/proc/self/fd/%d", fd);
    r = K(SYS_linkat, AT_FDCWD, l, AT_FDCWD, "/rolink.g", AT_SYMLINK_FOLLOW);
    leg("fdlink", r, r == want);
    if (ro)
        leg("file-kept", 0, had && lstat(f, &b) == 0 && S_ISREG(b.st_mode) &&
                            b.st_ino == a.st_ino && b.st_nlink == 1);
    close(fd);
}

int main(int argc, char **argv) {
    setvbuf(stdout, 0, _IOLBF, 0);
    if (argc < 4)
        return 2;
    const char *how = argv[1];
    if (!strcmp(how, "mklink")) {
        char sib[512];
        snprintf(sib, sizeof sib, "%s.l2", argv[3]);
        return link(argv[3], sib) != 0;
    }
    size_t n = strlen(how);
    ro = n >= 2 && how[n - 2] == 'r' && how[n - 1] == 'o';
    tag = !strncmp(how, "l2s", 3) ? "l2sro" : !strncmp(how, "link", 4) ? "rolink" : "robind";
    if (!strcmp(tag, "l2sro"))
        l2s(argv[3]);
    else if (!strcmp(tag, "rolink"))
        links(argv[3]);
    else
        plain(argv[2], argv[3], argc > 4 ? argv[4] : 0);
    printf("%s: %d failures\n", tag, fails);
    return fails ? 1 : 0;
}
