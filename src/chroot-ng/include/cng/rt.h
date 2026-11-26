/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* The runtime the monitor has of its own: formatted output, the bounds of its
 * image. Its mem/str are the libc's; everything else of the libc's it leaves
 * alone, and makes its syscalls through the gate (rt/gate.S) rather than
 * through the libc's wrappers, which an Android app process can have
 * interposed (libsigchain's sigaction).
 */
#ifndef CNG_RT_H
#define CNG_RT_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

size_t cng_strlcpy(char *dst, const char *src, size_t size);

/* I/O helpers */
long cng_write_all(int fd, const void *buf, size_t n);
void cng_puts(int fd, const char *s);

/* Minimal printf: %s %c %d %u %o %x %X %p %% ; length modifiers l, ll, z;
 * a leading "0" flag and a decimal field width ("%08lx", "%02u"). */
void cng_dprintf(int fd, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void cng_vdprintf(int fd, const char *fmt, va_list ap);

/* Same formatter into a buffer (NUL-terminated, truncating). Returns the byte
 * count written, excluding the terminator. */
size_t cng_snprintf(char *buf, size_t cap, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
size_t cng_vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);

/* Print "<msg>: <errno-name-or-number>\n" to stderr and exit(1). */
_Noreturn void cng_die(const char *msg, long err);

/* AArch64 cache maintenance (src/chroot-ng/rt/cache.S): make [start,end) executable-
 * coherent after writing code into it. */
void cng_flush_icache(void *start, void *end);

/* The bounds of chroot-ng's own image, wherever the loader put it: the ELF
 * header the linker places at its start and the end of its data, both symbols
 * every linker defines for a program. The guest runs in this address space, so
 * a mapping chroot-ng makes at an address the *guest* chose can land on the
 * monitor itself — which is not a failure the guest gets to see, because there
 * is no monitor left to report it. Both such places (an ET_EXEC's link-time
 * vaddr going down MAP_FIXED, and shmat(SHM_REMAP)) ask cng_hits_image() first
 * and refuse instead. */
extern const char __ehdr_start[] __attribute__((visibility("hidden")));
extern char _end[] __attribute__((visibility("hidden")));
static inline unsigned long cng_image_lo(void) { return (unsigned long)__ehdr_start; }
static inline unsigned long cng_image_hi(void) { return (unsigned long)_end; }

/* Does [addr, addr+len) overlap the image? A length that wraps past the top of
 * the address space counts as reaching everything above `addr`, since that is
 * what mapping it would have to cover. */
int cng_hits_image(unsigned long addr, unsigned long len);

/* The thread pointer the libc gave the thread main() runs on (in a fork child,
 * the one thread's). A handler of ours switches to the host context of the
 * thread it lands on (monitor/hentry.S, monitor/thread.c): a guest thread of
 * ours has its pthread's, any other thread this one. A guest thread's own is
 * the guest's to set, and whatever it is -- 0, a TCB of the guest's libc,
 * Go's -- no code of ours runs on it but what the gate and the bounce run in
 * guest context. */
extern unsigned long cng_host_tp __attribute__((visibility("hidden")));

/* The host's executable segments (rt/hostcode.c): up to `max` [lo, hi) pairs
 * into `out`, the vDSO at `vdso` left out. Returns how many. */
int cng_host_code(unsigned long (*out)[2], int max, unsigned long vdso);

#endif /* CNG_RT_H */
