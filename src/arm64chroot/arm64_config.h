/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* How arm64chroot builds the shared AArch64 emulation (lib/arm64). Included
 * first by lib/arm64/types.h, so it reaches every unit of the library and of
 * arm64chroot. */
#ifndef ARM64CHROOT_ARM64_CONFIG_H
#define ARM64CHROOT_ARM64_CONFIG_H

/* User mode: the guest only ever runs at EL0, and the emulator plays its
 * kernel. The EL1 state is the configuration Linux gives a process (see
 * sysreg_init), and what would trap to a kernel -- system calls, faults, the
 * ID-register MRS emulation -- is the emulator's own (loop.c, exception.c).
 * The EL1-only instructions (TLBI, AT, ...) are UNDEFINED at EL0 and are not
 * compiled in. */
#define ARM64_SYSTEM 0

/* Guest threads are host threads, running guest code at the same time over
 * one shared address space: LSE atomics, exclusives and LDAR/STLR must be host
 * atomic accesses, and DMB/DSB host fences, or another guest thread observes
 * torn or reordered memory (weakly ordered hosts reorder ordinary accesses). */
#define ARM64_SMP 1

/* The environment knobs of the shared code carry this program's prefix:
 * getenv(ARM64_JIT_ENV("PDMAX")) reads ARM64CHROOT_JIT_PDMAX. */
#define ARM64_JIT_ENV(name) "ARM64CHROOT_JIT_" name

#endif /* ARM64CHROOT_ARM64_CONFIG_H */
