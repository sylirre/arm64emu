/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
#include "cng/rt.h"
#include "cng/syscall.h"

/* strlcpy, which not every libc has: the length of `src`, as much of it as
 * fits copied with the terminator whenever `size` is not zero. */
size_t cng_strlcpy(char *dst, const char *src, size_t size) {
    size_t n = strlen(src);
    if (size) {
        size_t c = n < size - 1 ? n : size - 1;
        memcpy(dst, src, c);
        dst[c] = '\0';
    }
    return n;
}

/* ---- I/O -------------------------------------------------------------- */

long cng_write_all(int fd, const void *buf, size_t n) {
    const char *p = buf;
    size_t left = n;
    while (left) {
        long r = sys_write(fd, p, left);
        if (r < 0) {
            if (r == -EINTR)
                continue;
            return r;
        }
        if (r == 0)
            break;
        p += r;
        left -= (size_t)r;
    }
    return (long)(n - left);
}

void cng_puts(int fd, const char *s) { cng_write_all(fd, s, strlen(s)); }

/* ---- minimal printf --------------------------------------------------- */

/* One formatting run.
 *
 * `buf`/`cap` is where characters land, but when `fd` is >= 0 it is a *window*
 * rather than the destination: it is written out and reused each time it fills,
 * so a line longer than the buffer is delivered whole. That matters because the
 * lines this formats are not log lines — put_maps copies a mapping's path
 * through, put_mounts prints a bind's guest and host path on one row — and
 * cutting one at the window's width takes its trailing newline with it, running
 * two rows of a synthesized /proc file together.
 *
 * With `fd` < 0 nothing is flushed and the overflow is only counted, which is
 * what gives cng_snprintf the C return value: the length the format would have
 * produced, so a caller can tell that it did not fit. */
struct cng_fmt {
    char *buf;
    size_t cap;   /* bytes in buf, one of them reserved for a NUL */
    size_t len;   /* bytes held right now */
    size_t total; /* bytes the whole format produces */
    int fd;       /* >= 0: flush there when the window fills */
};

static void fmt_flush(struct cng_fmt *f) {
    if (f->len) {
        cng_write_all(f->fd, f->buf, f->len);
        f->len = 0;
    }
}

static void fmt_put(struct cng_fmt *f, char c) {
    f->total++;
    if (f->len + 1 >= f->cap) {
        if (f->fd < 0)
            return; /* snprintf: drop the byte, keep counting */
        fmt_flush(f);
    }
    f->buf[f->len++] = c;
}

/* A function call, so the argument is evaluated exactly once whether or not
 * there is room for it. Every caller below hands this an expression with a side
 * effect (`*s++`, `t[--i]`, `va_arg(...)`). */
static void fmt_run(struct cng_fmt *o, const char *fmt, va_list ap) {
#define PUT(ch) fmt_put(o, (char)(ch))

    for (const char *f = fmt; *f; f++) {
        if (*f != '%') {
            PUT(*f);
            continue;
        }
        f++;
        /* Flags/width: "0" (zero-pad) then a decimal field width. Needed by the
         * /proc synthesis, whose formats are fixed-width ("%02lu" in loadavg,
         * "%08llx" in maps) — a real reader parses them by column. */
        int zero = 0, width = 0;
        while (*f == '0') {
            zero = 1;
            f++;
        }
        while (*f >= '0' && *f <= '9') {
            width = width * 10 + (*f - '0');
            f++;
        }
        int lng = 0;
        while (*f == 'l') {
            lng++;
            f++;
        }
        if (*f == 'z') {
            lng = 2;
            f++;
        }
#define PAD(have)                                                              \
    do {                                                                       \
        for (int p_ = (have); p_ < width; p_++)                                \
            PUT(zero ? '0' : ' ');                                             \
    } while (0)
        /* The conversion specifier can be the terminator: stepping past the '%'
         * is unconditional and every scan above stops at '\0'. Falling into the
         * default arm below put that NUL in the output as a character and then
         * let the loop's own f++ walk past the end of the string, after which
         * this went on formatting whatever followed the literal in memory,
         * consuming a va_arg for every '%' it found there — an out-of-bounds
         * read in the one formatter every path shares, the SIGSYS handler's
         * included, where a fault is an unblockable kill.
         *
         * glibc calls an incomplete conversion an error and returns -1; this
         * returns a length and has nowhere to say that, so it keeps the stray
         * '%' (dropping nothing) and stops. */
        if (!*f) {
            PUT('%');
            break;
        }
        switch (*f) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            size_t sl = strlen(s);
            PAD((int)sl);
            while (*s)
                PUT(*s++);
            break;
        }
        case 'c':
            PUT((char)va_arg(ap, int));
            break;
        case '%':
            PUT('%');
            break;
        case 'd': {
            long v = lng ? va_arg(ap, long) : (long)va_arg(ap, int);
            unsigned long uv;
            int neg = 0;
            if (v < 0) {
                neg = 1;
                uv = (unsigned long)(-v);
            } else {
                uv = (unsigned long)v;
            }
            char t[24];
            int i = 0;
            if (uv == 0)
                t[i++] = '0';
            while (uv) {
                t[i++] = (char)('0' + uv % 10);
                uv /= 10;
            }
            if (zero) { /* sign first, then the zero fill: "-007" */
                if (neg)
                    PUT('-');
                PAD(i + neg);
            } else {
                PAD(i + neg);
                if (neg)
                    PUT('-');
            }
            while (i)
                PUT(t[--i]);
            break;
        }
        case 'u':
        case 'o':
        case 'x':
        case 'X':
        case 'p': {
            unsigned long uv;
            unsigned base = 10;
            const char *dig = "0123456789abcdef";
            if (*f == 'p') {
                PUT('0');
                PUT('x');
                uv = (unsigned long)va_arg(ap, void *);
                base = 16;
            } else {
                uv = lng ? va_arg(ap, unsigned long)
                         : (unsigned long)va_arg(ap, unsigned int);
                if (*f == 'o')
                    base = 8;
                else if (*f == 'x')
                    base = 16;
                else if (*f == 'X') {
                    base = 16;
                    dig = "0123456789ABCDEF";
                }
            }
            char t[24];
            int i = 0;
            if (uv == 0)
                t[i++] = '0';
            while (uv) {
                t[i++] = dig[uv % base];
                uv /= base;
            }
            PAD(i); /* %p pads its digits, after the "0x" */
            while (i)
                PUT(t[--i]);
            break;
        }
        default:
            PUT('%');
            PUT(*f);
            break;
        }
