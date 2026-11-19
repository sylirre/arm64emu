# arm64emu

Guidelines for AI agents when working with this repository.

## Overview

Two programs, built from one tree by one Makefile, sharing code as libraries:

* **arm64emu** — AArch64 emulator (interpreter, predecoded tier, JIT) with two
  modes: `arm64emu boot`, a full system, the QEMU `virt` machine; and
  `arm64emu chroot`, Linux user space, for unprivileged chroot environments.
  `src/arm64emu/`
* **chroot-ng** — native in-process chroot/bind emulation for AArch64 hosts, a
  ptrace-free proot (seccomp/SIGSYS, no CPU emulation). `src/chroot-ng/`

Target platforms are regular Linux distributions (musl, GNU libc) and Android
(Bionic, restricted by SELinux and seccomp).

Each program has its own guide, `src/<program>/AGENTS.md`: its structure,
tooling, test suites and diagnostics. Read it before working on that program.

## Layout

```
Makefile               the one build of everything (see "Building")
VERSION                the version of both programs
lib/arm64/             AArch64 emulation: the CPU arm64emu runs in both modes
lib/arm64/jit/         the JIT: translator (IR, frontend), runtime, the four host backends
lib/link2symlink/      hardlink emulation (--link2symlink) shared by arm64emu's
                       chroot mode and chroot-ng
src/<program>/         each program's own sources, and its AGENTS.md
                       (arm64emu: what both modes share, and boot/, chroot/)
tests/<program>/       each program's test suites (arm64emu: boot/, chroot/)
docs/<program>/        each program's documentation; README.md is its user guide
.github/workflows/     one workflow per program, plus version.yml (release tags)
build/                 everything the build makes (gitignored, see below)
```

The whole emulation core is one: the interpreter, the predecoded tier and
the JIT, both of arm64emu's modes running the same code. `lib/arm64` is
compiled once (per host compiler) and linked into every build of arm64emu;
`lib/link2symlink` is compiled into each program that uses it, with that
program's flags. Either way a change there must pass every suite that runs
it: both modes', and for `lib/link2symlink` chroot-ng's too.

What differs between the modes is configured, not forked, and decided at run
time, not at build time: each CPU carries the mode it runs in (`c->eng.sys`:
1 for boot mode's full system, 0 for chroot mode's user space, where the
guest never leaves EL0; `c->eng.smp`: 1 where guest threads run concurrently
on host threads, chroot mode's case, so atomics and barriers must be host
ones). The interpreter's EL1-only checks are the architecture's
own, decided by the EL and the reset values `sysreg_init` gives each mode; the
JIT makes its choices as it translates, so generated code carries none. The
engines (`cpu_step`, the predecoded tier's `pd_run`, the JIT's `jit_run`)
share one contract with the mode's run loop (`cpu.h`, "Run control"):
they run guest code until the CPU asks for attention -- a stop, a halt, an
interrupt it can take, whatever the mode raises of its own -- or reach the
deadline the loop gives them.
`lib/arm64` includes no program header and names no program function: what
it cannot do itself it asks the program through the table of operations each
CPU carries beside its mode (`c->eng.ops`, `struct CpuOps` in `cpu.h`),
which the mode sets up with the CPU. Guest memory is the library's own
interface, `lib/arm64/mem.h`: the typed accesses, instruction fetch and
`mem_host_ptr`, inline over the translation caches part of every CPU
(`c->eng`: one D-TLB entry layout and one fetch cache for both modes),
with slow paths and cache maintenance (`mem_read_slow`, ...,
`tlb_flush_all`) the mode's ops supply for the memory it models. The
JIT's coherence with that memory is the mode's too: the runtime asks it
through the ops' `JitCoherence` (`lib/arm64/jit/jit.h`) what a translated
page's change looks like there -- a store to a code page in a full system,
an munmap or IC IVAU in user mode. Exception entry (`exception_take`), the
generic timer and the firmware's HVC/SMC are ops as well. The environment
knobs the library reads carry the prefix the program sets in
`g_env_prefix` (`arm64_getenv`).

