/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* link2symlink: emulate hardlinks with tracked symlinks + a backing file, for
 * hosts that refuse link(2) (Android/SELinux returns EACCES/EXDEV, some EPERM).
 * The scheme and its engine are lib/link2symlink (link2symlink.h, included
 * below); this is chroot-ng's side of it (monitor/l2s.c): the host the engine
 * runs on, and the answers it leaves to the program — stat/statx of a name of
 * a group in the AArch64 layouts, a listing's record, the guest paths that
 * name the emulation's own files.
 *
 * All paths here are already rootfs-resolved *host* paths. Freestanding: raw
 * syscalls only, safe to call from the SIGSYS handler. Triggered as the linkat
 * fallback, and only when opted into with -l/--link2symlink.
 */
#ifndef CNG_L2S_H
#define CNG_L2S_H

#include <stddef.h>

#include "link2symlink.h"

/* -l/--link2symlink: opt in to the emulation. Off by default — a host that
 * refuses link(2) then reports that refusal to the guest verbatim, rather than
 * silently substituting symlinks + hidden backing files for its hardlinks. */
extern int cng_g_l2s;

/* CHROOTNG_L2S_FORCE=1 in the environment: route every linkat through the emulation
 * without trying the real hardlink first. Test aid for hosts whose filesystem
 * happily allows link(2) (mirrors arm64chroot's A64_L2S_FORCE). */
extern int cng_g_l2s_force;

/* If `host` resolves to a backing file (via our symlink, or is the data file
 * itself), stat/statx it into `buf` (a real regular file) with st_nlink set to
 * the live count. statx honors the guest's mask and (follow-forced) flags and
 * advertises STATX_NLINK. Returns 1 (filled), 0 (not ours), or -errno. */
int cng_l2s_stat(const char *host, void *statbuf);
int cng_l2s_statx(const char *host, void *statxbuf, unsigned mask,
                  unsigned flags);

/* fd variants: if the fd names a backing file, correct st_nlink (and for
 * statx, advertise STATX_NLINK) in the buffer. */
void cng_l2s_fix_fd(long fd, void *statbuf);
void cng_l2s_fix_fd_statx(long fd, void *statxbuf);

/* Listing side of the emulation: is the entry `name` of the directory open
 * on `dirfd` one of our links? If so fill *ino and *type (a DT_* value) with
 * what stat(2) of that name answers — the data file's — since the kernel's
 * record describes the symlink (DT_LNK, the link's own inode) and a guest that
 * trusts d_type/d_ino (GNU ls -F, find -type f, ls -i) would see through the
 * emulation. Returns 1 (ours, filled) or 0 (leave the record as it is). */
int cng_l2s_dirent(long dirfd, const char *name, unsigned long long *ino,
                   unsigned *type);

/* Record the view chroot-ng was started with (run.c's, never written after
 * it is published). A guest chroot narrows the view; a group linked before
 * it keeps its data where the wider view put it, and is still recognized
 * there. Only a link's target can name such a place — see l2s.c for which
 * targets are the emulation's own. */
struct cng_fs;
void cng_l2s_home(const struct cng_fs *fs);

/* 1 if the guest-supplied (dirfd, path) names l2s machinery — a data/marker
 * basename anywhere, or the "/.l2s" store dir — which must appear not to
 * exist (callers return -ENOENT before any resolution). */
int cng_l2s_deny(long dirfd, const char *gp);

#endif /* CNG_L2S_H */
