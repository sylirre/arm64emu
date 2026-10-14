/* The terminal ioctls on descriptors that are no terminal, self-checking.
 *
 * A kernel answers every terminal command ENOTTY on a pipe, a socket, a regular
 * file, a directory, a memfd, a /proc file, an eventfd or /dev/null, and EBADF
 * on a descriptor that is not open or is open O_PATH; FIOCLEX and FIONCLEX set
 * and clear close-on-exec on any descriptor an ioctl can reach.
 *
 * Android's SELinux policy refuses most of those commands with EACCES ahead of
 * the file -- every terminal command but TCGETS on a pipe, a socket or a
 * regular file, TCGETS too on a directory, a memfd or a /proc file, and
 * FIOCLEX/FIONCLEX on a memfd -- so a guest's tcgetpgrp(3) of a pipe read
 * "Permission denied" and an ioctl(FIOCLEX) of a memfd failed. The emulator
 * answers those refusals the way the kernel would have (src/sys_file.c,
 * ioctl_refused); A64_TTYIOCTL_FORCE_DENY makes any host refuse the same way,
 * and the answers must be the kernel's over it too. The termios2 commands and
 * TIOCGSID are asked as well: their own fallbacks lean on TCGETS and TIOCGPGRP,
 * which the policy refuses on the same descriptors.
 *
 * Each descriptor prints "all <errno>" when every terminal command got the same
 * answer and the commands one by one otherwise; then FIOCLEX, F_GETFD, FIONCLEX
 * and F_GETFD again; and FIONREAD of a pipe holding three bytes, which is no
 * terminal command and must not be touched. The expected output is what this
 * program prints built for the host and run on a real kernel. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/syscall.h>

static const struct { unsigned long cmd; const char *name; } cmds[] = {
    { 0x5401, "TCGETS" },      { 0x5402, "TCSETS" },       { 0x5403, "TCSETSW" },
    { 0x5404, "TCSETSF" },     { 0x5409, "TCSBRK" },       { 0x540A, "TCXONC" },
    { 0x540B, "TCFLSH" },      { 0x540E, "TIOCSCTTY" },    { 0x540F, "TIOCGPGRP" },
    { 0x5410, "TIOCSPGRP" },   { 0x5413, "TIOCGWINSZ" },   { 0x5414, "TIOCSWINSZ" },
    { 0x5415, "TIOCMGET" },    { 0x5416, "TIOCMBIS" },     { 0x5417, "TIOCMBIC" },
    { 0x5418, "TIOCMSET" },    { 0x5419, "TIOCGSOFTCAR" }, { 0x541A, "TIOCSSOFTCAR" },
    { 0x5420, "TIOCPKT" },     { 0x5422, "TIOCNOTTY" },    { 0x5429, "TIOCGSID" },
    { 0x80045430, "TIOCGPTN" }, { 0x40045431, "TIOCSPTLCK" }, { 0x5603, "VT_GETSTATE" },
    { 0x4b33, "KDGKBTYPE" },   { 0x802c542a, "TCGETS2" },  { 0x402c542b, "TCSETS2" },
    { 0x402c542c, "TCSETSW2" }, { 0x402c542d, "TCSETSF2" },
};
#define NCMDS (sizeof cmds / sizeof cmds[0])

static const char *ename(int e) {
    static char num[16];
    switch (e) {
    case 0:      return "ok";
    case ENOTTY: return "ENOTTY";
    case EBADF:  return "EBADF";
    case EACCES: return "EACCES";
    case EINVAL: return "EINVAL";
    case EPERM:  return "EPERM";
    case EFAULT: return "EFAULT";
    }
    snprintf(num, sizeof num, "%d", e);
    return num;
}

/* The errno of one ioctl, 0 for success. */
static int ask(int fd, unsigned long cmd, unsigned long arg) {
    errno = 0;
    return syscall(SYS_ioctl, fd, cmd, arg) < 0 ? errno : 0;
}

static void row(const char *what, int fd) {
    int got[NCMDS];
    for (unsigned i = 0; i < NCMDS; i++) {
        unsigned char buf[64];
        memset(buf, 0, sizeof buf);
        got[i] = ask(fd, cmds[i].cmd, (unsigned long)buf);
    }
    unsigned same = 1;
    for (unsigned i = 1; i < NCMDS; i++) same &= got[i] == got[0];
    printf("%s:", what);
    if (same) {
        printf(" all %s", ename(got[0]));
    } else {
        for (unsigned i = 0; i < NCMDS; i++) printf(" %s=%s", cmds[i].name, ename(got[i]));
    }
    int set = ask(fd, 0x5451 /*FIOCLEX*/, 0);
    int on = fcntl(fd, F_GETFD);
    int clr = ask(fd, 0x5450 /*FIONCLEX*/, 0);
    int off = fcntl(fd, F_GETFD);
    printf(" | fioclex=%s fd=%d fionclex=%s fd=%d\n", ename(set), on, ename(clr), off);
}

int main(void) {
    int p[2], s[2];
    if (pipe(p) != 0 || socketpair(AF_UNIX, SOCK_STREAM, 0, s) != 0) {
        perror("pipe/socketpair");
        return 1;
    }
    row("pipe_r", p[0]);
    row("pipe_w", p[1]);
    row("stream", s[0]);
    int dg = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (dg >= 0) row("dgram", dg); else printf("dgram: %s\n", strerror(errno));
    int dir = open(".", O_RDONLY | O_DIRECTORY);
    if (dir >= 0) row("dir", dir); else printf("dir: %s\n", strerror(errno));
    int file = open("/proc/self/exe", O_RDONLY);
    if (file >= 0) row("file", file); else printf("file: %s\n", strerror(errno));
    int mfd = (int)syscall(SYS_memfd_create, "ttynotty", 0);
    if (mfd >= 0) row("memfd", mfd); else printf("memfd: %s\n", strerror(errno));
    int proc = open("/proc/self/status", O_RDONLY);
    if (proc >= 0) row("proc", proc); else printf("proc: %s\n", strerror(errno));
    int ev = eventfd(0, 0);
    if (ev >= 0) row("eventfd", ev); else printf("eventfd: %s\n", strerror(errno));
    int nul = open("/dev/null", O_RDWR);
    if (nul >= 0) row("null", nul); else printf("null: %s\n", strerror(errno));
    int path = open(".", O_PATH);
    if (path >= 0) row("opath", path); else printf("opath: %s\n", strerror(errno));
    int closed = dup(p[0]);
    close(closed);
    row("closed", closed);
    int n = -1;
    if (write(p[1], "abc", 3) != 3) perror("write");
    int e = ask(p[0], 0x541B /*FIONREAD*/, (unsigned long)&n);
    printf("fionread=%s n=%d\n", ename(e), n);
    printf("done\n");
    return 0;
}
