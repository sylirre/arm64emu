/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* How arm64emu builds the shared AArch64 emulation (lib/arm64). Included first
 * by lib/arm64/types.h, so it reaches every unit of the library and of
 * arm64emu. */
#ifndef ARM64EMU_ARM64_CONFIG_H
#define ARM64EMU_ARM64_CONFIG_H

/* A full system: the guest runs at EL1 as well as EL0, behind the stage-1 MMU,
 * with its own exception vectors, TLB and address-translation maintenance,
 * and CPACR_EL1 traps. */
#define ARM64_SYSTEM 1

#ifdef ANDROID_JNI
/* Embedded build (see main.c): the emulator is linked into a host process and
 * entered in a fork()ed child of it, so the atexit handlers and DSO destructors
 * registered in that process — an Android runtime's, say — are not ours to run.
 * A fatal path must therefore leave through _exit(). Shadowing the name here
 * covers every call site at once, present and future, which 40-odd scattered
 * edits would not. The system headers come first so that the declarations they
 * carry are made before the name is taken. */
#include <stdlib.h>
#include <unistd.h>
#define exit(code) _exit(code)
#endif

#endif /* ARM64EMU_ARM64_CONFIG_H */
