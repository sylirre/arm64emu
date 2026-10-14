/* The terminal ioctls a pty slave answers from the tty core alone, TIOCOUTQ /
 * SIOCOUTQ, and the job-control check some of them make; self-checking.
 *
 * Android's SELinux policy lets an app issue only a few terminal commands on
 * its pty slave (TCGETS, TCSETS*, TCFLSH, TIOCSCTTY, TIOC[GS]PGRP,
 * TIOC[GS]WINSZ, TIOCOUTQ) and refuses the rest with EACCES -- tcdrain(3),
 * tcsendbreak(3) and tcflow(3) among them. A pty slave has no ioctl, break,
 * modem lines, output queue or xchar of its own, so the kernel's answers are
 * the tty core's, and the emulator serves them from the commands the policy
 * allows (src/sys_file.c, pts_refused); A64_TTYIOCTL_FORCE_DENY makes any host
 * refuse them the same way, and the answers here must be the kernel's over it.
 *
 * What is asked:
 *   - TIOCGSOFTCAR/TIOCSSOFTCAR: CLOCAL read and set (any non-zero sets it),
 *     seen from TCGETS and through the master too; EFAULT both ways for a
 *     null buffer;
 *   - TCSBRK (drain, break), TCSBRKP, TCXONC (TCOON, TCIOFF and TCION -- the
 *     master reads the STOP and START characters, and nothing once VSTOP is
 *     disabled -- an invalid selector, and selectors past 32 bits, which a
 *     64-bit kernel takes as invalid and a 32-bit host must not wrap onto a
 *     valid one; TCFLSH likewise);
 *   - TIOCNOTTY on a slave that is not the caller's controlling terminal;
 *   - the modem-line, packet-mode, pty-number and console commands, ENOTTY;
 *   - TIOCOUTQ on the slave and the master (empty: 0), a pipe (ENOTTY), a
 *     socket with bytes unread at its peer (some) and after they are read
 *     (none), a null buffer on a socket (EFAULT); FIONREAD and TIOCOUTQ on a
 *     netlink socket, which answers no ioctl of its own (ENOTTY) -- the suite
 *     runs this again with the host's netlink refused, where the emulator's
 *     AF_UNIX stand-in must not count its bytes;
 *   - from a session whose controlling terminal the slave is, in a background
 *     process group: with SIGTTOU ignored TCSBRK, TCXONC and TIOCSSOFTCAR all
 *     pass; with it at its default TIOCSSOFTCAR still passes (the kernel makes
 *     no job-control check for it, though the TCSETS the emulator serves it
 *     with does) and TCSBRK stops the group with SIGTTOU; and in an orphaned
 *     group TCSBRK and TCXONC are EIO.
 * TCXONC's TCOOFF and TIOCNOTTY on the caller's own controlling terminal are
 * not asked: nothing an app may issue does either, so the emulator leaves
 * them refused where the policy refuses them.
 *
 * The expected output is what this program prints built for the host and run
 * on a real kernel. A host that will not hand a pty to a new session steps
 * aside with a lone SKIP line. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <linux/netlink.h>

#define T_GETS      0x5401UL
#define T_SETS      0x5402UL
#define T_SBRK      0x5409UL
#define T_XONC      0x540AUL
#define T_FLSH      0x540BUL
#define T_SCTTY     0x540EUL
#define T_OUTQ      0x5411UL
#define T_GSOFTCAR  0x5419UL
#define T_SSOFTCAR  0x541AUL
#define T_INQ       0x541BUL
#define T_NOTTY     0x5422UL
#define T_SBRKP     0x5425UL
#define CLOCAL_BIT  0x800u

/* r and errno of one ioctl, as "r errno=e" (errno 0 on success). */
static long ask(int fd, unsigned long cmd, unsigned long arg, int *err) {
    errno = 0;
    long r = syscall(SYS_ioctl, fd, cmd, arg);
    *err = r < 0 ? errno : 0;
    return r;
}

static void row(const char *name, int fd, unsigned long cmd, unsigned long arg) {
    int e;
    long r = ask(fd, cmd, arg, &e);
    printf("%s=%ld errno=%d\n", name, r, e);
}

static int softcar(int fd) {
    int v = -7, e;
    ask(fd, T_GSOFTCAR, (unsigned long)&v, &e);
    return e ? -e : v;
}

static int clocal(int fd) {
    unsigned char t[36];
    unsigned int cflag;
    if (ioctl(fd, T_GETS, t) < 0) return -1;
    memcpy(&cflag, t + 8, 4);
    return (cflag & CLOCAL_BIT) != 0;
}

/* What the master has to read, as hex, or "none". */
static void drain(const char *name, int m) {
    unsigned char b[64];
    usleep(20000);
    ssize_t n = read(m, b, sizeof b);
    printf("%s=", name);
    if (n <= 0) printf("none");
    for (ssize_t i = 0; i < n; i++) printf("%02x", b[i]);
    printf("\n");
}

