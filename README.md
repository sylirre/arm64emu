# arm64emu

Three ways to run AArch64 Linux software where you could not otherwise, written
in C11 with no external dependencies:

| Program | What it is | Runs on |
|---|---|---|
| [**arm64emu**](docs/arm64emu/README.md) | A full-system emulator of a QEMU `virt` machine: boots EDK2 firmware and Linux. Interpreter, predecoded tier and JIT. | 64-bit Linux hosts |
| [**arm64chroot**](docs/arm64chroot/README.md) | A user-space emulator that runs an AArch64 rootfs in an unprivileged, isolated chroot, with bind mounts, a fake root and hardlink emulation. Interpreter and JIT. | x86-64, i686, ARM32 and AArch64 Linux, Android/Termux |
| [**chroot-ng**](docs/chroot-ng/README.md) | A native chroot and `mount --bind` for AArch64 hosts that needs no root and no ptrace: an in-process seccomp/SIGSYS monitor instead of an emulated CPU. | AArch64 Linux, Android/Termux |

The two emulators share their AArch64 emulation core (`lib/arm64`).

## Building

```sh
make                 # every program this host can build, into build/
make arm64chroot     # or just one: arm64emu, arm64chroot, chroot-ng
make test            # each program's main test suite, one after another
make help            # every target
```

chroot-ng needs an AArch64 C compiler: the native one on an AArch64 host, the
`aarch64-linux-gnu` cross toolchain elsewhere (it then runs under
`qemu-aarch64`). arm64emu needs a 64-bit host. Each program's guide describes
its own build options and test suites.

## Layout

```
src/<program>/     each program's sources
lib/arm64/         AArch64 emulation shared by arm64emu and arm64chroot
tests/<program>/   each program's test suite
docs/<program>/    each program's documentation
build/             everything the build makes (not tracked)
```

All three programs share one version, given in [`VERSION`](VERSION).

## License

**Apache License 2.0** — see [LICENSE](LICENSE) and [NOTICE](NOTICE).
