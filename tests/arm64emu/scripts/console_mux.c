/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* Development harness and worked example for --console-fd / --ctrl-fd.
 *
 * Gives ttyAMA0 and hvc0..hvcN-1 a socketpair each plus a control channel,
 * forks the emulator onto them, and demultiplexes: every guest terminal's
 * output is printed with its own prefix, so "did each console really get its
 * own stream" is answerable by looking. This is the shape any embedding process
 * driving several guest terminals takes, so it doubles as the reference for
 * one — including the descriptor shuffle, which is the part that bites: the
 * child's ends must be parked above the numbers they are dup2()'d onto, or the
 * shuffle overwrites a descriptor it has not copied yet.
 *
 * Not a gate — it needs firmware and a guest image, so it is run by hand:
 *
 *   cc -O1 -o console_mux tests/arm64emu/scripts/console_mux.c
 *   ./console_mux 3 -- build/arm64emu --jit --bios QEMU_EFI.fd --drive alpine.iso,ro
 *
 * stdin is forwarded to ttyAMA0, except lines of the form
 *   @size <dev> <cols> <rows>     send a control-channel winsize message
 *   @to <dev>                     forward following input to <dev> instead
 *
 * A session that shows the whole surface working: log in on ttyAMA0, then
 *   setsid getty -L 0 hvc0 linux &      -> its banner appears on [hvc0   ]
 *   @size hvc1 100 30
 *   stty size < /dev/hvc1               -> "30 100"
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_HVC 8
#define CTRL_UART 0xff

struct tab {
    char name[16];
    int  fd;          /* our end of the socketpair */
    int  far;         /* the emulator's end */
    int  at_bol;      /* next output starts a line -> print the prefix */
};

static struct tab tabs[1 + MAX_HVC];   /* [0] = ttyAMA0, [1..] = hvc0.. */
static int n_tabs;
static int ctrl_fd = -1;

static int dev_index(const char *name) {
    for (int i = 0; i < n_tabs; i++)
        if (!strcmp(tabs[i].name, name)) return i;
    return -1;
}

/* Print a chunk with the tab's name at the start of every line. */
static void show(struct tab *t, const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (t->at_bol) { printf("[%-7s] ", t->name); t->at_bol = 0; }
        if (p[i] == '\n') { putchar('\n'); t->at_bol = 1; }
        else if (p[i] == '\r') continue;
        else if (p[i] < 32 && p[i] != '\t') printf("\\x%02x", p[i]);
        else putchar(p[i]);
    }
    fflush(stdout);
}

