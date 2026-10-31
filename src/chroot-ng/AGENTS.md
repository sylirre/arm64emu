# chroot-ng

Guidelines for AI agents when working with chroot-ng (`src/chroot-ng/`). The
repository-wide ones — layout, build, test discipline, conventions, commit
messages — are in the root `AGENTS.md`; this file adds what is particular to
chroot-ng.

## Overview

Emulator for chroot and `mount --bind` for AArch64 devices, similar to proot but ptrace-free.

Target platforms are regular Linux distributions (Musl, GNU libc) and Android OS (Bionic, restricted by SELinux and seccomp).

## Project structure

```
Makefile               (repository root) `make chroot-ng` -> build/chroot-ng; needs an
                       AArch64 C compiler (CHROOTNG_CC, or the aarch64-linux-gnu cross one)
src/chroot-ng/
  main.c               command-line front end, --help (the option and environment reference)
  run.c                cng_run(): load the guest with the userland loader, install the monitor
  probe.c              --probe: the host capabilities the in-process design depends on
  debug.c              hidden debug subcommands (-t ...) that test internals without a real kernel
  loader/              userland ELF loader: map a guest from a read-only fd, its initial stack
  monitor/             the syscall monitor: SIGSYS handler, dispatcher, svc-site rewriting (-R),
                       and the emulated subsystems (/proc, ptrace, SysV IPC, netlink, link2symlink, ...)
  path/                rootfs path translation and containment
  rt/                  the freestanding runtime: _start, the syscall gate (the only svc #0), libc-like helpers
  include/cng/         headers, including the UAPI constants re-declared from the AArch64 syscall ABI
  chroot-ng.ld         linker script: the image lives far above the guest ET_EXEC range
lib/link2symlink/      (repository root) the -l/--link2symlink engine, shared with arm64chroot;
                       monitor/l2s.c is chroot-ng's host for it (struct l2s_ops) and its stat answers
tests/chroot-ng/       run.sh + lib.sh (host detection) + one m*.sh per milestone; guests/ are the
                       AArch64 test programs the milestones build
docs/chroot-ng/        README.md (user guide), DESIGN.md, STATUS.md (milestone log), testing.md
```

chroot-ng is freestanding: no libc, raw syscalls only, and no floating point or SIMD
anywhere in the monitor (`-mgeneral-regs-only`; the Makefile explains why).

## Testing

* `make test-chroot-ng`: the whole suite. It builds arm64chroot first, the
  real-hardlink oracle the differential `--link2symlink` legs compare against.
* `make probe-chroot-ng`: the capability probe on this host.

The suite runs natively on an AArch64 host and under `qemu-aarch64` on any
other (where the seccomp/SIGSYS tier is inert and `-R` carries translation).
`docs/chroot-ng/testing.md` lists its environment knobs and what each host
provides.

**Important**: do not run the suite in parallel with any other suite.
