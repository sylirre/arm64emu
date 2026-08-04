/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* Host console endpoints: the process terminal, or an inherited fd pair.
 * See console.h for what an endpoint is and why the devices go through one. */
#include "console.h"
#include "tty.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Input is refilled a chunk at a time rather than a byte per read(2). Sized well
 * under a device RX FIFO so surplus keystrokes stay in the kernel's buffer and
 * the flow control the devices already implement (only pull when there is room)
 * still reaches the host. */
#define IN_CHUNK   256

/* Output that the host side is not draining. A tab whose reader has gone away
 * (an unattached console) must not be able to block the guest, so writes never
 * block: they land here and are pushed out on later ticks. Full means the peer
 * has ignored 64 KB, at which point further output is dropped — deliberately the
 * newest bytes, so what does get through stays a valid prefix of the stream
 * rather than a mid-escape-sequence splice. */
#define OUT_CAP    (64 * 1024)

struct ConsoleEP {
    int  fd_in, fd_out;         /* -1 once the peer is gone */
    bool stdio;                 /* the process terminal (tty.c) */

    u8   in[IN_CHUNK];
    int  in_pos, in_len;

    u8   *out;                  /* lazily allocated: an endpoint that keeps up
                                 * never buffers, so most never allocate */
    size_t out_pos, out_len;    /* pending bytes are out[out_pos .. out_len) */
    bool  out_dropped;          /* reported once, so a stuck peer can't spam */

    u16  cols, rows;
    bool winch;
};

/* ---- the process terminal ---- */

static ConsoleEP stdio_ep = { .fd_in = STDIN_FILENO, .fd_out = STDOUT_FILENO,
                              .stdio = true };

ConsoleEP *console_stdio(void) { return &stdio_ep; }

bool console_is_stdio(const ConsoleEP *e) { return e && e->stdio; }

/* ---- fd endpoints ---- */

static void set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

ConsoleEP *console_open_fd(int fd_in, int fd_out) {
    if (fd_in < 0 && fd_out < 0) return NULL;
    ConsoleEP *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->fd_in = fd_in;
    e->fd_out = fd_out;
    if (fd_in >= 0) set_nonblock(fd_in);
    if (fd_out >= 0 && fd_out != fd_in) set_nonblock(fd_out);
    return e;
}

void console_close(ConsoleEP *e) {
    if (!e || e->stdio) return;
    if (e->fd_in >= 0) close(e->fd_in);
    if (e->fd_out >= 0 && e->fd_out != e->fd_in) close(e->fd_out);
    free(e->out);
    free(e);
}

/* ---- input ---- */

int console_getchar(ConsoleEP *e) {
    if (!e) return -1;
    if (e->stdio) return tty_getchar();
    if (e->in_pos < e->in_len) return e->in[e->in_pos++];
    if (e->fd_in < 0) return -1;
    ssize_t n;
    do { n = read(e->fd_in, e->in, sizeof e->in); } while (n < 0 && errno == EINTR);
    if (n <= 0) {
        /* 0 is the peer closing its end; EAGAIN just means nothing right now.
         * Anything else is a broken descriptor. Only the first two are normal,
         * and only EAGAIN leaves the endpoint usable. */
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK))
            e->fd_in = -1;
        return -1;
    }
    e->in_pos = 1;
    e->in_len = (int)n;
    return e->in[0];
}

int console_poll_fd(const ConsoleEP *e) {
    if (!e) return -1;
    /* A non-tty stdin (a pipe from a test harness, /dev/null) is not a wake
     * source the run loop should park on — machine_wait_for_event has always
     * treated it that way. */
    if (e->stdio) return isatty(STDIN_FILENO) ? STDIN_FILENO : -1;
    return e->fd_in;
}

/* ---- output ---- */

/* Hand as much of the pending buffer to the kernel as it will take. */
static void drain(ConsoleEP *e) {
    while (e->out_pos < e->out_len) {
        ssize_t n = write(e->fd_out, e->out + e->out_pos, e->out_len - e->out_pos);
        if (n > 0) { e->out_pos += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;  /* later */
        e->fd_out = -1;                      /* peer gone: stop trying */
        break;
    }
    e->out_pos = e->out_len = 0;             /* empty (or abandoned): reset */
}

static void buffer(ConsoleEP *e, const u8 *p, size_t len) {
    if (e->fd_out < 0) return;               /* drain() just lost the peer */
    if (!e->out) {
        e->out = malloc(OUT_CAP);
        if (!e->out) return;                 /* no buffer, no output: drop */
    }
    if (e->out_pos > 0) {                    /* reclaim the drained prefix */
        memmove(e->out, e->out + e->out_pos, e->out_len - e->out_pos);
        e->out_len -= e->out_pos;
        e->out_pos = 0;
    }
    size_t room = OUT_CAP - e->out_len;
    if (len > room) {
        len = room;
        if (!e->out_dropped) {
            e->out_dropped = true;
            fprintf(stderr, "[console] host side is not reading; output dropped\n");
        }
    }
    memcpy(e->out + e->out_len, p, len);
    e->out_len += len;
}

void console_write(ConsoleEP *e, const void *buf, size_t len) {
    if (!e || !len) return;
    if (e->stdio) { tty_write(buf, len); return; }
    if (e->fd_out < 0) return;
    const u8 *p = buf;
    /* Anything already queued must go first, or the stream reorders. */
    if (e->out_pos < e->out_len) { drain(e); }
    if (e->out_pos < e->out_len) { buffer(e, p, len); return; }
    while (len) {
        ssize_t n = write(e->fd_out, p, len);
        if (n > 0) { p += n; len -= (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { buffer(e, p, len); return; }
        e->fd_out = -1;
        return;
    }
}

void console_putchar(ConsoleEP *e, int ch) {
    u8 c = (u8)ch;
    if (e && e->stdio) { tty_putchar(ch); return; }
    console_write(e, &c, 1);
}

void console_flush(ConsoleEP *e) {
    if (!e || e->stdio || e->fd_out < 0) return;
    if (e->out_pos < e->out_len) drain(e);
}

/* ---- geometry ---- */

void console_get_winsize(ConsoleEP *e, u16 *cols, u16 *rows) {
    if (!e) { *cols = *rows = 0; return; }
    if (e->stdio) {
        unsigned short c = 0, r = 0;
        tty_get_winsize(&c, &r);
        *cols = (u16)c;
        *rows = (u16)r;
        return;
    }
    *cols = e->cols;
    *rows = e->rows;
}

void console_set_winsize(ConsoleEP *e, u16 cols, u16 rows) {
    if (!e || e->stdio) return;              /* stdio's geometry is the window's */
    if (e->cols == cols && e->rows == rows) return;
    e->cols = cols;
    e->rows = rows;
    e->winch = true;
}

bool console_take_winch(ConsoleEP *e) {
    if (!e) return false;
    if (e->stdio) return tty_take_winch() != 0;
    bool w = e->winch;
    e->winch = false;
    return w;
}
