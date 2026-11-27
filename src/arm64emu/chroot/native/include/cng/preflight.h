/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* The native tier's preflight (preflight.c): whether it can run the guest in
 * this process at all, asked before anything is done that the emulator, which
 * runs the guest otherwise, would mind. Each check answers 0 (yes) or 1 (no,
 * with a phrase in `why` saying what stands in the way). Self-contained: the
 * unit that implements it is the libc's, and includes none of ours but this. */
#ifndef CNG_PREFLIGHT_H
#define CNG_PREFLIGHT_H

#include <stddef.h>

/* The process has one thread: the tier takes over its signal dispositions,
 * its threads and its address space. */
int cng_preflight_threads(char *why, size_t n);

/* A forked child of this process can set no_new_privs and install a seccomp
 * filter, and the kernel applies the filter -- an emulator underneath may
 * accept one and go on as if it were not there -- and guest code can be made
 * executable there: anonymous memory turned executable, the anonymous
 * strategy of the loader, or else (and with `file_backed`, -F, only) the
 * program's file `host_prog` mapped executable. Then, where `host_prog` is
 * linked at a fixed address (ET_EXEC), that its segments' place is free. */
int cng_preflight(const char *host_prog, int file_backed, char *why, size_t n);

/* The host's auxiliary vector, the entries the tier reads of it (AT_PAGESZ,
 * AT_RANDOM, the ids, AT_CLKTCK, the HWCAPs, AT_SYSINFO_EHDR, AT_MINSIGSTKSZ)
 * as a vector of its own, AT_NULL-terminated: from getauxval, which every
 * libc has, rather than from beyond the environment's terminator, which only
 * a main() that the kernel's stack still underlies could walk to -- not one
 * an app called in an embedded build. */
unsigned long *cng_preflight_auxv(void);

#endif /* CNG_PREFLIGHT_H */