static void send_winsize(const char *dev, int cols, int rows) {
    if (ctrl_fd < 0) return;
    unsigned char m[8] = {0};
    m[0] = 1;                                     /* CONSOLE_OP_WINSIZE */
    int i = dev_index(dev);
    m[1] = (i == 0) ? CTRL_UART : (unsigned char)(i - 1);
    m[2] = (unsigned char)(cols & 0xff); m[3] = (unsigned char)(cols >> 8);
    m[4] = (unsigned char)(rows & 0xff); m[5] = (unsigned char)(rows >> 8);
    if (write(ctrl_fd, m, sizeof m) != (ssize_t)sizeof m)
        fprintf(stderr, "[mux] control write failed: %s\n", strerror(errno));
    else
        fprintf(stderr, "[mux] %s -> %dx%d\n", dev, cols, rows);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: console_mux <n_hvc> -- <emu argv>\n"); return 2; }
    int n_hvc = atoi(argv[1]);
    if (n_hvc < 0 || n_hvc > MAX_HVC) { fprintf(stderr, "bad n_hvc\n"); return 2; }
    int sep = 2;
    if (strcmp(argv[sep], "--")) { fprintf(stderr, "expected --\n"); return 2; }
    sep++;

    n_tabs = 1 + n_hvc;
    for (int i = 0; i < n_tabs; i++) {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { perror("socketpair"); return 1; }
        tabs[i].fd = sv[0];
        /* Park the child's end well above the numbers we dup2 onto (3..3+n), or
         * the shuffle in the child clobbers a descriptor it has not copied yet:
         * socketpair hands out exactly those low numbers. */
        tabs[i].far = fcntl(sv[1], F_DUPFD, 20);
        close(sv[1]);
        tabs[i].at_bol = 1;
        if (i == 0) snprintf(tabs[i].name, sizeof tabs[i].name, "ttyAMA0");
        else        snprintf(tabs[i].name, sizeof tabs[i].name, "hvc%d", i - 1);
    }
    int cv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, cv) != 0) { perror("socketpair"); return 1; }
    ctrl_fd = cv[0];
    int ctrl_far = fcntl(cv[1], F_DUPFD, 20);
    close(cv[1]);

    /* Build the emulator argv: its own args, then the bindings. The far ends are
     * dup2()'d onto fixed numbers in the child so the values are predictable. */
    char *eargv[64];
    int n = 0;
    for (int i = sep; i < argc && n < 40; i++) eargv[n++] = argv[i];
    static char bind[1 + MAX_HVC][32], ctrlarg[32];
    for (int i = 0; i < n_tabs; i++) {
        snprintf(bind[i], sizeof bind[i], "%s=%d", tabs[i].name, 3 + i);
        eargv[n++] = "--console-fd";
        eargv[n++] = bind[i];
    }
    snprintf(ctrlarg, sizeof ctrlarg, "%d", 3 + n_tabs);
    eargv[n++] = "--ctrl-fd";
    eargv[n++] = ctrlarg;
    eargv[n] = NULL;

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }
    if (pid == 0) {
        for (int i = 0; i < n_tabs; i++) dup2(tabs[i].far, 3 + i);
        dup2(ctrl_far, 3 + n_tabs);
        /* Everything above the bindings is ours, not the emulator's: the parked
         * copies and our own ends of every pair. */
        for (int f = 4 + n_tabs; f < 64; f++) close(f);
        execvp(eargv[0], eargv);
        perror("exec");
        _exit(127);
    }
    for (int i = 0; i < n_tabs; i++) close(tabs[i].far);
    close(ctrl_far);

    int target = 0;                       /* tab that stdin feeds */
    char line[256];
    size_t line_len = 0;
    for (;;) {
        struct pollfd pfd[2 + MAX_HVC];
        int np = 0;
        for (int i = 0; i < n_tabs; i++) {
            pfd[np].fd = tabs[i].fd; pfd[np].events = POLLIN; pfd[np].revents = 0; np++;
        }
        pfd[np].fd = STDIN_FILENO; pfd[np].events = POLLIN; pfd[np].revents = 0; np++;

        if (poll(pfd, (nfds_t)np, 500) < 0 && errno != EINTR) break;

        for (int i = 0; i < n_tabs; i++) {
            if (!(pfd[i].revents & (POLLIN | POLLHUP))) continue;
            unsigned char buf[4096];
            ssize_t r = read(tabs[i].fd, buf, sizeof buf);
            if (r > 0) show(&tabs[i], buf, (size_t)r);
        }
        if (pfd[np - 1].revents & POLLIN) {
            char ch;
            ssize_t r = read(STDIN_FILENO, &ch, 1);
            if (r <= 0) { /* stdin done: keep running until the child exits */ }
            else if (ch == '\n' && line_len && line[0] == '@') {
                line[line_len] = '\0';
                char dev[16]; int a, b;
                if (sscanf(line, "@size %15s %d %d", dev, &a, &b) == 3) send_winsize(dev, a, b);
                else if (sscanf(line, "@to %15s", dev) == 1) {
                    int t = dev_index(dev);
                    if (t >= 0) { target = t; fprintf(stderr, "[mux] input -> %s\n", dev); }
                }
                line_len = 0;
            } else {
                if (ch == '\n') line_len = 0;
                else if (line_len < sizeof line - 1) line[line_len++] = ch;
                /* A @-line is for the harness, so it must not also reach the
                 * guest — otherwise its text lands in whatever is reading. */
                int is_cmd = line_len && line[0] == '@';
                if (!is_cmd && write(tabs[target].fd, &ch, 1) < 0) { /* ignore */ }
            }
        }

        int st;
        if (waitpid(pid, &st, WNOHANG) == pid) {
            fprintf(stderr, "[mux] emulator exited\n");
            break;
        }
    }
    return 0;
}
