/* pidfd_getfd, which the emulator answered ENOSYS: a duplicate of another
 * process's descriptor -- the same open file description, so what is written
 * through the copy arrives where the target's goes -- handed over O_CLOEXEC,
 * and the refusals in the kernel's order: flags (EINVAL), a descriptor that is
 * no pidfd (EBADF), a number the target has nothing at (EBADF), a process
 * reaped since (ESRCH). One's own descriptors can be taken as well.
 *
 * NEEDS-HOST-SYSCALL: pidfd
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef SYS_pidfd_getfd
#define SYS_pidfd_getfd 438
#endif

static const char *en(long r) {
    if (r >= 0) return "ok";
    switch (errno) {
    case EINVAL: return "EINVAL";
    case EBADF:  return "EBADF";
    case ESRCH:  return "ESRCH";
    case EPERM:  return "EPERM";
    default:     return "other";
    }
}

static long getfd(int pfd, int fd, unsigned flags) {
    return syscall(SYS_pidfd_getfd, pfd, fd, flags);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int p[2], ready[2];
    if (pipe(p) || pipe(ready)) return 1;
    pid_t k = fork();
    if (k == 0) {
        /* The write end, at a number of the child's own, and nothing at 51. */
        if (dup2(p[1], 50) != 50) _exit(2);
        close(p[1]);
        close(51);
        if (write(ready[1], "r", 1) != 1) _exit(2);
        for (;;) pause();
    }
    close(p[1]);
    char b;
    if (read(ready[0], &b, 1) != 1) return 1;
    int pfd = (int)syscall(SYS_pidfd_open, k, 0);
    if (pfd < 0) { printf("pidfd_open: %s\n", en(pfd)); return 1; }

    printf("flags: %s\n", en(getfd(pfd, 50, 1)));
    printf("not a pidfd: %s\n", en(getfd(p[0], 50, 0)));
    printf("nothing there: %s\n", en(getfd(pfd, 51, 0)));
    printf("negative: %s\n", en(getfd(pfd, -1, 0)));

    long fd = getfd(pfd, 50, 0);
    printf("taken: %s cloexec=%d\n", en(fd),
           fd >= 0 ? (fcntl((int)fd, F_GETFD) & FD_CLOEXEC) != 0 : -1);
    if (fd >= 0) {
        ssize_t w = write((int)fd, "x", 1);
        char got = 0;
        ssize_t n = read(p[0], &got, 1);
        printf("the child's pipe: wrote=%d read=%d byte=%c\n", (int)w, (int)n, got);
        close((int)fd);
    }

    /* One's own. */
    int self = (int)syscall(SYS_pidfd_open, getpid(), 0);
    fd = getfd(self, p[0], 0);
    printf("own: %s same=%d\n", en(fd), fd >= 0 && fd != p[0]);
    if (fd >= 0) close((int)fd);
    close(self);

    kill(k, SIGKILL);
    waitpid(k, NULL, 0);
    printf("reaped: %s\n", en(getfd(pfd, 50, 0)));
    close(pfd);
    return 0;
}