arm64emu's entry (`src/arm64emu/main.c`) hands the command line to the mode
its first word names, and what both modes' command lines share -- the help renderer, the
option helpers, the version, the engine options -- is `src/arm64emu/cli.c`'s.
Each mode's own sources and headers are in its directory (`boot/`, `chroot/`),
which the other's never include. A 32-bit host builds chroot mode only (boot
mode's guest-RAM model needs `__int128`), and `arm64emu boot` says so there.

`lib/link2symlink` includes no program header at all: it reaches the host only
through the table of operations the program defines as `l2s_host` (`struct
l2s_ops` in `link2symlink.h`: path calls kept inside the guest's places, a few
descriptor calls, the view), and it never fills a `struct stat` — it says which
file and which link count a name stands for, and the program makes the stat
call in its own host's layout.

## Building

`make` builds every program the host can build: chroot-ng needs an AArch64 C
compiler (native, or the `aarch64-linux-gnu` cross toolchain); arm64emu builds
anywhere, its boot mode on a 64-bit host only. `make <program>` builds one;
`make help` lists every target, arm64emu's variant builds among them.

Everything produced goes under `build/`: the programs as `build/<name>`,
objects in `build/obj/<flavour>/`, the programs the suites build in
`build/tests/<program>/`, the provisioned test rootfs images and recordings in
`build/cache/<program>/`. `make clean` keeps `build/cache/`;
`make clean-testenv` removes it.

`CC`, `CFLAGS` and `LDFLAGS` are arm64emu's host compiler and extra flags, of
its own sources and of the `lib/arm64` it links alike (`make CFLAGS="-O3
-flto=8"` is the fast recipe). Every other build variable belongs to one program and
carries its prefix: `ARM64EMU_OPT`, `ARM64EMU_M32CC`, `CHROOTNG_CC`,
`CHROOTNG_CFLAGS`, ...

## Testing

Each program has its own suites, arm64emu one family per mode
(`test-arm64emu-boot*`, `test-arm64emu-chroot*`, `test-chroot-ng`; see the
program's guide). `make test` runs the three main suites one after another:
`test-arm64emu-boot`, `test-arm64emu-chroot`, `test-chroot-ng`.

**Important**: never run two suites at the same time. They share fixed scratch
names and have timing-sensitive rows. The Makefile chains several suites named
on one command line, but it cannot see a suite started from another shell.

A change to `lib/arm64/`, or to what both of arm64emu's modes share, is tested
by both modes' suites, interpreter and JIT; a change to `lib/link2symlink/` by
both chroot mode's and chroot-ng's.

## Environment variables

Every environment variable a program or its test harness reads carries that
program's prefix: `ARM64EMU_` (`ARM64EMU_JIT_` for JIT knobs), whichever the
mode, and `CHROOTNG_`. Shared code takes the prefix from the program
that runs it (`lib/arm64`'s `g_env_prefix`, `lib/link2symlink`'s from the
program that compiles it). New variables must also be added to the program's
built-in help.

## Versioning

Both programs share one version, written only in `VERSION`. The Makefile
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
  * `arm64emu`, `chroot-ng` — a program (its `src/`, `tests/` and `docs/` trees); a change to one of arm64emu's modes names it as the area, or the part of it touched (`arm64emu(boot): ...`, `arm64emu(chroot): ...`, `arm64emu(syscall): ...`);
  * `arm64` — the shared AArch64 emulation, `lib/arm64/**` (e.g. `arm64(jit): ...`);
  * `l2s` — the shared hardlink emulation, `lib/link2symlink/`;
  * `repo` — repository-level work: the Makefile, `VERSION`, CI, the root documents (e.g. `repo(ci): ...`).

  The area is optional and names the part touched, e.g. `boot`, `chroot`, `ci`, `main`, `interp`, `jit`, `fpsimd`, `syscall`, `proc`, `tests`, `docs`.
* The commit body must be detailed and explain why change was necessary, what was the story behind it. If that's a new feature, explain what it does. If that's a bugfix, explain what was the bug and how it was fixed.
* Wrap each line of the commit body at 72 characters.
* Add `Co-Authored-By` footer with explanation who you are.
* It is forbidden to include `Claude-Session` reference in the commit message.
