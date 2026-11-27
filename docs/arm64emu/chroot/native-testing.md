# The native tier's test suite

```sh
make test-arm64emu-chroot-native   # or: sh tests/arm64emu/chroot/native/run.sh
```

The suite of `arm64emu chroot --native` (its milestones are chroot-ng's, the
program the tier was before it became one; see [native-history.md](native-history.md)).
It runs under `ARM64EMU_NATIVE_NO_FALLBACK=1`, so a run that the tier could
not make is an error rather than the emulator's run in its place: no row
passes on the emulator's answers, except M29's, which clear it to see the
fallback itself.

The suite runs on three hosts. `tests/arm64emu/chroot/native/lib.sh` resolves every difference between
them at startup and prints what it found; the milestone scripts (`m*.sh`) assert
against that rather than against a hardcoded devbox.

| host | how binaries run | seccomp/SIGSYS tier | guest toolchain |
|---|---|---|---|
| Linux x86_64 | `build/arm64emu-a64`, the static-PIE cross build, under `build/arm64emu chroot --jit --keep-fds --shared-proc -w $PWD /`; the whole environment is forwarded with `-E`, and the emulator's own is cleared of the `ARM64EMU_*` names, which are the tier's knobs there | **live** — arm64emu's chroot mode applies a guest's filter, `RET_TRAP` and `SIGSYS` included (qemu-user does not, and cannot run the tier) | `aarch64-linux-gnu-gcc*`, static-PIE |
| Linux aarch64 | `build/arm64emu`, directly | **live** | native `gcc`/`cc`/`clang`, static-PIE |
| Termux (Android aarch64) | `build/arm64emu`, directly | **live** | `clang` (bionic); often dynamic-only |

That the tier is live is checked, not inferred from `uname`: a compiled guest
opens a file that is only in a scratch rootfs, and the suite stops with the
reason where it is not found there — nothing a row asserts holds on a host that
does not apply the filter. Whether anonymous memory may be made executable is
asked of the host directly (`tests/arm64emu/chroot/native/guests/execmem.c`).

Where a differential compares against "the kernel", the reference on a foreign
host is the same AArch64 build run under the runner without the tier: the
kernel a guest's calls reach there is arm64emu's (the 6.1 it advertises), not
the x86 host's.

## Skips

A leg a host genuinely cannot exercise prints `skip <reason>` and is counted
separately in the summary, so a partial run never reads like full coverage:

```
== summary: 312 passed, 0 failed, 1 skipped ==
```

Common reasons: no AArch64 guest toolchain; an Alpine/Debian rootfs image that
could not be provisioned (no network, `ARM64EMU_NATIVE_ROOTFS_FETCH=0`); no
host-native `arm64emu chroot` oracle (M10 is differential against it); the host has
no working SysV shm to diff against (Android drops it); the guest links
dynamically *and* the seccomp tier is inert, so neither tier reaches libc's own
`svc` sites.

### On Termux (Android)

The device is the strictest host, and most of its skips are the platform
refusing something rather than a leg being unsound. A skip is only ever taken
for a fact the run observed — the driver or the shell says which — never for an
expected failure:

* **Android's seccomp filter blocks syscalls** that the kernel has (`openat2`,
  `faccessat2`, `name_to_handle_at`, `fchmodat2`, `rseq`, `set_robust_list`, the
  6.13 `*xattrat` family). The emulation answers `ENOSYS` as an older kernel
  would; a leg that needs the real call, or a host-built reference that issues
  it, skips. `openat2` is the exception: libc no longer falls back from it, so
  it is emulated, here and on any kernel without it, and m24 holds the
  emulation to the kernel's recorded answers (`ARM64EMU_OPENAT2_FORCE_DENY=1` takes it as
  refused anywhere, so a native host diffs it against its own kernel). The uid
  and gid setters are refused too (all but `setresuid`), and
  answered from the real ids as the kernel would answer an app's process (m9;
  `ARM64EMU_SETID_FORCE_DENY=1` takes them as refused anywhere, and
  `tests/arm64emu/chroot/native/guests/ambtrap.c` installs such a filter where seccomp is live).
