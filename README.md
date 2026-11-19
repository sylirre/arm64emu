# arm64emu

Two ways to run AArch64 Linux software where you could not otherwise, written
in C11 with no external dependencies:

| Program | What it is | Runs on |
|---|---|---|
| [**arm64emu**](docs/arm64emu/README.md) | An AArch64 emulator with two modes. `arm64emu boot` is a full-system emulator of a QEMU `virt` machine: it boots EDK2 firmware and Linux. `arm64emu chroot` is a user-space emulator that runs an AArch64 rootfs in an unprivileged, isolated chroot, with bind mounts, a fake root and hardlink emulation. Interpreter, predecoded tier and JIT. | x86-64, i686, ARM32 and AArch64 Linux, Android/Termux (boot mode: 64-bit hosts) |
| [**chroot-ng**](docs/chroot-ng/README.md) | A native chroot and `mount --bind` for AArch64 hosts that needs no root and no ptrace: an in-process seccomp/SIGSYS monitor instead of an emulated CPU. | AArch64 Linux, Android/Termux |

## Building

```sh
make                 # every program this host can build, into build/
make arm64emu        # or just one: arm64emu, chroot-ng
make test            # the main test suites, one after another
make help            # every target
```

chroot-ng needs an AArch64 C compiler: the native one on an AArch64 host, the
`aarch64-linux-gnu` cross toolchain elsewhere (it then runs under
`qemu-aarch64`). arm64emu builds on any Linux host, its boot mode on a 64-bit
one. Each program's guide describes its own build options and test suites.

## Layout

```
src/<program>/     each program's sources
lib/arm64/         AArch64 emulation: the CPU of both of arm64emu's modes
tests/<program>/   each program's test suites
docs/<program>/    each program's documentation
build/             everything the build makes (not tracked)
```

Both programs share one version, given in [`VERSION`](VERSION).

## License

**Apache License 2.0** — see [LICENSE](LICENSE) and [NOTICE](NOTICE).
