/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* How arm64chroot builds the shared AArch64 emulation (lib/arm64). Included
 * first by lib/arm64/types.h, so it reaches every unit of the library and of
 * arm64chroot. */
#ifndef ARM64CHROOT_ARM64_CONFIG_H
#define ARM64CHROOT_ARM64_CONFIG_H

/* The environment knobs of the shared code carry this program's prefix:
 * getenv(ARM64_ENV("PD_MAX")) reads ARM64CHROOT_PD_MAX, and the JIT's,
 * getenv(ARM64_JIT_ENV("PDMAX")), ARM64CHROOT_JIT_PDMAX. */
#define ARM64_ENV(name)     "ARM64CHROOT_" name
#define ARM64_JIT_ENV(name) ARM64_ENV("JIT_" name)

#endif /* ARM64CHROOT_ARM64_CONFIG_H */