* **SELinux hides or refuses what a kernel would allow**: hardlinks (`ln` is
  `EACCES`, so the l2s fallback is what runs), `stat` and `open` of
  `/proc/{loadavg,uptime,stat,version}` and `/proc/sys/kernel/overflow{u,g}id`
  (they are synthesized for that reason, and so is their `stat` —
  `ARM64EMU_PROCSYNTH_FORCE_STAT_DENY=1` forces that path elsewhere, `ARM64EMU_OVERFLOWID_FORCE_SYNTH=1`
  the open of the sysctls),
  reopening a memfd through `/proc/self/fd/N` (so a synthesized fd's read-only
  view is an unlinked file in a private directory under `$TMPDIR` instead —
  `ARM64EMU_PROCFS_REOPEN_FORCE_DENY=1` forces that elsewhere — and a guest's own
  read-only reopen of its memfd is refused, as there, except for the script a
  `#!` exec ran from one), and opening `/dev/pts`. The ioctls on a pty are a whitelist as
  well: `TCGETS2` and the other termios2 commands are `EACCES`, which the
  monitor answers from `TCGETS`/`TCSETS*` (m27; `ARM64EMU_TERMIOS2_FORCE_DENY=1` forces
  that path on a host that serves them), and so is `TIOCGSID` on a slave and
  on a pipe, answered from `TCGETS`, `TIOCGPGRP` and `getsid`
  (`ARM64EMU_TIOCGSID_FORCE_DENY=1`). qemu-user answers `TIOCGSID` with success and no
  session, so m27 asks the host once, outside the monitor, and judges only the
  forced rows there. Android 8's list lacks `TCSETSF` too (`tcsetattr` with
  `TCSAFLUSH`), answered from `TCFLSH` and `TCSETSW` (`ARM64EMU_TCSETSF_FORCE_DENY=1`).
  Android 15's is shorter still for what is no terminal, where a kernel says
  `ENOTTY` to all of them: on a pipe `TCGETS` alone, on a directory none at
  all. The monitor's fallbacks take a refused `TCGETS` on what is no
  character device for `ENOTTY`, and m27's pipe and directory rows are where
  a native run on the device differs from the expected text.
* **The kernel is built without checkpoint/restore support**: no
  `/proc/self/timers`, and `PTRACE_O_SUSPEND_SECCOMP` is `EINVAL`.
* **`$TMPDIR` is inside `$PREFIX`, which the harness binds** for a guest's
  linker. A scratch directory the guest must *not* be able to name therefore has
  to be moved out of the binds (`cng_outside_binds`), and the arm64emu oracle
  needs its rootfs there too, because it spells a guest path with the longest
  host prefix it finds.
* **`LD_PRELOAD` carries `libtermux-exec`**, which answers some `execve`
  failures itself. A host-built reference that stands for "the kernel" is run
  through `kref`, which drops it.
