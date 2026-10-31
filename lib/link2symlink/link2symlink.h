/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* link2symlink: emulate hardlinks with tracked symlinks + a backing file, for
 * hosts that refuse link(2) (Android/SELinux returns EACCES/EXDEV, some EPERM).
 * Shared by chroot-ng and arm64chroot; each compiles it with its own flags and
 * hands it the host through struct l2s_ops (below), as `l2s_host`.
 *
 * A guest hardlink group lives in the per-rootfs object store "<rootfs>/.l2s":
 *   data   ".l2s.<ino>"          the linked file itself, moved here — a regular
 *                                file, or the FIFO, socket or device node that
 *                                was linked; every "hardlink" name
 *                                is a symlink carrying the data file's absolute
 *                                *host* path, so links span directories and
 *                                survive renames of names or parent dirs. <ino>
 *                                is the host inode of the original file (kept
 *                                by the rename into the store), giving each
 *                                group a stable unique name and pinning the
 *                                inode number against reuse.
 *   marker ".l2s.<ino>.<count>"  an empty file whose name encodes the live link
 *                                count; renamed on every count change, deleted
 *                                with the data on the last unlink.
 * When the store is unusable or the source sits on another filesystem (bind
 * mount: rename returns EXDEV), the group falls back to the pre-store layout —
 * data + marker in the first-linked name's own directory, names as same-
 * directory *relative* symlinks. That legacy format (also what arm64chroot
 * wrote before it used this engine) is still recognized everywhere; absolute
 * targets whose rootfs prefix went stale (the tree was moved or copied)
 * self-heal onto the current store. A link is ours only where its target names
 * a data file the emulation could have written — in a place of the guest's
 * own, or a stale store path — and the guest is not to write a target in the
 * ".l2s." grammar itself (the program refuses its symlinkat: see l2s_hidden).
 *
 * stat/statx on any of the names is the data file's, with st_nlink the marker
 * count, so the group presents as ordinary files sharing one inode (matching
 * st_ino, shared contents) — which is what lets programs that set+verify
 * metadata (e.g. apk preserving mtime) succeed. The engine says which file and
 * which count (l2s_target); the program makes the stat call, in its own host's
 * struct stat. A directory listing says the same: the names' dirents carry the
 * data file's d_type/d_ino rather than the symlinks' own.
 *
 * The groups proot's own link2symlink made — a rootfs installed by
 * proot-distro holds them — are read and kept as well: every name a symlink to
 * the HOST path of an indirection symlink ".l2s.<name><NNNN>" (in proot's l2s
 * directory, proot-distro's being this same "<rootfs>/.l2s", or beside the
 * first name), which names the data file beside it, "<indirection>.<CCCC>",
 * CCCC the live count. l2s_resolve presents a member as the data file with
 * that count, so every caller treats it as one of ours; a link or an unlink
 * renames the data file to the new count and repoints the indirection, as
 * proot does, so the tree goes on working under proot. New groups are made in
 * the scheme above, which proot does not read (l2s.c, "proot's groups").
 *
 * All paths here are already rootfs-resolved *host* paths, and every one the
 * engine hands back to the host goes through the program's l2s_ops, which keep
 * it inside the guest's places. Freestanding: no C library beyond the string
 * functions both programs have, no state but the ops and two flags, so it is
 * safe wherever the program's ops are (chroot-ng: the SIGSYS handler).
 */
#ifndef LINK2SYMLINK_H
#define LINK2SYMLINK_H

#include <stddef.h>

/* A file, as much of it as the engine asks about. */
struct l2s_stat {
    unsigned mode;          /* st_mode: the type and the permission bits */
    unsigned long long ino; /* st_ino */
    long long atime_sec, mtime_sec;
    long atime_nsec, mtime_nsec;
};

