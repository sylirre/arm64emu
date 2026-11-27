/* Which file does a guest name reach? (M5)
 *
 * usage: viewpath OP:ARG[:ARG] ...
 *
 * Each operation is made in order, in one process, and answered on a line of
 * its own: "OP ARG: <text>". What a name reaches is told by the content of the
 * file behind it, which the harness plants, so nothing here knows a host path;
 * a call that fails answers "errno N".
 *
 *   read:P            open P and print its first line
 *   readat:D:P        the same, against a descriptor of directory D
 *   fdread:P          open P O_PATH, reopen it through /proc/self/fd, print
 *   fdlink:P          open P, print what /proc/self/fd says its name is
 *   lstat:P  stat:P   the file's type
 *   nofollow:P        open with O_NOFOLLOW
 *   access:P          faccessat(F_OK), with garbage in the register past its
 *                     three arguments (the call has no flags word)
 *   accessnf:P        faccessat2(F_OK, AT_SYMLINK_NOFOLLOW)
 *   badmode:P         access with an invalid mode
 *   getxa:P           getxattr of "user.cng"
 *   inotify:P         inotify_add_watch
 *   mk:P              open P O_CREAT|O_EXCL for writing
 *   chroot:P chdir:P  move the root, the working directory
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/xattr.h>
#include <unistd.h>

#ifndef SYS_faccessat2
#define SYS_faccessat2 439
#endif

static void err(void) { printf("errno %d\n", errno); }

static void content(int fd) {
    char b[256];
    if (fd < 0) {
        err();
        return;
    }
    ssize_t n = read(fd, b, sizeof b - 1);
    if (n < 0) {
        err();
        close(fd);
        return;
    }
    b[n] = 0;
    b[strcspn(b, "\n")] = 0;
    printf("%s\n", b);
    close(fd);
}

static const char *type(mode_t m) {
    return S_ISLNK(m) ? "symlink" : S_ISDIR(m) ? "directory" : S_ISREG(m) ? "file"
         : S_ISCHR(m) ? "char device" : "other";
}

int main(int argc, char **argv) {
    setvbuf(stdout, 0, _IOLBF, 0);
    for (int i = 1; i < argc; i++) {
        char *op = argv[i], *p = strchr(op, ':');
        if (!p)
            return 2;
        *p++ = 0;
        char *q = 0;
        struct stat st;
        if (!strcmp(op, "readat") && (q = strchr(p, ':')))
            *q++ = 0;
        /* A name too long for the line is named by its length. */
        if (strlen(p) > 200)
            printf("%s <%d>: ", op, (int)strlen(p));
        else
            printf("%s %s%s%s: ", op, p, q ? ":" : "", q ? q : "");
        if (!strcmp(op, "read")) {
            content(open(p, O_RDONLY));
        } else if (!strcmp(op, "readat")) {
            int d = open(p, O_RDONLY | O_DIRECTORY);
            content(openat(d, q, O_RDONLY));
            if (d >= 0)
                close(d);
        } else if (!strcmp(op, "fdread")) {
            int d = open(p, O_PATH);
            char l[64];
            snprintf(l, sizeof l, "/proc/self/fd/%d", d);
            content(open(l, O_RDONLY));
            if (d >= 0)
                close(d);
        } else if (!strcmp(op, "fdlink")) {
            int d = open(p, O_RDONLY);
            char l[64], b[512];
            snprintf(l, sizeof l, "/proc/self/fd/%d", d);
            ssize_t n = readlink(l, b, sizeof b - 1);
            if (n < 0)
                err();
            else
                printf("%.*s\n", (int)n, b);
            if (d >= 0)
                close(d);
        } else if (!strcmp(op, "lstat") || !strcmp(op, "stat")) {
            if (fstatat(AT_FDCWD, p, &st, *op == 'l' ? AT_SYMLINK_NOFOLLOW : 0))
                err();
            else
                printf("%s\n", type(st.st_mode));
        } else if (!strcmp(op, "nofollow")) {
            content(open(p, O_RDONLY | O_NOFOLLOW));
        } else if (!strcmp(op, "access")) {
            if (syscall(SYS_faccessat, AT_FDCWD, p, F_OK, 0x7fffffffUL))
                err();
            else
                printf("ok\n");
        } else if (!strcmp(op, "accessnf")) {
            if (syscall(SYS_faccessat2, AT_FDCWD, p, F_OK, AT_SYMLINK_NOFOLLOW))
                err();
            else
                printf("ok\n");
        } else if (!strcmp(op, "badmode")) {
            if (syscall(SYS_faccessat, AT_FDCWD, p, 8))
                err();
            else
                printf("ok\n");
        } else if (!strcmp(op, "getxa")) {
            char b[64];
            if (getxattr(p, "user.cng", b, sizeof b) < 0)
                err();
            else
                printf("ok\n");
        } else if (!strcmp(op, "inotify")) {
            int d = inotify_init1(0);
            if (inotify_add_watch(d, p, IN_OPEN) < 0)
                err();
            else
                printf("ok\n");
            close(d);
        } else if (!strcmp(op, "mk")) {
            int d = open(p, O_WRONLY | O_CREAT | O_EXCL, 0644);
            if (d < 0) {
                err();
            } else {
                printf("ok\n");
                close(d);
            }
        } else if (!strcmp(op, "chroot") || !strcmp(op, "chdir")) {
            if (op[2] == 'r' ? chroot(p) : chdir(p))
                err();
            else
                printf("ok\n");
        } else {
            return 2;
        }
    }
    return 0;
}