* **An older Android is older all the way down** (Android 8 on a 3.18
  kernel). There is no `/bin`, so a script the host kernel runs names the
  host's own `sh` (`$HOST_SH`); the ramdisk may carry a `/lib` of its own, so
  a guest's binds there are only the directories bionic's linker reads; its
  libc has no `posix_spawn` (API 28). The filter traps every syscall newer
  than itself — `statx`, the pidfd calls — and a leg that needs one skips on
  the `ENOSYS` (`-1 = not issuable here` in a driver's line). The kernel
  predates what some references are compared for — `execveat` (3.19), the
  `NoNewPrivs` status line (4.10), `PTRACE_O_SUSPEND_SECCOMP` (4.3, which the
  emulation then refuses as that kernel does), the `NLM_F_CAPPED` flag on a
  netlink ACK (4.12), the NUL ending a title run past argv in
  `/proc/<pid>/cmdline` (4.18), a recvmsg name of length 0 and an iovec count
  past `UIO_MAXIOV` read as newer kernels read them — and those comparisons
  skip, the emulation answering as the 6.1 it advertises.
  SELinux refuses `accept` on a netlink socket before the family answers it.
  Termux ships `<sys/sem.h>` in a package of its own
  (`libandroid-sysv-semaphore`) and `libandroid-shmem` turns the shm calls
  into macros for an in-process emulation; `tests/arm64emu/chroot/native/guests/sysvipc.h` builds
  the SysV guests on raw syscalls either way.

## Environment knobs

| variable | meaning |
|---|---|
| `ARM64EMU_NATIVE_BIN` | the arm64emu under test, run as `chroot --native` (default `build/arm64emu` on an AArch64 host, `build/arm64emu-a64` elsewhere; `build/arm64emu-embed` tests the embedded build) |
| `ARM64EMU_NATIVE_EMU` | the arm64emu that runs AArch64 programs on a foreign host. Unset = `build/arm64emu` there, none on an AArch64 host. Set **empty** to force direct execution on a cross host that runs AArch64 binaries another way (a `binfmt_misc` handler) |
| `ARM64EMU_NATIVE_GUESTCC` | compiler for `tests/arm64emu/chroot/native/guests/*.c`. Unset = probe a candidate list |
| `ARM64EMU_NATIVE_GUESTLD` | link mode for those guests: `-static-pie`, `-static`, or empty for dynamic. Unset = try each in that order and keep the first that both links an AArch64 binary and runs here |
| `ARM64EMU_NATIVE_GUEST_BINDS` | extra `-b SRC:DST` args a dynamic guest needs to find its ELF interpreter inside a synthetic rootfs. Auto-derived (`/system`, `/apex`, `/linkerconfig`, …) when the guest is not static |
| `ARM64EMU_NATIVE_ROOTFS_DIR` | where the `alpine/` and `debian/` rootfs images are provisioned (default `build/cache/arm64emu/chroot/native/rootfs`) |
| `ARM64EMU_NATIVE_ROOTFS_FETCH` | `0` = never download a rootfs tarball; one already in `$ARM64EMU_NATIVE_ROOTFS_DIR/downloads/` is still unpacked |
| `ARM64EMU_NATIVE_ALPINE`, `ARM64EMU_NATIVE_DEBIAN` | individual rootfs paths, used as they are instead of provisioning one |
| `ARM64EMU_NATIVE_ORACLE` | `arm64emu` binary for M10's real-hardlink differential, run in its chroot mode. Unset = `build/arm64emu`, which `make test-arm64emu-chroot-native` builds first (a bare `sh tests/arm64emu/chroot/native/run.sh` uses it only if present). Dropped automatically if its machine does not match the host — it is a host-native emulator, not an AArch64 program |
| `ARM64EMU_NATIVE_SYSROOT` | sysroot holding the AArch64 ELF interpreter for M4 (cross host default `/usr/aarch64-linux-gnu`; empty on an AArch64 host, where it is on the system paths) |

`cng_dyn_binds` sets `GUEST_DYN_BINDS`, the same list for a guest that is
dynamic even where the probed link mode is static — M23 builds one deliberately,
since the noexec `.so` path only exists for a guest whose own `ld.so` maps
libraries. On a cross host it binds the sysroot's directories at the paths the
binary names, which is how the loader finds the interpreter there.

The suite provisions its rootfs images itself, under `$ARM64EMU_NATIVE_ROOTFS_DIR`. Each one
is a pinned tarball (`cng_rootfs_pin` in `tests/arm64emu/chroot/native/lib.sh`) that is downloaded
once into `downloads/`, checked against its sha256, and unpacked under its own
name:

* **`alpine/`** is the stock Alpine minirootfs (~3.5 MB), provisioned at
  startup. The suite only ever calls `bin/busybox` inside it.
* **`debian/`** is the arm64 layer of the official `debian:trixie-…-slim` image,
  fetched from Docker Hub (~30 MB). It is provisioned only when M10 reaches its
  Debian leg, which needs the arm64emu oracle as well.

A tree is stamped with the hash it came from (`.alpine.sha256` beside it), so
bumping a pin replaces it on the next run. A tree that carries no stamp was
staged by hand, and is used as it is and never touched. Runs that share
`$ARM64EMU_NATIVE_ROOTFS_DIR` take turns provisioning. On a host without network access,
copy the tarball into `downloads/` yourself; with `ARM64EMU_NATIVE_ROOTFS_FETCH=0`, the
legs skip instead of trying the network. CI (`.github/workflows/arm64emu.yml`'s AArch64
job, a hosted Linux AArch64 runner) caches `downloads/` between runs.

## Adding a milestone script

Source-level contract for a new `tests/arm64emu/chroot/native/mNN_*.sh`:

- `run` / `run_t SECS` — invoke `arm64emu chroot --native`; `emu` / `emu_t SECS`
  — invoke any other AArch64 program. Never spell `$ARM64EMU_NATIVE_EMU` or
  `$ARM64EMU_NATIVE_BIN` directly.
- `guest_cc OUT SRC` (rc 2 = no toolchain) or `guest_cc_report OUT SRC`, which
  emits the right diagnosis on its own. Never invoke a compiler by name.
- `guest_xlate_ready WHAT` before any leg that needs a compiled guest's *own*
  path syscalls to be translated.
- `check`, `check_contains`, `check_absent`, `skip` for reporting — `skip` keeps
  the count honest.
- A guest inherits no host environment (M17-16). What a guest program must see
  goes in with `-E VAR=VAL`; a `VAR=val run …` prefix reaches arm64emu only,
  which is how the `ARM64EMU_*` knobs are set.
- `elf_type` / `elf_machine` / `elf_has_interp` instead of `file(1)`, which a
  bare Termux does not have.
- Prefer a directory you created over a host path (`/usr`, `/tmp`, `/run` and
  `/etc/passwd` are not universal), and derive an expected errno from a control
  run rather than pinning one that depends on the filesystem.
- When a leg is gated on a host capability, test *that* capability, not a proxy
  for it. M16 gated its rtnetlink differential on `getifaddrs` succeeding — but
  bionic has its own fallback for the very restriction being emulated, so on a
  device `getifaddrs` succeeds while `bind`/`sendto` on a NETLINK_ROUTE socket
  are refused; the gate took the devbox branch there and failed four legs for
  exactly the reason the emulation exists. Where a capability splits hosts,
  give each side its own assertions rather than skipping one.
- Ask what the guest sees with a guest program: the tier has no self-test
  entry, so a behaviour is checked end to end, through the filter, the way a
  guest meets it.