/* What l2s_ops.open is asked to open; every one close-on-exec. */
enum l2s_open {
    L2S_OPEN_DIR,    /* O_RDONLY | O_DIRECTORY: a directory to lock */
    L2S_OPEN_READ,   /* O_RDONLY: a data file to lock */
    L2S_OPEN_WRITE,  /* O_WRONLY: the same, where it cannot be read */
    L2S_OPEN_SOURCE, /* O_RDONLY | O_NONBLOCK | O_NOCTTY: a copy's source */
    L2S_OPEN_TOUCH,  /* O_WRONLY | O_CREAT, with `mode`: a marker */
    L2S_OPEN_NEW,    /* O_WRONLY | O_CREAT | O_EXCL, with `mode`: a copy */
};

/* l2s_ops.rename's flags: the kernel's RENAME_* values. */
#define L2S_RENAME_NOREPLACE 1u
#define L2S_RENAME_EXCHANGE  2u

/* l2s_ops.flock's operations: the kernel's LOCK_* values. */
#define L2S_LOCK_EX 2
#define L2S_LOCK_UN 8

/* The host, as the program gives it. Every call answers what the syscall
 * would — 0 (a descriptor, a length) or -errno, the kernel's numbering.
 *
 * Path calls take a host path the engine built from one the program gave it,
 * or read out of a link's text. The program keeps each inside the places of
 * the guest's own (the rootfs, its binds), as it keeps the guest's own calls,
 * and does not follow the last component — except `stat`, which follows it:
 * the one path that is meant to be followed is a descriptor's magic link
 * (/proc/self/fd/N, an O_TMPFILE being published). A path under the host's
 * /proc is such a link and is handed over as it is. */
struct l2s_ops {
    long (*lstat)(const char *path, struct l2s_stat *st);
    long (*stat)(const char *path, struct l2s_stat *st);
    long (*readlink)(const char *path, char *buf, size_t n);
    long (*symlink)(const char *target, const char *path);
    /* L2S_RENAME_* flags; -ENOSYS where the host cannot be asked for them. */
    long (*rename)(const char *from, const char *to, unsigned flags);
    long (*unlink)(const char *path);
    long (*mkdir)(const char *path, unsigned mode);
    long (*open)(const char *path, enum l2s_open how, unsigned mode);
    /* Each name in the directory `dir`, to `each` until that returns nonzero.
     * 0 or -errno (the directory could not be read). */
    long (*scan)(const char *dir, int (*each)(void *arg, const char *name),
                 void *arg);
    /* The access and modification times of `times`, set on the symlink
     * `path` itself. Best effort. */
    long (*utimes)(const char *path, const struct l2s_stat *times);

    /* Calls on a descriptor `open` returned. */
    long (*fstat)(long fd, struct l2s_stat *st);
    long (*read)(long fd, void *buf, size_t n);
    long (*write)(long fd, const void *buf, size_t n);
    long (*fchmod)(long fd, unsigned mode);
    long (*flock)(long fd, int op); /* L2S_LOCK_*; retried on EINTR here */
    void (*close)(long fd);

    /* An update of a group runs between these, under its lock: what the
     * program must hold off meanwhile (chroot-ng blocks every signal, since
     * a handler that linked a name would wait for its own lock). `saved` is
     * the program's to keep what it puts back. */
    void (*hold)(unsigned long long *saved);
    void (*release)(const unsigned long long *saved);
    /* Let another thread run: a reader that found a group mid-change looks
     * again after it. */
    void (*yield)(void);

    /* The view. `rootfs`: the rootfs's host path, "" where it is the host's
     * root; returns its length (sz or more: did not fit). `owned`: is the
     * canonical host path `path` in a place of the guest's own — the rootfs,
     * a bind, any place the view the program was started with named.
     * `guest`: the guest's name for the host path `path` (a place the view
     * names now), into `out`: 0 or -1. `resolve`: the guest path `gpath`
     * resolved as the guest's own walk would, its last component not
     * followed, into `out`: 0 or -errno. */
    size_t (*rootfs)(char *out, size_t sz);
    int (*owned)(const char *path);
    int (*guest)(const char *path, char *out, size_t sz);
    int (*resolve)(const char *gpath, char *out, size_t sz);

