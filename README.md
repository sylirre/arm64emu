# arm64emu

A way to run AArch64 Linux software where you could not otherwise, written in
C11 with no external dependencies:

| Program | What it is | Runs on |
|---|---|---|
| [**arm64emu**](docs/arm64emu/README.md) | An AArch64 emulator with two modes. `arm64emu boot` is a full-system emulator of a QEMU `virt` machine: it boots EDK2 firmware and Linux. `arm64emu chroot` is a user-space emulator that runs an AArch64 rootfs in an unprivileged, isolated chroot, with bind mounts, a fake root and hardlink emulation; interpreter, predecoded tier and JIT. On an AArch64 host, `arm64emu chroot --native` runs the rootfs's programs on the CPU itself, needing no root and no ptrace: an in-process seccomp/SIGSYS monitor instead of an emulated CPU (it was chroot-ng). | x86-64, i686, ARM32 and AArch64 Linux, Android/Termux (boot mode: 64-bit hosts; `--native`: AArch64) |

## Building

```sh
make                 # build/arm64emu
make test            # the main test suites, one after another
make help            # every target
```

arm64emu builds on any Linux host: its boot mode on a 64-bit one, chroot
mode's native tier on an AArch64 one. Its guide describes its build options
and test suites.

## Layout

```
src/arm64emu/      the program's sources
lib/arm64/         AArch64 emulation: the CPU of both of arm64emu's modes
lib/link2symlink/  hardlink emulation, shared by both of chroot mode's tiers
tests/arm64emu/    its test suites
docs/arm64emu/     its documentation
build/             everything the build makes (not tracked)
```

The version is given in [`VERSION`](VERSION).

## License

**Apache License 2.0** — see [LICENSE](LICENSE) and [NOTICE](NOTICE).
