/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* include/cng/unistd.h, checked against the build host's own table.
 *
 * The binary takes its syscall numbers from our header alone (see the note
 * there), so nothing else in the tree includes <asm/unistd.h>. This unit
 * includes both, and only where the host has one to offer: every number the two
 * agree on is a silent identical redefinition, and a number they disagree on is
 * a "macro redefined" diagnostic naming it — which the Makefile turns into an
 * error for this one file. A host whose headers stop short of ours says nothing,
 * which is right: that is exactly the host our own table exists for. Compiles
 * to nothing.
 *
 * The comparison is textual, so the two tables have to agree on the spelling of
 * a number and not only on its value. Ours spells every one of them out, as
 * bionic and musl do; asm-generic/unistd.h is the exception, giving eleven of
 * them under a __NR3264_* name and defining __NR_* as aliases of those. The
 * alias is where the spellings part, and it carries nothing of its own, so it
 * is dropped below — the __NR3264_* definition it came from stays, and our
 * header's copy of that one still has to match it number for number. Dropping
 * them is conditional on that spelling actually being the host's, so on bionic
 * (no __NR3264_* at all, plain numbers) the host's own __NR_fcntl is the thing
 * ours is checked against, not something this file quietly undefined first.
 */
#if defined(__has_include)
#if __has_include(<asm/unistd.h>)
#include <asm/unistd.h>
#endif
#endif

#ifdef __NR3264_fcntl
#undef __NR_fcntl
#undef __NR_statfs
#undef __NR_fstatfs
#undef __NR_truncate
#undef __NR_ftruncate
#undef __NR_lseek
#undef __NR_sendfile
#undef __NR_newfstatat
#undef __NR_fstat
#undef __NR_mmap
#undef __NR_fadvise64
#endif

#include "cng/unistd.h"

/* And our own two spellings of those eleven have to agree with each other. On
 * an asm-generic host it is the __NR3264_* copy that meets the host's number,
 * while every caller in the tree writes __NR_*, so a typo in one of the two
 * would leave the check guarding a number nothing uses. These compare macros
 * that are both ours, so they hold on every host, with or without headers.
 */
_Static_assert(__NR_fcntl == __NR3264_fcntl, "__NR_fcntl");
_Static_assert(__NR_statfs == __NR3264_statfs, "__NR_statfs");
_Static_assert(__NR_fstatfs == __NR3264_fstatfs, "__NR_fstatfs");
_Static_assert(__NR_truncate == __NR3264_truncate, "__NR_truncate");
_Static_assert(__NR_ftruncate == __NR3264_ftruncate, "__NR_ftruncate");
_Static_assert(__NR_lseek == __NR3264_lseek, "__NR_lseek");
_Static_assert(__NR_sendfile == __NR3264_sendfile, "__NR_sendfile");
_Static_assert(__NR_newfstatat == __NR3264_fstatat, "__NR_newfstatat");
_Static_assert(__NR_fstat == __NR3264_fstat, "__NR_fstat");
_Static_assert(__NR_mmap == __NR3264_mmap, "__NR_mmap");
_Static_assert(__NR_fadvise64 == __NR3264_fadvise64, "__NR_fadvise64");