    /* Failure-path diagnostics, printf-style; NULL for none. */
    void (*log)(const char *fmt, ...);
};

/* The host: every program that compiles the engine defines this table. */
extern const struct l2s_ops l2s_host;

/* Emulate link(src, dst) (host paths) via the backing-file symlink scheme.
 * Returns 0 or -errno. */
int l2s_link(const char *src, const char *dst);

/* If `host` is one of our l2s symlinks, fill `data` with its backing-file path
 * and *count with the marker count (0 if the marker is lost). Returns 1 (ours),
 * 0 (a real file/symlink), or -errno. `count` may be NULL. */
int l2s_resolve(const char *host, char *data, size_t dsz,
                unsigned long *count);

/* As l2s_resolve, and also for `host` being the data file itself (a following
 * walk already landed on it): the file a stat of `host` is to describe, and
 * the count its st_nlink is to carry. 1 (`data`, *count filled), 0, -errno. */
int l2s_target(const char *host, char *data, size_t dsz,
               unsigned long *count);

/* The count of the group whose data file is at `path` — where the kernel says
 * an open descriptor's file is — floored to 1: 1 (*count filled) or 0 (not a
 * data file of a group, in a place of the guest's own). */
int l2s_path_count(const char *path, unsigned long *count);

/* One name of a group was removed: `data`/`count` come from l2s_resolve run
 * *before* the removal. Decrement the marker; delete data+marker on the
 * last. */
void l2s_decref(const char *data, unsigned long count);

/* Rename support for legacy-format links (bare same-directory symlink
 * targets, which dangle when the name moves away from its data): before the
 * rename, `prep` captures the data file's absolute host path (1 = ours,
 * caller must fix up after a successful move); after it, `fixup` repoints the
 * moved name unless it still sits beside the data. */
int l2s_rename_prep(const char *srch, char *absdata, size_t sz);
void l2s_rename_fixup(const char *dsth, const char *absdata);

/* rename(2) of a group's last name: move the data file onto `dsth` (so the
 * group dissolves into the plain file a real hardlink's last name is) and drop
 * the marker and `srch`. Returns 1 if handled (*res = 0 or -errno), 0 if the
 * caller should rename the name itself. `flags`: L2S_RENAME_NOREPLACE or 0. */
int l2s_rename_last(const char *srch, const char *dsth, unsigned flags,
                    long *res);

/* True for any hidden l2s file (data or marker) basename — used to hide them
 * from the guest's directory listings and lookups, and to refuse a guest
 * symlink whose target's last component is one (such a link would be taken
 * for one of the emulation's own). */
int l2s_hidden(const char *name);

/* Is the canonical guest path `canon` the store, "/.l2s", or under it? The
 * guest is not to reach it by name. */
int l2s_store_name(const char *canon);

/* Could the symlink text `tgt` be one of the emulation's own links — ours (a
 * data file's name, absolute or a bare same-directory one) or a member of
 * proot's (an indirection's absolute path)? A shape test only, the cheap
 * first question of a listing: l2s_target has the answer. */
int l2s_link_text(const char *tgt);

/* Is the host path `path` canonical and in a place of the guest's own: one
 * whose text can be taken as it stands? */
int l2s_in_place(const char *path);

/* If `tgt` (a symlink target) is an absolute host path naming an l2s data
 * file, fill `out` with its guest-view path — for the guest-level resolver,
 * which must not re-root such targets as guest paths. Self-heals store paths
 * whose rootfs prefix went stale. Returns 1 (filled) or 0 (not ours: the
 * target is re-rooted as any other). */
int l2s_untranslate_target(const char *tgt, char *out, size_t sz);

#endif /* LINK2SYMLINK_H */