static void set_vstop(int fd, unsigned char ch) {
    unsigned char t[36];
    if (ioctl(fd, T_GETS, t) == 0) {
        t[17 + 9] = ch;
        ioctl(fd, T_SETS, t);
    }
}

/* Copy what a child reported to `out`, waiting at most `ms` for it to start:
 * a child that stopped or hung where it should not have must make a line
 * missing, not a run that never ends. */
static void relay(int from, int out, int ms) {
    char got[512];
    struct pollfd pf = { from, POLLIN, 0 };
    if (poll(&pf, 1, ms) != 1) return;
    ssize_t n = read(from, got, sizeof got);
    if (n > 0 && write(out, got, (size_t)n) < 0) _exit(1);
}

/* The job-control half, run as the leader of a session the slave is the
 * controlling terminal of; every line goes out through `out`. */
static int session(const char *path, int out) {
    char line[256];
    if (setsid() < 0) return 1;
    int s = open(path, O_RDWR);
    if (s < 0 || ioctl(s, T_SCTTY, 0) < 0) {
        snprintf(line, sizeof line, "SKIP: the host refuses TIOCSCTTY on a pty (errno %d)\n", errno);
        if (write(out, line, strlen(line)) < 0) return 1;
        return 0;
    }
    int rep[2];
    if (pipe(rep)) return 1;
    pid_t bg = fork();
    if (bg == 0) {
        close(rep[0]);
        setpgid(0, 0);                     /* a background group of the session */
        int e1, e2, e3, e4, one = 1;
        signal(SIGTTOU, SIG_IGN);
        long a = ask(s, T_SBRK, 1, &e1);
        long b = ask(s, T_XONC, 1 /*TCOON*/, &e2);
        long c = ask(s, T_SSOFTCAR, (unsigned long)&one, &e3);
        signal(SIGTTOU, SIG_DFL);
        int zero = 0;
        long d = ask(s, T_SSOFTCAR, (unsigned long)&zero, &e4);
        snprintf(line, sizeof line,
                 "bg_ignored: tcsbrk=%ld/%d tcxonc=%ld/%d softcar=%ld/%d\n"
                 "bg_default: softcar=%ld/%d\n", a, e1, b, e2, c, e3, d, e4);
        if (write(rep[1], line, strlen(line)) < 0) _exit(1);
        ask(s, T_SBRK, 1, &e1);            /* stops here: SIGTTOU */
        snprintf(line, sizeof line, "bg_default: tcsbrk not stopped (%d)\n", e1);
        if (write(rep[1], line, strlen(line)) < 0) _exit(1);
        _exit(0);
    }
    close(rep[1]);
    int st = 0;
    pid_t w = waitpid(bg, &st, WUNTRACED);
    int stopped = w == bg && WIFSTOPPED(st), sig = stopped ? WSTOPSIG(st) : 0;
    kill(bg, SIGKILL);              /* before reading: a stopped writer never closes */
    waitpid(bg, &st, 0);
    relay(rep[0], out, 0);
    snprintf(line, sizeof line, "bg_default: tcsbrk %s %s\n",
             stopped ? "stopped" : "not-stopped", sig == SIGTTOU ? "SIGTTOU" : "-");
    if (write(out, line, strlen(line)) < 0) return 1;
    close(rep[0]);

    /* An orphaned background group: its one member's parent left, and the
     * process it was handed to is outside the session. */
    if (pipe(rep)) return 1;
    pid_t mid = fork();
    if (mid == 0) {
        setpgid(0, 0);
        pid_t me = getpid();
        if (fork() == 0) {
            close(rep[0]);
            for (int i = 0; i < 2000 && getppid() == me; i++) usleep(1000);
            int e1, e2;
            long a = ask(s, T_SBRK, 1, &e1);
            long b = ask(s, T_XONC, 1, &e2);
            snprintf(line, sizeof line, "orphaned: tcsbrk=%ld/%d tcxonc=%ld/%d\n", a, e1, b, e2);
            if (write(rep[1], line, strlen(line)) < 0) _exit(1);
            _exit(0);
        }
        _exit(0);
    }
    close(rep[1]);
    waitpid(mid, &st, 0);
    relay(rep[0], out, 10000);
    kill(-mid, SIGKILL);            /* the orphan, should it have stopped instead */
    return 0;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0 || grantpt(m) || unlockpt(m)) { printf("SKIP: no pty (errno %d)\n", errno); return 0; }
    char path[64];
    snprintf(path, sizeof path, "%s", ptsname(m));
    int s = open(path, O_RDWR | O_NOCTTY);
    if (s < 0) { printf("SKIP: the pty slave cannot be opened (errno %d)\n", errno); return 0; }
    fcntl(m, F_SETFL, fcntl(m, F_GETFL) | O_NONBLOCK);

    /* The job-control half first: it needs the pty fresh, and it decides
     * whether the rest runs at all. */
    int out[2];
    if (pipe(out)) return 1;
    pid_t k = fork();
    if (k == 0) { close(out[0]); _exit(session(path, out[1])); }
    close(out[1]);
    char jc[1024];
    ssize_t jn = 0, n;
    while ((n = read(out[0], jc + jn, sizeof jc - 1 - (size_t)jn)) > 0) jn += n;
    jc[jn] = 0;
    int st;
    waitpid(k, &st, 0);
    if (!strncmp(jc, "SKIP:", 5)) { fputs(jc, stdout); return 0; }

    int e, v = 7, zero = 0;
    printf("softcar_get0=%d\n", softcar(s));
    row("softcar_set1", s, T_SSOFTCAR, (unsigned long)&v);
    printf("softcar_now=%d clocal=%d master=%d\n", softcar(s), clocal(s), softcar(m));
    row("softcar_set0", s, T_SSOFTCAR, (unsigned long)&zero);
    printf("softcar_now=%d clocal=%d master=%d\n", softcar(s), clocal(s), softcar(m));
    row("softcar_get_fault", s, T_GSOFTCAR, 0);
    row("softcar_set_fault", s, T_SSOFTCAR, 0);

    row("tcsbrk_drain", s, T_SBRK, 1);
    row("tcsbrk_break", s, T_SBRK, 0);
    row("tcsbrkp_0", s, T_SBRKP, 0);
    row("tcsbrkp_2", s, T_SBRKP, 2);
    row("tcxonc_on", s, T_XONC, 1);
    row("tcxonc_bad", s, T_XONC, 9);
    row("tcxonc_wide", s, T_XONC, (unsigned long)(sizeof(long) > 4 ? 0x100000001ULL : 9));
    row("tcflsh_wide", s, T_FLSH, (unsigned long)(sizeof(long) > 4 ? 0x100000000ULL : 9));
    drain("before_xchar", m);
    row("tcxonc_ioff", s, T_XONC, 2);
    drain("master_read", m);
    row("tcxonc_ion", s, T_XONC, 3);
    drain("master_read", m);
    set_vstop(s, 0);
    row("tcxonc_ioff_disabled", s, T_XONC, 2);
    drain("master_read", m);
    set_vstop(s, 0x13);

    row("notty_not_ctty", s, T_NOTTY, 0);
    static const struct { unsigned long cmd; const char *name; } enotty[] = {
        { 0x5415, "TIOCMGET" }, { 0x5416, "TIOCMBIS" }, { 0x5417, "TIOCMBIC" },
        { 0x5418, "TIOCMSET" }, { 0x5420, "TIOCPKT" }, { 0x80045430, "TIOCGPTN" },
        { 0x40045431, "TIOCSPTLCK" }, { 0x5603, "VT_GETSTATE" }, { 0x4b33, "KDGKBTYPE" },
    };
    printf("slave_tty_core:");
    for (unsigned i = 0; i < sizeof enotty / sizeof enotty[0]; i++) {
        unsigned char buf[64];
        memset(buf, 0, sizeof buf);
        ask(s, enotty[i].cmd, (unsigned long)buf, &e);
        printf(" %s=%d", enotty[i].name, e);
    }
    printf("\n");

    int q = -7;
    long r = ask(s, T_OUTQ, (unsigned long)&q, &e);
    printf("outq_slave=%ld errno=%d n=%d\n", r, e, q);
    q = -7;
    r = ask(m, T_OUTQ, (unsigned long)&q, &e);
    printf("outq_master=%ld errno=%d n=%d\n", r, e, q);
    int p[2], sp[2];
    if (pipe(p) || socketpair(AF_UNIX, SOCK_STREAM, 0, sp)) return 1;
    row("outq_pipe", p[0], T_OUTQ, (unsigned long)&q);
    char data[100];
    memset(data, 'x', sizeof data);
    if (write(sp[0], data, sizeof data) != (ssize_t)sizeof data) return 1;
    q = -7;
    r = ask(sp[0], T_OUTQ, (unsigned long)&q, &e);
    printf("outq_sock=%ld errno=%d queued=%d\n", r, e, q > 0);
    if (read(sp[1], data, sizeof data) != (ssize_t)sizeof data) return 1;
    q = -7;
    r = ask(sp[0], T_OUTQ, (unsigned long)&q, &e);
    printf("outq_sock_read=%ld errno=%d n=%d\n", r, e, q);
    row("outq_sock_fault", sp[0], T_OUTQ, 0);
    int nl = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (nl >= 0) {
        row("netlink_fionread", nl, T_INQ, (unsigned long)&q);
        row("netlink_outq", nl, T_OUTQ, (unsigned long)&q);
    } else {
        printf("netlink: %s\n", strerror(errno));
    }

    fputs(jc, stdout);
    printf("done\n");
    return 0;
}
