/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* Guest-kernel diagnostics (diag.c), on the step driver's hooks. */
#ifndef ARM64EMU_DIAG_H
#define ARM64EMU_DIAG_H

#include "cpu.h"

extern int g_systrace;     /* env ARM64EMU_SYS: trace mm syscalls (mmap/brk/munmap/...) */
extern int g_heaptrack;    /* env ARM64EMU_HEAP: musl malloc/free/realloc double-alloc finder */
extern u64 g_heap_at;      /* env ARM64EMU_HEAP_AT: log every alloc/free touching this VA */
/* Set the step driver's hooks for whichever of the two is on (allocating the
 * heap tracker's tables); returns whether either is, which makes them
 * per-instruction debug facilities (g_debug_hooks). */
bool diag_install(void);
void heaptrack_report(void); /* print heap-tracker stats (call once at exit) */
void heaptrack_query(CPU *c, u64 va); /* report which live block (if any) contains va */

#endif /* ARM64EMU_DIAG_H */
