/* chroot(2) looks its name up before it asks for CAP_SYS_CHROOT
 * (ksys_chroot): an unprivileged caller hears EFAULT, ENOENT or ENOTDIR about
 * its path first and EPERM only for a directory it could have entered. The
 * emulator answered EPERM before reading the path at all. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

static void say(const char *what, long r) { printf("%s=%d\n", what, r < 0 ? errno : 0); }

int main(void) {
    if (geteuid() == 0) { printf("root: nothing to compare\n"); return 0; }
    say("null", syscall(SYS_chroot, NULL));
    say("missing", syscall(SYS_chroot, "/nonexistent-chrootorder"));
    say("file", syscall(SYS_chroot, "/proc/self/status"));
    say("dir", syscall(SYS_chroot, "/"));
    return 0;
}
