/* Which error an ioctl with an unreadable argument gets, self-checking.
 *
 * NEEDS-HOST-SYSCALL: ioctl-ptr-late
 *
 * A kernel reads an ioctl's argument only once the file has taken the
 * command: a descriptor that is not open (or is open O_PATH) is EBADF, a pipe
 * asked a terminal command ENOTTY, a pty slave asked for its modem lines,
 * packet mode or pty number ENOTTY -- whatever the pointer. Only where the
 * handler goes on to read it is a bad one EFAULT. The emulator copied every
 * input argument in from the guest first and answered EFAULT for one it could
 * not read, ahead of all of those; it now hands the host an address no process
 * has and lets the kernel decide (src/sys_file.c, SYSDEF(ioctl)). The answers
 * must be the kernel's also where a policy refuses the command ahead of the
 * file (A64_TTYIOCTL_FORCE_DENY), and for the commands then served in the
 * host's place (TIOCSSOFTCAR). An output argument is written after the
 * handler ran, so a bad one is EFAULT on any descriptor that answers.
 *
 * qemu-user locks an ioctl's argument in its own memory before it asks the
 * kernel, so it answers EFAULT first too; the ARM32 tier steps aside here.
 * The expected output is what this program prints built for the host and run
 * on a real kernel. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/syscall.h>

static void row(const char *name, int fd, unsigned long cmd) {
    errno = 0;
    long r = syscall(SYS_ioctl, fd, cmd, 0UL);
    printf("%s=%ld errno=%d\n", name, r, r < 0 ? errno : 0);
}

int main(void) {
    int p[2], sp[2];
    if (pipe(p) || socketpair(AF_UNIX, SOCK_STREAM, 0, sp)) return 1;
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0 || grantpt(m) || unlockpt(m)) { printf("SKIP: no pty\n"); return 0; }
    int s = open(ptsname(m), O_RDWR | O_NOCTTY);
    int path = open(".", O_PATH);
    int closed = dup(p[0]);
    close(closed);
    if (s < 0 || path < 0) { printf("SKIP: no pty slave or O_PATH\n"); return 0; }

    /* refused before the argument is read */
    row("pipe_tcsets", p[0], 0x5402);
    row("pipe_tiocswinsz", p[0], 0x5414);
    row("pipe_tiocspgrp", p[1], 0x5410);
    row("sock_tcsets", sp[0], 0x5402);
    row("closed_tcsets", closed, 0x5402);
    row("closed_fionbio", closed, 0x5421);
    row("opath_tcsets", path, 0x5402);
    row("slave_tiocmset", s, 0x5418);
    row("slave_tiocmbis", s, 0x5416);
    row("slave_tiocpkt", s, 0x5420);
    row("slave_tiocsptlck", s, 0x40045431);
    /* read, and so EFAULT */
    row("slave_tcsets", s, 0x5402);
    row("slave_tiocswinsz", s, 0x5414);
    row("slave_tiocssoftcar", s, 0x541A);
    row("master_tiocpkt", m, 0x5420);
    row("pipe_fionbio", p[0], 0x5421);
    /* written after the handler ran */
    row("pipe_tcgets", p[0], 0x5401);
    row("slave_tcgets", s, 0x5401);
    row("slave_tiocgsoftcar", s, 0x5419);
    row("sock_tiocoutq", sp[0], 0x5411);
    printf("done\n");
    return 0;
}