#undef PAD
    }
#undef PUT
}

/* Writes at most `cap - 1` bytes into `buf` plus a NUL, and returns the length
 * the format would have produced — so `>= cap` means it did not all fit. */
size_t cng_vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap) {
    struct cng_fmt f = {buf, cap, 0, 0, -1};
    fmt_run(&f, fmt, ap);
    if (cap)
        buf[f.len] = '\0';
    return f.total;
}

size_t cng_snprintf(char *buf, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    size_t n = cng_vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return n;
}

/* No length limit: the window below is refilled as often as the format needs. */
void cng_vdprintf(int fd, const char *fmt, va_list ap) {
    char buf[1024];
    struct cng_fmt f = {buf, sizeof buf, 0, 0, fd};
    fmt_run(&f, fmt, ap);
    fmt_flush(&f);
}

void cng_dprintf(int fd, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    cng_vdprintf(fd, fmt, ap);
    va_end(ap);
}

_Noreturn void cng_die(const char *msg, long err) {
    if (err)
        cng_dprintf(2, "arm64emu: %s: errno %d\n", msg, (int)(err < 0 ? -err : err));
    else
        cng_dprintf(2, "arm64emu: %s\n", msg);
    sys_exit_group(1);
}

/* ---- the monitor's own image ------------------------------------------ */

/* [addr, addr+len) against the image [cng_image_lo, cng_image_hi). `len` is
 * guest-supplied at both call sites, so a wrapping sum is a real input: treat
 * it as reaching the top of the address space rather than wrapping round to a
 * range that happens to miss us. */
int cng_hits_image(unsigned long addr, unsigned long len) {
    unsigned long lo = cng_image_lo();
    unsigned long hi = cng_image_hi();
    if (!len)
        return 0; /* an empty range covers nothing */
    unsigned long end = addr + len;
    if (end < addr)
        end = ~0UL;
    return addr < hi && end > lo;
}

/* ---- process bootstrap ------------------------------------------------ */

/* Actual page size, set from the host's AT_PAGESZ at the tier's entry
 * (run.c; declared in loader.h). Some Android devices use 16 KiB pages, so
 * never assume 4096. */
unsigned long cng_page_size = 4096;

/* The thread pointer the libc gave the main thread (run.c records it). */
unsigned long cng_host_tp;

/* The stack protector's canary is a global (-mstack-protector-guard=global,
 * see the Makefile) in the native tier's code and in lib/link2symlink, which
 * it runs: a dispatch bounced to guest context runs on the guest's thread
 * pointer, where Bionic's own place for it, a TLS slot, is whatever the guest
 * keeps there. glibc and musl export one, which their startup has set before
 * any constructor runs; Bionic exports one only for its 32-bit ABIs, so a
 * Bionic build has its own, set here from AT_RANDOM before main -- and before
 * any function that checks it is entered, whichever of chroot mode's tiers
 * the process goes on to run. */
#ifdef __BIONIC__
#include <sys/auxv.h>
uintptr_t __stack_chk_guard __attribute__((visibility("hidden")));

__attribute__((constructor, no_stack_protector))
static void cng_stack_guard_seed(void) {
    const uintptr_t *r = (const uintptr_t *)getauxval(25 /* AT_RANDOM */);
    if (r)
        __stack_chk_guard = r[1];
}
#endif
