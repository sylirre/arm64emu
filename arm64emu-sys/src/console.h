/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* Host-side endpoints for the guest's console devices.
 *
 * Every guest console — the PL011 UART, each virtio-console — has to reach some
 * host byte stream. That used to be the process terminal by construction: both
 * devices called into tty.c directly, so every console in a run shared one
 * stdin/stdout. Fine for a single console on a terminal, and exactly what a run
 * with several consoles cannot do. A ConsoleEP names one host endpoint; each
 * device holds a pointer to the one it speaks through.
 *
 * Two kinds exist:
 *
 *   stdio  the process terminal, backed by tty.c (raw mode, the CTRL-A escape,
 *          geometry from TIOCGWINSZ and SIGWINCH). A singleton: devices left
 *          unbound all share it, which is what keeps the "input follows output"
 *          arbitration in machine_tick meaningful.
 *
 *   fd     a plain descriptor pair — a socketpair from an embedding process, a
 *          pipe, a PTY. Geometry is pushed in from outside instead of read from
 *          a terminal, and output is buffered rather than blocking: a console
 *          whose host side has stopped reading must never stall the guest.
 */
#ifndef A64_CONSOLE_H
#define A64_CONSOLE_H

#include "types.h"
#include <stddef.h>

typedef struct ConsoleEP ConsoleEP;

/* The process terminal. Never NULL, never freed; repeated calls return the same
 * endpoint, so devices that share it really do share one stdin. */
ConsoleEP *console_stdio(void);

/* An endpoint on inherited descriptors. Pass the same fd for both when the host
 * side is one bidirectional channel (a socketpair or a PTY). Both are set
 * non-blocking. Returns NULL if the descriptors are unusable. */
ConsoleEP *console_open_fd(int fd_in, int fd_out);

/* Release an fd endpoint and close its descriptors. No-op for stdio. */
void console_close(ConsoleEP *e);

/* True for the shared process-terminal endpoint. */
bool console_is_stdio(const ConsoleEP *e);

/* One byte of host input, or -1 when none is available right now. Never blocks. */
int console_getchar(ConsoleEP *e);

/* Guest console output. Bulk (a whole descriptor's worth) and single-byte forms;
 * both are non-blocking on an fd endpoint, where bytes that cannot be written
 * yet are buffered and drained by console_flush. */
void console_write(ConsoleEP *e, const void *buf, size_t len);
void console_putchar(ConsoleEP *e, int ch);

/* Push out whatever console_write could not hand to the kernel earlier. Cheap
 * and safe to call on every tick; a no-op for stdio and for an empty buffer. */
void console_flush(ConsoleEP *e);

/* Descriptor to wait on for host input, or -1 when this endpoint can never
 * produce any (stdio that is not a terminal, a closed peer). */
int console_poll_fd(const ConsoleEP *e);

/* Terminal geometry, 0x0 when unknown — the "no geometry" state a serial line
 * reports. For stdio this is the host window; for an fd endpoint it is whatever
 * console_set_winsize last pushed in. */
void console_get_winsize(ConsoleEP *e, u16 *cols, u16 *rows);
void console_set_winsize(ConsoleEP *e, u16 cols, u16 rows);

/* True once (clearing the flag) if the geometry changed since the last call:
 * SIGWINCH for stdio, a console_set_winsize for an fd endpoint. */
bool console_take_winch(ConsoleEP *e);

#endif /* A64_CONSOLE_H */
