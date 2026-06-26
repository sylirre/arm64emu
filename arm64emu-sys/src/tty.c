/* Raw terminal handling using POSIX termios (no external deps). */
#include "tty.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <signal.h>

static struct termios saved_tio;
static int raw_active = 0;
static int stdin_flags = 0;

void tty_raw_disable(void) {
    if (!raw_active) return;
    tcsetattr(STDIN_FILENO, TCSANOW, &saved_tio);
    fcntl(STDIN_FILENO, F_SETFL, stdin_flags);
    raw_active = 0;
}

static void on_signal(int sig) {
    tty_raw_disable();
    /* Re-raise with default disposition so exit status is correct. */
    signal(sig, SIG_DFL);
    raise(sig);
}

void tty_raw_enable(void) {
    if (!isatty(STDIN_FILENO)) return;
    if (tcgetattr(STDIN_FILENO, &saved_tio) != 0) return;

    struct termios t = saved_tio;
    /* Raw: disable canonical mode, echo, signal generation, CR/LF translation.
     * ISIG off: CTRL-C/Z/\ become raw bytes forwarded to the guest instead of
     * generating SIGINT/SIGTSTP/SIGQUIT on the host. */
    t.c_lflag &= ~(ICANON | ECHO | IEXTEN | ISIG);
    t.c_iflag &= ~(IXON | ICRNL | INLCR | ISTRIP | INPCK | BRKINT);
    t.c_cflag |= CS8;
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);

    stdin_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, stdin_flags | O_NONBLOCK);

    raw_active = 1;
    atexit(tty_raw_disable);
    signal(SIGTERM, on_signal);
    signal(SIGSEGV, on_signal);
}

int tty_getchar(void) {
    static int esc_pending = 0;
    unsigned char ch;
    if (read(STDIN_FILENO, &ch, 1) != 1) return -1;
    if (esc_pending) {
        esc_pending = 0;
        if (ch == 'x' || ch == 'X') {
            static const char msg[] = "\r\n[quit]\r\n";
            if (write(STDOUT_FILENO, msg, sizeof(msg) - 1) < 0) { /* ignore */ }
            tty_raw_disable();
            exit(0);
        }
        if (ch == 0x01) return 0x01;  /* CTRL-A CTRL-A: send CTRL-A to guest */
        static const char hint[] = "\r\n[CTRL-A: x=quit, CTRL-A=send CTRL-A]\r\n";
        if (write(STDOUT_FILENO, hint, sizeof(hint) - 1) < 0) { /* ignore */ }
        return -1;
    }
    if (ch == 0x01) { esc_pending = 1; return -1; }
    return ch;
}

void tty_putchar(int ch) {
    unsigned char c = (unsigned char)ch;
    if (write(STDOUT_FILENO, &c, 1) < 0) { /* ignore */ }
}
