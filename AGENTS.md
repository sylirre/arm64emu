# arm64emu

Guidelines for AI agents when working with this repository.

## Overview

Three programs, built from one tree by one Makefile, sharing code as libraries:

* **arm64emu** — AArch64 full-system emulator of a QEMU `virt` machine
  (interpreter, predecoded tier, JIT). `src/arm64emu/`
* **arm64chroot** — AArch64 Linux user-space emulator for unprivileged chroot
  environments (interpreter + optional JIT). `src/arm64chroot/`
* **chroot-ng** — native in-process chroot/bind emulation for AArch64 hosts, a
  ptrace-free proot (seccomp/SIGSYS, no CPU emulation). `src/chroot-ng/`

Target platforms are regular Linux distributions (musl, GNU libc) and Android
(Bionic, restricted by SELinux and seccomp).

Each program has its own guide, `src/<program>/AGENTS.md`: its structure,
tooling, test suites and diagnostics. Read it before working on that program.

## Layout

```
Makefile               the one build of everything (see "Building")
VERSION                the version of all three programs
lib/arm64/             AArch64 emulation shared by arm64emu and arm64chroot
lib/arm64/jit/         the JIT translator (IR, frontend, x86-64 and AArch64 backends)
lib/link2symlink/      hardlink emulation (--link2symlink), chroot-ng's and, next, arm64chroot's
src/<program>/         each program's own sources, and its AGENTS.md
tests/<program>/       each program's test suite
docs/<program>/        each program's documentation; README.md is its user guide
.github/workflows/     one workflow per program, plus version.yml (release tags)
build/                 everything the build makes (gitignored, see below)
```

The parts of the emulation core that still differ between the two emulators
(step driver, the predecoded tier's executor, the JIT runtime) live in each emulator's tree for now and
are kept diffable against each other; they move into `lib/` as they are
unified. A file in `lib/` is compiled into every program that uses it, with
that program's flags, so a change there must pass every such program's suites.

What differs between the programs is configured, not forked. `lib/arm64` takes
exactly two headers from the program, found on its include path:
`arm64_config.h`, how the library is built there (`ARM64_SYSTEM`: 1 for
arm64emu's full system, 0 for arm64chroot's user mode, where the guest never
leaves EL0; `ARM64_SMP`: 1 where guest threads run concurrently on host
threads, arm64chroot's case, so atomics and barriers must be host ones), and `mmu.h`, guest memory access (`mem_read`/`mem_write`,
`mem_host_ptr`, `tlb_flush_all`, ...). `lib/arm64/jit` also takes the program's
`jit_priv.h`: each program's JIT runtime (`jit.c`) defines the environment the
translator emits code against (`JitEnv`, its jump cache, helpers). Beyond
those it calls functions the program defines (`exception_take`, `gt_count`,
...). It includes no other program header.

`lib/link2symlink` includes no program header at all: it reaches the host only
through the table of operations the program defines as `l2s_host` (`struct
l2s_ops` in `link2symlink.h`: path calls kept inside the guest's places, a few
descriptor calls, the view), and it never fills a `struct stat` — it says which
file and which link count a name stands for, and the program makes the stat
call in its own host's layout.

## Building

`make` builds every program the host can build: chroot-ng needs an AArch64 C
compiler (native, or the `aarch64-linux-gnu` cross toolchain), arm64emu a
64-bit host. `make <program>` builds one; `make help` lists every target.

Everything produced goes under `build/`: the programs as `build/<name>`,
objects in `build/obj/<flavour>/`, the programs the suites build in
`build/tests/<program>/`, the provisioned test rootfs images and recordings in
`build/cache/<program>/`. `make clean` keeps `build/cache/`;
`make clean-testenv` removes it.

`CC`, `CFLAGS` and `LDFLAGS` are the host compiler and extra flags of the two
emulators. Every other build variable belongs to one program and carries its
prefix: `ARM64EMU_OPT`, `ARM64CHROOT_M32CC`, `CHROOTNG_CC`,
`CHROOTNG_CFLAGS`, ...

## Testing

Each program has its own suites (`test-arm64emu*`, `test-arm64chroot*`,
`test-chroot-ng`; see the program's guide). `make test` runs the three main
suites one after another.

**Important**: never run two suites at the same time. They share fixed scratch
names and have timing-sensitive rows. The Makefile chains several suites named
on one command line, but it cannot see a suite started from another shell.

A change to `lib/arm64/` is tested by both emulators' suites, interpreter and
JIT; a change to `lib/link2symlink/` by both arm64chroot's and chroot-ng's.

## Environment variables

Every environment variable a program or its test harness reads carries that
program's prefix: `ARM64EMU_` (`ARM64EMU_JIT_` for JIT knobs), `ARM64CHROOT_`
(`ARM64CHROOT_JIT_`), `CHROOTNG_`. Shared code takes the prefix from the program
that compiles it. New variables must also be added to the program's built-in
help.

## Versioning

All three programs share one version, written only in `VERSION`. The Makefile
passes it as `PROJECT_VERSION` to the units that print it. A release is the
tag `vX.Y.Z`, which CI checks against `VERSION`. Older history carries
per-program tags (`<program>/vX.Y.Z`).

## Conventions and rules

General:

* Be thorough in reasoning and concise in output.
* Do not re-read files unless they were changed.
* Do not switch branches, do not look up changes on other branches unless explicitly were asked for this.
* Think about best approach when implementing a requested feature. Ask clarifying questions before making architectural changes and propose solution variants, especially if there are caveats and unintended side effects of requested changes.
* New command line options and environment variables must be added into utility built-in help information.
* Ensure that code comments are up-to-date after made changes.
* Compiler warnings or errors must be resolved.
* Do not give up chasing bugs. You know the code better than anyone.
* Run test suite after finishing changes.
* Never make a failing test pass by weakening it. Investigate the root cause of test failure. If the test indeed faulty, ask the project owner first before making changes.
* If all tests passed and there are no already staged files, commit your changes to current branch.
* Never run `git push`.

Commit messages:

* Each commit must consist of header and description.
* The header must follow this format: `scope(area): brief description of change`, or `scope: ...` without an area. The scope is what the change is to:
  * `arm64emu`, `arm64chroot`, `chroot-ng` — a program (its `src/`, `tests/` and `docs/` trees);
  * `arm64` — the shared AArch64 emulation, `lib/arm64/**` (e.g. `arm64(jit): ...`);
  * `l2s` — the shared hardlink emulation, `lib/link2symlink/`;
  * `repo` — repository-level work: the Makefile, `VERSION`, CI, the root documents (e.g. `repo(ci): ...`).

  The area is optional and names the part touched, e.g. `ci`, `main`, `interp`, `jit`, `fpsimd`, `syscall`, `proc`, `tests`, `docs`.
* The commit body must be detailed and explain why change was necessary, what was the story behind it. If that's a new feature, explain what it does. If that's a bugfix, explain what was the bug and how it was fixed.
* Wrap each line of the commit body at 72 characters.
* Add `Co-Authored-By` footer with explanation who you are.
* It is forbidden to include `Claude-Session` reference in the commit message.
