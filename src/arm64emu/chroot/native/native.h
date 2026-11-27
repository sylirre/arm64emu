/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* The native tier, `arm64emu chroot --native`, as chroot mode's entry sees it:
 * the one header of this directory chroot mode includes, and the one place
 * the two meet.
 *
 * The tier runs the guest's code on the host CPU itself, in this process: a
 * userland ELF loader maps the program and its interpreter, and a seccomp
 * filter traps the guest's system calls into a SIGSYS handler of ours, which
 * serves them against the same view of the rootfs the emulator serves (the
 * binds, the fake identity, /proc and /dev, the emulated hardlinks). Its
 * names carry the prefix cng_, from chroot-ng, the program it was before it
 * became a tier of this one; its design is docs/arm64emu/chroot/native.md.
 *
 * Built only where the target is AArch64 (the Makefile); elsewhere
 * native_run is not defined at all, and chroot mode, which declares it weak,
 * falls back to the emulator saying so. */
#ifndef ARM64EMU_NATIVE_H
#define ARM64EMU_NATIVE_H

#include <stddef.h>

/* A -b/--bind mount: the host path `host` (canonical) at the guest path
 * `guest` (canonical, absolute, not "/"), read-only when `ro`. */
struct native_bind {
    const char *host, *guest;
    int ro;
};

/* The command line, parsed and checked by chroot mode (chroot/main.c). */
struct native_opts {
    const char *rootfs;   /* canonical host path; "/" for the host's root */
    const char *workdir;  /* -w/--work-dir, a guest path; NULL: the default */
    const char *prog;     /* <program>, a guest path */
    char **argv;          /* the guest's argv (argv[0] -0's when given) */
    char **envp;          /* the guest's whole environment */
    const struct native_bind *binds;
    int nbinds;
    /* -u/--fake-id and what implies it; fake_explicit: -u gave the identity,
     * which --setuid-root/--setgid-root alone leave the invoking user's. */
    int fake_id, fake_explicit;
    unsigned fake_uid, fake_gid;
    int setuid_root, setgid_root;
    int link2symlink;     /* -l */
    int file_backed;      /* -F */
    int no_proc, no_dev, keep_fds, host_keyring, share_abstract, shared_proc;
};

/* native_run's answer when the tier cannot run this guest here. */
#define NATIVE_FALLBACK (-1)

/* Run the guest natively. Returns only when it could not be entered:
 * NATIVE_FALLBACK, with `why` (a phrase, "the kernel refused the seccomp
 * filter (EINVAL)") saying what stood in the way, before anything was done
 * that the emulator, which then runs the guest, would mind; or an exit status,
 * after a message, for a failure the emulator would meet as well (a
 * -w that names no directory, a program that is not there).
 *
 * The checks before the point of no return (preflight.c): the process has
 * one thread; a forked child of it can install a seccomp filter, and the
 * kernel applies it (an emulator underneath may accept one and ignore it);
 * guest code can be made executable there, by anonymous memory turned
 * executable or by mapping the program's file; and a program linked at a
 * fixed address (ET_EXEC) finds that address free. */
int native_run(const struct native_opts *o, char *why, size_t whysz);

#endif /* ARM64EMU_NATIVE_H */
