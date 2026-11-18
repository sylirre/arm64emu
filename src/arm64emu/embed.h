/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* The embedded build (-DANDROID_JNI, see main.c), forced into every unit of
 * it by the Makefile (-include). The emulator is linked into a host process
 * and entered in a fork()ed child of it, so the atexit handlers and DSO
 * destructors registered in that process -- an Android runtime's, say -- are
 * not ours to run. A fatal path must therefore leave through _exit().
 * Shadowing the name here covers every call site at once, present and
 * future, which 40-odd scattered edits would not. The system headers come
 * first so that the declarations they carry are made before the name is
 * taken. lib/arm64 never leaves the process itself: a fatal condition there
 * is the program's to act on. */
#ifndef ARM64EMU_EMBED_H
#define ARM64EMU_EMBED_H

#include <stdlib.h>
#include <unistd.h>
#define exit(code) _exit(code)

#endif /* ARM64EMU_EMBED_H */
