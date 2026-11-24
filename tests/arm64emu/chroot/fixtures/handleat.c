/* name_to_handle_at(2) and the calls past the advertised kernel.
 *
 * No file handle is handed out: the answer is the kernel's for a filesystem
 * that does not export handles, EOPNOTSUPP, after the kernel's own checks in
 * the kernel's order -- the flags (6.1 knows AT_SYMLINK_FOLLOW and
 * AT_EMPTY_PATH only), then the name. A syscall newer than the advertised
 * 6.1 (setxattrat, 463) is ENOSYS, as there, and silent: the harness checks
 * that stderr stays empty.
 *
 * Self-checking: a host kernel answers with a real handle. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

static void show(const char *what, long r) {
    printf("%s=%s\n", what, r == 0 ? "0" : r == -1 && errno == EOPNOTSUPP ? "EOPNOTSUPP"
           : errno == ENOENT ? "ENOENT" : errno == EINVAL ? "EINVAL"
           : errno == ENOSYS ? "ENOSYS" : errno == EBADF ? "EBADF" : "other");
}

int main(void) {
    struct { unsigned bytes; int type; unsigned char h[128]; } fh = { 128, 0, {0} };
    int mnt;
    show("root", syscall(SYS_name_to_handle_at, AT_FDCWD, "/", &fh, &mnt, 0));
    show("missing", syscall(SYS_name_to_handle_at, AT_FDCWD, "/no/such/x", &fh, &mnt, 0));
    show("badflag", syscall(SYS_name_to_handle_at, AT_FDCWD, "/", &fh, &mnt, 0x1));
    show("emptyfd", syscall(SYS_name_to_handle_at, 0x7fff, "", &fh, &mnt, AT_EMPTY_PATH));
    show("setxattrat", syscall(463, AT_FDCWD, "/", 0, 0, 0, 0));
    return 0;
}
