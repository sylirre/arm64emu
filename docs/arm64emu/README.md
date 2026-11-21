# arm64emu — an AArch64 emulator

A from-scratch **ARMv8-A (AArch64) emulator** written in C11 with **no external
dependencies** (libc/POSIX only), with two modes:

- **`arm64emu boot`** — a **full system**: the QEMU `virt` machine (GICv2). It
  runs the **real EDK2 `ArmVirtQemu` UEFI firmware** and **boots Linux to a
  userspace shell**, with the **serial console on your terminal**.
- **`arm64emu chroot`** — **Linux user space**: it runs the programs of an
  AArch64 root filesystem in an **unprivileged, isolated chroot** on the host's
  own kernel, with bind mounts, a fake root and hardlink emulation.

Both modes run the same CPU: the plain **interpreter**, which is the reference,
the **predecoded tier**, the default (`--no-predecode` opts out), and the
**JIT** (`--jit`, on AArch64, x86-64, i686 and ARM32 hosts), the two faster
tiers kept byte-identical to the reference. Boot mode needs a 64-bit host;
chroot mode runs on x86-64, i686, ARM32 and AArch64 Linux and on
Android/Termux.

```
arm64emu boot [options]
arm64emu chroot [options] <rootfs> <program> [args...]
```

`arm64emu --help` lists the modes, `arm64emu <mode> --help` is a mode's full
reference — options and environment variables — reflowed to your terminal.
Options are GNU-style (`--long`, `--opt=VAL`, clustered short ones), and the
engine options mean the same in both modes: `-j/--jit`, `--no-predecode`,
and the diagnostics `-d/--debug`, `--reg-trace` and `--max-insn N` (see
[Diagnostics](#diagnostics)).

> **Breaking change (2.0).** arm64emu and arm64chroot used to be two
> programs; they are one now. `arm64chroot ARGS` is `arm64emu chroot ARGS`,
> and the old `arm64emu ARGS` is `arm64emu boot ARGS`, with its `--trace` now
> `--debug` and `--no-pd` now `--no-predecode`. Every `ARM64CHROOT_*`
> variable is `ARM64EMU_*`; the guest's kernel release reads `6.1.0-arm64emu`.

## Building

```sh
make arm64emu                  # build/arm64emu: both modes (boot on 64-bit hosts)
make arm64emu32                # its 32-bit build (gcc -m32 on x86-64, armhf on an ARM host)
make test-arm64emu-boot        # boot mode's assembly self-tests
make test-arm64emu-chroot      # chroot mode's differential test suite
make help                      # every target and variant build
```

Requires only a C11 compiler and libc (`-lm -lpthread`, plus `-latomic` where
the host needs it for wide atomics — added automatically). On a 32-bit host the
build leaves boot mode out, its guest-RAM model needing `__int128`, and
`arm64emu boot` says so.

Cross-compile for a specific host (static, no runtime deps):

```sh
make CC=arm-linux-gnueabihf-gcc LDFLAGS=-static arm64emu   # 32-bit ARM host
make CC=aarch64-linux-gnu-gcc LDFLAGS=-static arm64emu     # 64-bit ARM host
```

`make CFLAGS="-O3 -flto=8"` is the fast recipe; the default is plain `-O2`
(see the Makefile for the measurements).

### Termux

```sh
pkg install clang make
make CC=clang arm64emu
```

Chroot mode is engineered to survive Android's app seccomp configuration and
includes `--link2symlink` to work around the hardlink restriction.

## Boot mode (`arm64emu boot`)

- Loads and runs the **actual** `/usr/share/qemu-efi-aarch64/QEMU_EFI.fd`
  firmware and runs it through SEC/PEI/DXE/BDS to the **UEFI Interactive Shell**
  (`Shell>`), including the 5→1s boot-countdown driven by the generic timer.
- **Boots a Linux kernel** supplied over fw_cfg: the EDK2 EFI-stub decompresses
  the kernel, exits boot services, and Linux comes up — detecting the CPU, GICv2,
  generic timer, PL011 console, PL031 RTC, PSCI and the PCIe host bridge — then
  mounts the initramfs and runs **`/init` in userspace (EL0)**, reaching an
  interactive **BusyBox shell**.

```sh
# Boot the EDK2 firmware to the UEFI shell (serial -> your terminal):
arm64emu boot --bios /usr/share/qemu-efi-aarch64/QEMU_EFI.fd

# Boot Linux via fw_cfg (the EDK2 --kernel path, no disk needed):
arm64emu boot --bios /usr/share/qemu-efi-aarch64/QEMU_EFI.fd \
              --kernel Image --initrd initramfs \
              --append "console=ttyAMA0 earlycon=pl011,0x9000000"

# Share host directories into the guest over virtio-9p (repeatable):
arm64emu boot --bios .../QEMU_EFI.fd --kernel Image --initrd initramfs \
              --virtfs /path/to/project,tag=proj \
              --virtfs /srv/data,tag=data,ro

# virtio-console on hvc0, so the guest terminal size tracks the host window:
arm64emu boot --bios .../QEMU_EFI.fd --kernel Image --initrd initramfs \
              --console virtio \
              --append "console=hvc0 earlycon=pl011,0x9000000"
```

Useful flags: `-m/--memory MB` RAM size, `--dtb FILE` supply a device tree,
`--drive IMG[,ro]` attach a virtio-blk disk (repeatable; `ro` opens the image
read-only and advertises VIRTIO_BLK_F_RO), `--net` user-mode networking
(`--netfwd tcp|udp:HOST_PORT:GUEST_PORT` forwards host ports to the guest),
`--virtfs DIR[,tag=TAG][,ro]` share a host directory over virtio-9p
(repeatable), `--console pl011|virtio[,count=N]` pick the console device
(default `pl011`) and create N virtio-consoles (`hvc0`..`hvcN-1`), each a
separate guest terminal, `--console-fd DEV=RFD[:WFD]` bind `ttyAMA0`/`hvcN` to
inherited descriptors instead of the terminal (with `--ctrl-fd FD` carrying
their window sizes — see `tests/arm64emu/boot/scripts/console_mux.c`),
`--bin FILE[@ADDR]` load a flat binary (bare-metal tests), and the
diagnostics both modes have: `--reg-trace` register trace, `--max-insn N`
stop after N instructions ([Diagnostics](#diagnostics)).

**Several guest terminals (`--console ,count=N`, `--console-fd`).** Each
virtio-console device is one more guest tty — `hvc0`, `hvc1`, … — and each can
speak through a host channel of its own instead of the shared terminal. Bind
them with `--console-fd hvc0=<fd>` (and `--console-fd ttyAMA0=<fd>` for the
UART) to descriptors the process inherited: a socketpair per terminal, plus
`--ctrl-fd` carrying their window sizes, since a socketpair has no
`TIOCGWINSZ`. Run a `getty` on each inside the guest and every terminal is an
independent login. `tests/arm64emu/boot/scripts/console_mux.c` is a working host driver.

**Host directory sharing (`--virtfs`).** Each `--virtfs DIR` exports a host
directory to the guest as a 9P2000.L filesystem on its own virtio-mmio slot.
`tag=` names the mount (default: the directory's basename); `ro` makes it
read-only (default is read-write, changes pass straight through to the host).
Repeat `--virtfs` for multiple independent shares. Mount inside the guest with:

```sh
mount -t 9p -o trans=virtio,version=9p2000.L proj /mnt/proj
```

The guest kernel needs 9p-over-virtio support (`CONFIG_NET_9P`,
`CONFIG_NET_9P_VIRTIO`, `CONFIG_9P_FS`, plus `virtio_mmio`) built in or as
loadable modules. `..` is confined to the share root; symlinks inside a share
are followed by the host, so a share is a convenience, not a security boundary.

**Console (`--console pl011|virtio`).** The default `pl011` is the PL011 UART
(`ttyAMA0`): a byte-pipe serial line, unchanged and fully deterministic.
`--console virtio` additionally attaches a **virtio-console** on its own
virtio-mmio slot, exposed to the guest as `hvc0`. It advertises
`VIRTIO_CONSOLE_F_SIZE`, so the guest terminal size follows the host window
automatically — the initial columns/rows and every later host resize (SIGWINCH)
are propagated, and full-screen programs (`vi`, `less`, `top`) redraw without a
manual `resize`. The guest kernel needs `CONFIG_VIRTIO_CONSOLE` (with
`CONFIG_HVC_DRIVER`) built in or in the initramfs.

For the resize feature to reach your shell, the guest's **interactive console must
be `hvc0`** — i.e. the kernel must boot with `console=hvc0`. There are two host
consoles sharing one terminal (the PL011 for firmware/GRUB/`earlycon`, and hvc0
for the OS), so host keystrokes are routed to **whichever console last produced
output** ("input follows output"): firmware and GRUB menus stay on the PL011, and
once the guest writes its console to hvc0 your typing (and the window size) follow
there. This means `--console virtio` never leaves you unable to type, wherever the
guest's login ends up.

Getting the login onto hvc0 depends on how you boot:

- **`--kernel` path**: the emulator adds `console=hvc0` to the kernel cmdline for you
  when `--console virtio` is set, so the login lands on hvc0 with no extra flags
  (keep `earlycon=pl011,0x9000000` for early output before the driver probes). This
  works for a guest that has its own root filesystem (an installed disk image, or a
  kernel+initramfs that mounts a real root).
- **`--drive` ISO / GRUB boot** (e.g. the stock Alpine live ISO): the kernel cmdline
  comes from the ISO's bootloader, not from `--append`, and the emulator can't
  override a `console=` baked into it. The Alpine ISO uses
  `console=tty0 console=ttyAMA0`, so its login is on `ttyAMA0` — you can still type
  (input follows it). To move the login to hvc0, edit the GRUB entry at the menu:
  press `e`, go to the `linux` line, and **append** ` console=hvc0` at the end
  (last `console=` wins), then `Ctrl-X` to boot. Keep the existing
  `console=ttyAMA0` — *append*, don't replace it: the live ISO's boot-media
  discovery relies on it, and dropping it makes early init fail (`switch_root:
  can't execute '/sbin/init'`). For the same reason, booting a live ISO's kernel
  directly via `--kernel` won't work — its root overlay is assembled only under its
  own GRUB boot. (An installed system with a normal root filesystem has neither
  restriction: just use the `--kernel` path, where `console=hvc0` is auto-added.)

Note that host resize events make `virtio` mode as non-deterministic as any
keyboard input; the default `pl011` path is untouched.

Debug/bring-up env vars (all off by default, no runtime cost when unset): the
step diagnostics both modes have, `ARM64EMU_PROF=1` hot-PC profiler,
`ARM64EMU_RING=1` recent-instruction ring buffer, `ARM64EMU_TPC=0xADDR` dump CPU
state at a PC and `ARM64EMU_COV=file` coverage-divergence finder (see below and
[Diagnostics](#diagnostics)); and boot mode's own, `ARM64EMU_DBG=N` device/IRQ +
fw_cfg/flash logging, `ARM64EMU_WATCH=0xADDR` watch writes to a physical address,
`ARM64EMU_VAW=0xVA` to a virtual one, `ARM64EMU_IABORT=1` log instruction aborts,
`ARM64EMU_SYS=1` trace the guest's mm syscalls, `ARM64EMU_HEAP=1` musl-heap
double-alloc finder.

The generic timer and PL031 RTC follow the host wall clock by default. Set
`ARM64EMU_RTCLOCK=0` to switch both to a deterministic instruction-count clock and a
fixed RTC epoch, giving a bit-for-bit reproducible boot — the mode the test gates
(`make test-arm64emu-boot`, `test-arm64emu-boot-pd`, `test-arm64emu-boot-jit`,
`fuzz-arm64emu`) pin for cross-engine comparison.

### What is implemented

- **System level**: EL0/EL1 — what EL0 may execute decided by `SCTLR_EL1` and
  `CNTKCTL_EL1`, everything else UNDEFINED there (ID registers included, for the
  guest kernel to emulate), and an EL0 `WFI`/`WFE` that would wait trapped
  unless `nTWI`/`nTWE` allow it — MSR/MRS with a large system-register file, ID
  registers matched to QEMU's cortex-a57 plus the advertised extensions, MMU
  (48-bit VA, 4 KB granule, blocks & pages, AP/XN/AF) with a software TLB and
  an instruction-fetch fast path, exception entry/return (incl. CPACR_EL1.FPEN
  traps), SVC/HVC/SMC/BRK, barriers/CLREX/WFI/WFE, TLBI, DC ZVA, AT
  S1E1R/W→PAR_EL1.
- **Platform devices**: GICv2, generic timer (periodic IRQs), PL011 serial,
  PL031 RTC, PSCI (HVC/SMC), fw_cfg (data port + DMA, legacy kernel keys),
  **Intel CFI (pflash_cfi01) NOR flash** so EDK2's UEFI variable store works,
  and empty virtio-mmio / PCIe / GICv2m / GPIO transports so probing behaves.
- **Devices**: **virtio-blk** (`--drive`, disk-backed rootfs), **virtio-net**
  (`--net`, user-mode NAT via the built-in usernet stack in
  `src/arm64emu/boot/net/`: DHCP, DNS redirect, TCP/UDP proxying and `--netfwd`
  port forwarding over plain host sockets — IPv4 only, no TAP/TUN, no external
  libraries), **virtio-9p** (`--virtfs`, host directory sharing), and
  **virtio-console** (`--console virtio`, host-tracking `hvc0` terminal size)
  are implemented over virtio-mmio, alongside the fw_cfg initramfs rootfs.

The instruction set is the CPU's, the same in both modes (see
[The CPU](#the-cpu) below).

### Differential testing against QEMU (development only)

QEMU is used purely as a *development oracle* — never linked or required to run
`arm64emu`. Two techniques made deep bring-up tractable:

1. **Same-run DTB + coverage.** Boot the same firmware in QEMU, dump its runtime
   device tree from guest memory, and capture `-d in_asm` coverage from the *same
   run* (so the DTB, including its random seed, matches). Reconstruct the full
   set of executed instruction addresses from the `OBJD-T` bytes.
2. **Coverage-divergence finder** (`ARM64EMU_COV=qemu_cov.txt`): run `arm64emu boot` with the
   identical DTB and stop at the first PC it executes that QEMU never does — i.e.
   the exact point where control flow first leaves QEMU's path. This pinpointed
   each bug (a missing device register, a wrong DTB node, an unimplemented
   instruction) instead of debugging a silent hang.

This located, among others: the virtio-mmio magic value, a too-small device
tree (missing the `cfi-flash`/PCIe/virtio nodes), the CFI flash command set, and
an exception-return bug (instruction-abort ELR pointing at the wrong PC).

### Current state

- **Firmware**: boots SEC→PEI→DXE→BDS to the **UEFI Interactive Shell**, with a
  working CFI-flash variable store.
- **Linux**: the EFI-stub kernel boots to **userspace** — kernel init completes,
  `/init` runs, and a **BusyBox shell** is reached over the serial console.
- **Performance**: ~90 MIPS by default (the predecoded tier), ~40 with
  `--no-predecode` (the plain interpreter), and ~370 with `--jit`, so a full
  distro boot is slow (see [Performance notes](#performance-notes) below).

The hard parts — a correct CPU/MMU/exception core, the device model, the CFI
flash and firmware↔platform contract (DTB, fw_cfg, PSCI), and the full
firmware→kernel→userspace hand-off — are done and validated against QEMU.

## Chroot mode (`arm64emu chroot`)

Chroot mode was developed as the runtime engine of a
[terminal app](https://github.com/sylirre/ghostty-android-terminal) for Android
OS. Loaded through JNI there (see [Embedding](#embedding)), it needs not even a
single `execve()` call, so it falls through the SELinux barrier. It can even
ignore the `noexec` mount option and the absence of the executable bit in a
file's access mode. Specifically for Android there is also the
`--link2symlink` option for emulating hard links where SELinux restricts them,
an approach similar to [proot](https://github.com/termux/proot)'s.

If SELinux restricts execmem, `--jit` falls back to memfd dual-mapping or to
the interpreter.

### Benchmark

Below is a comparison between native execution, proot, and chroot mode's
interpreter and JIT. The host is Intel i7-8665U and used benchmark program is
[byte-unixbench](https://github.com/kdlucas/byte-unixbench).

Benchmark configuration: `./Run -c 1 -i 2 arithmetic dhry syscall context1 spawn`

Higher final index means the faster execution.

| Setup                  | Benchmark Score |
|------------------------|-----------------|
| native                 | 1163.6          |
| proot                  | 270.7           |
| QEMU user              | 100.6           |
| **This (interpreter)** | 94.2            |
| **This (JIT)**         | 248.1           |

*Here we see that chroot mode with the JIT outperforms qemu-aarch64 (chroot
with binfmt_misc).*

### Usage

Command line options overview. The same reference can be obtained through
`arm64emu chroot --help`.

```
arm64emu chroot [options] <rootfs> <program> [args...]

  -h, --help              Show this help (options + environment variables)
  -v, --version           Show version and exit
      --strace            Log guest syscalls to stderr
      --strace-full       Like --strace, but decode arguments strace-style
                          (symbolic flags, quoted strings, structs, errno)
  -d, --debug             Per-instruction trace (very verbose; forces the
                          interpreter)
      --reg-trace         Compact per-instruction register trace (forces the
                          interpreter)
      --max-insn N        Stop after N instructions and dump the CPU state:
                          per guest thread, from the start of its program;
                          the process ends as SIGKILL would
  -j, --jit               Translate hot code to native code (AArch64 /
                          x86-64 / i686 / ARM32 hosts; falls back to the
                          interpreter elsewhere)
      --no-predecode      Run the plain interpreter instead of the default
                          predecoded tier (diagnostic; slower)
  -l, --link2symlink      Emulate hardlinks with tracked symlinks where the host
                          forbids link() (Android/SELinux -> EXDEV); also reads
                          and keeps the hardlink groups a rootfs installed
                          through proot (proot-distro) holds
      --shared-proc       Key the shared guest-PID registry by rootfs so `ps`/`top`
                          see guest processes across emulator invocations
      --no-dev            Disable the built-in /dev device-node passthrough; /dev
                          is served from the rootfs only (bind your own if needed)
      --no-proc           Disable the synthesized /proc; /proc is served from the
                          rootfs only (bind your own for a real host view)
  -b, --bind SRC:DST[:ro] Expose host directory `src` at guest path `dst`
                          (repeatable); append `:ro` for a read-only mount. Host
                          paths may not contain ':'.
  -E, --env VAR=VAL       Set/override a guest environment variable (repeatable).
                          The guest does not inherit the host environment: TERM
                          and COLORTERM are passed through, PATH defaults to
                          /usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:
                          /sbin:/bin and HOME to /root, everything else is
                          dropped. -E overrides any of them.
  -0, --argv0 ARG0        Override argv[0] for the guest program
  -u, --fake-id[=ID]      Present a fake identity (fakeroot-style); ID = uid | uid:gid,
                          default 0:0 (root)
      --share-abstract-sockets  Do not isolate abstract-namespace AF_UNIX sockets
                          per rootfs; share the host's global abstract namespace
                          (default: isolate, like pathname sockets)
      --host-keyring      Forward keyctl/add_key/request_key to the host keyring
                          (default: report the facility absent, as a kernel built
                          without CONFIG_KEYS does -- keyrings are not scoped to a
                          rootfs, so the guest would otherwise read, add to and
                          revoke the invoking user's own keys)
      --keep-fds          Let the guest inherit the open descriptors above
                          stdin/stdout/stderr that the caller left it (default:
                          close them before the guest starts -- guest fd is host
                          fd, so an inherited one is a live handle onto a host
                          file outside the rootfs)
  -w, --work-dir DIR      Start the guest with this directory as its working
                          directory (a guest path resolved inside the rootfs);
                          default is '/'
```

Basic usage example with Alpine Linux rootfs:

1. Obtain Alpine Linux rootfs:

   ```sh
   curl -LO https://dl-cdn.alpinelinux.org/alpine/v3.24/releases/aarch64/alpine-minirootfs-3.24.1-aarch64.tar.gz
   mkdir -p ./alpine
   tar -C alpine -zxf alpine-minirootfs-3.24.1-aarch64.tar.gz
   ```

2. Start emulator:
   ```sh
   arm64emu chroot ./alpine /bin/ash -l
   ```

The program automatically whitelists access to certain /dev nodes: null, zero,
full, random, urandom, tty, ptmx and the directories pts/ and shm/. Console
maps to the controlling terminal, stdin/stdout/stderr and fd/* to the
process's own file descriptors. These nodes also show up in a listing of /dev
(synthesized on the fly, since they are not physically present in the rootfs).
Pass `--no-dev` to disable this passthrough entirely; /dev is then served from
the rootfs only, so bind your own (e.g. `--bind /dev:/dev`) if you need it.

Many /proc entries are synthesized: maps, cmdline, comm, environ, auxv,
mounts, mountinfo, mountstats, loadavg, uptime, version, locks. The /proc/stat and
/proc/sys/kernel/overflow{u,g}id are synthesized only when they can't be read
(Android OS primarily). Pass `--no-proc` to disable the synthesis entirely;
/proc is then served from the rootfs only (bind your own, e.g.
`--bind /proc:/proc`, for the real host view).

Emulator can't be escaped and guest programs can't see processes running
on the host.

#### Host directory bindings

Command line option `--bind src:dst` exposes host directory `src` at guest
path `dst`, like a `mount --bind ./src ./dst`. The binding can be read-write
(default) and read-only, in both cases configuration is permanent and set
during emulator invocation: the guest cannot unmount a `--bind` or make a
read-only one writable, even as fake root, just as a kernel locks the mounts
a namespace inherited from a more privileged one.

Multiple directories can be shared by repeating `--bind src:dst` as needed.

Read-only mode enabled by specifying `:ro` suffix.

Examples:

```sh
arm64emu chroot --bind "$PWD:/work" ./rootfs /bin/sh          # read-write
arm64emu chroot --bind /etc/ssl:/etc/ssl:ro ./rootfs /bin/sh  # read-only
```

All bindings get registered in the guest's `/proc/mounts` and
`/proc/PID/mountinfo`. The host information about mount points is discarded.

The mount point also shows up in a listing of its parent directory — `ls /`
displays a `--bind $HOME:/host` even though `/host` is virtual and does not
exist in the rootfs. This is synthesized on the fly, so the destination need
not be created beforehand. (A destination whose *parent* directory is itself
absent from the rootfs stays reachable by name but cannot be listed.)

It is possible to use `mount --bind src dst` within guest environment if
emulator uses `--fake-id`. Remove binding by `umount dst`. Run time bindings
are not shared between multiple emulator sessions.

Caveat: the src/dst paths must not contain the colon character (`:`).

#### Sandboxes (bubblewrap, flatpak)

Guest sandbox helpers run unmodified under `--fake-id`. `mount -t tmpfs` is
backed by a fresh empty host directory bound at the mountpoint, `mount -t proc`
and `-t devpts` bind the corresponding passthrough zone, `pivot_root(2)`
re-roots like `chroot(2)` while exposing the old root at `put_old`, and a guest
that unshares a mount namespace gets a private copy of the bind table, so its
mounts stay its own:

```sh
arm64emu chroot --fake-id ./rootfs \
    bwrap --unshare-all --bind / / --proc /proc --dev /dev /bin/sh
```

Guest `seccomp` filters are the exception to the faking: they are evaluated for
real, by the same dispatcher that runs every guest syscall, so `bwrap
--seccomp` and flatpak-style syscall blacklists actually block what they say
they block.

The rest is emulation, not containment — the sandbox is a rearranged view of
the same rootfs, and the namespaces it thinks it created do not exist. It is
enough for helpers that only check whether their setup calls succeeded.

> **Do not treat a faked namespace as a security boundary.** `unshare(2)` and
> `setns(2)` report success without creating anything, and the one with a
> visible consequence is `CLONE_NEWNET`: the guest is told it owns a network
> namespace, and rtnetlink refusals are rewritten into acks so its `ip`
> commands appear to work, but ordinary `AF_INET`/`AF_INET6` sockets still
> reach the **host** network exactly as before. A guest that relies on
> `--unshare-net` (bubblewrap, flatpak) for network isolation does not have it.
> The isolation this emulator does enforce is the rootfs path containment, the
> per-rootfs abstract-socket namespace, the guest-visible process set, and
> guest `seccomp` filters — all of which hold whether or not a namespace was
> asked for.

#### Fake identity

Option `--fake-id` emulates specific UID and GID in guest environment, by
default a root user and group. The primary use case is to satisfy certain
utilities such as package managers which may refuse to work as non-root user.

```sh
arm64emu chroot --fake-id            ./rootfs /bin/sh   # guest is root 0:0
arm64emu chroot --fake-id 1000:1000  ./rootfs /bin/sh   # guest is 1000:1000
arm64emu chroot --fake-id 1000       ./rootfs /bin/sh   # uid=gid=1000
```

The whole `get*()`/`set*()` credential syscall family works with real Linux
privilege rules: a fake euid of 0 is privileged, a dropped identity cannot
regain it. Executables with setuid/setgid bit take on the file owner's
(remapped) identity on exec (unless `no_new_privs` is set), `AT_SECURE` is
set on such transitions, `capget` reports full capabilities for fake-root.

`stat` present a consistent view: a file the host reports as owned by the
real invoking user appears owned by the fake identity. `chown` or `chmod`
that the host would reject succeed. The illusion is confined to what the
emulator reports. It does not bypass host DAC, so fake-root still cannot
read a file the host user genuinely cannot and there is no persistent
per-file ownership database (a `chown` to an arbitrary third uid is accepted
but not remembered).

### What works

- **Threads**

  `clone(CLONE_VM)`: one host thread per guest thread over a shared
  software address space, with the guest tid being the real host tid (as
  guest pid is host pid). Exclusives and LSE atomics are SMP-correct (real
  host compare-and-swap) and guest memory barriers map to host fences, so
  pthread mutexes/condvars and lock-free code are correct even on weakly
  ordered ARM hosts.

- **190+ Linux system calls**

  Almost everything needed for normal operation of Linux userland.

Validated against an oracle — `qemu-aarch64`, or on an AArch64 host the CPU
itself: static and dynamic C programs, `busybox`, an Alpine (musl) rootfs with
`apk`, `bash`, and `openssl`, plus threaded and crypto workloads — all
byte-for-byte identical.

### Testing

```sh
make test-arm64emu-chroot           # differential test suite
make test-arm64emu-chroot-seccomp   # same suite with the emulator under an
                                    # Android-Oreo seccomp mimic (no device needed)
```

The suite runs on an x86-64 host and on a real AArch64 one, and works out for
itself what each provides (`tests/arm64emu/chroot/hostenv.sh`):

| | x86-64 host | AArch64 host |
|---|---|---|
| guest compiler | `aarch64-linux-gnu-gcc` | the system `cc` |
| oracle | `qemu-aarch64` | `qemu-aarch64` if installed, else the CPU |
| `make test-arm64emu-chroot32` | `gcc -m32` | armhf cross gcc, or SKIP |
| `make test-arm64emu-chroot32-jit` | the same, with `--jit` | the same, with `--jit` |

`ARM64EMU_CC` overrides the compiler and `ARM64EMU_ORACLE=qemu|native|recorded` the
oracle. Running against hardware is the stricter check of the two live modes
— it compares the emulator with silicon rather than with a second emulator —
and it is the only way the JIT's AArch64 backend is executed at all. A test
that needs an optional extension (FP16, LSE, MOPS, SHA3, ...) declares it in
a `REQUIRES:` marker and is skipped, with the reason named, on a CPU that
lacks it.

A host that can neither build the guest programs nor run any oracle — a
32-bit ARM device, whose CPU cannot execute AArch64 and for which no
`qemu-aarch64` exists (qemu-user cannot fit a 64-bit guest address space
into a 32-bit host one) — replays a **recorded** oracle instead:
`make test-arm64emu-chroot-pack` on a dev box runs the suite once with every
oracle answer captured and bundles it with the built test binaries; unpacked
on the target at the same commit, `ARM64EMU_ORACLE=recorded make
test-arm64emu-chroot` runs everything portable against the recorded answers
(tests that compare live host state skip, with the reason named). See
[chroot/android-termux.md](chroot/android-termux.md).

`make test-arm64emu-chroot` provisions the Alpine (busybox/bash) and glibc
rootfs it needs from scratch into a repo-local cache
(`build/cache/arm64emu/chroot/`) on first run; the programs it builds go to
`build/tests/arm64emu/chroot/`. The glibc tree is built offline, from the
cross sysroot or from the host's own runtime. The Alpine tree needs a one-time
network fetch.

The suite ends with a differential instruction fuzzer
(`tests/arm64emu/chroot/fixtures/insnfuzz.c`), which runs guest code against a varying
register/vector state and compares a digest of the result. It has three modes
because the useful comparisons have different oracles. `conform` replays a
table of real, allocated encodings one instruction at a time and requires an
exact match against the oracle. `chaos` feeds fully random instruction
words — where qemu is *not* an oracle, since the emulator still executes some
unallocated encodings and implements no MTE/SM3/SM4/I8MM — and instead
requires the decode cache, the plain decoder and the JIT to agree with each
other. `seq` draws from the same allocated table but runs 2..24 instructions
as one basic block, refereed the same way; that is what reaches the work a
translator does *between* instructions — register allocation and spills, the
lazy-flag window from a producer to its consumer, fused memory runs, the
block-local vector-register cache — none of which a single-instruction stub
can exercise.

Override the location of test environment with variable `ARM64EMU_TEST_ROOT`.
Wipe test environment with `make clean-testenv` (`make clean` keeps it).

### Known limitations

- `DCZID_EL0` advertises a 64-byte DC ZVA block (self-consistent with the
  implementation); it differs from QEMU's advertised value but libc memset works.
- FPCR flush-to-zero (`FZ`, `FZ16`) is implemented, including the boundary:
  the architecture judges tininess *before* rounding, so an exact result
  inside the last ULP below the smallest normal is flushed even though
  round-to-nearest carries it up onto the boundary, and one that rounds
  *down* onto the boundary from above is kept. On an AArch64 host the two
  bits are mirrored into the host's own FPCR — the same architecture, so the
  hardware does the flushing and `--jit` goes on inlining floating point;
  elsewhere it is done in software and `--jit` leaves floating point to the
  interpreter for as long as either bit is set (see
  [chroot/jit.md](chroot/jit.md)). FPSR cumulative exception flags stay
  best-effort in denormal corners; normal values and NaN
  generation/propagation match hardware.
- No vDSO (`AT_SYSINFO_EHDR` absent) — libc falls back to real syscalls.
- Big-endian hosts are not supported (the SIMD register union is little-endian).

## The CPU

Both modes run one AArch64 CPU (`lib/arm64`):

- **Integer ISA**: data-processing (immediate & register), shifts/bitfields,
  conditional select/compare, mul/div/madd/smulh/umulh, rev/rbit/clz/cls,
  **CRC32/CRC32C**, branches, the full load/store family incl. LDP/STP,
  pre/post/unscaled/register-offset, **STTR/LDTR** (true unprivileged
  semantics), **STNP/LDNP** (non-temporal pair), exclusives, acquire/release,
  **FEAT_LSE** atomics (LDADD/…/SWP/CAS/CASP/LDAPR), **FEAT_LRCPC2**
  LDAPUR/STLUR, the **FEAT_FLAGM/FLAGM2** flag-manipulation set
  (CFINV/RMIF/SETF8/SETF16/AXFLAG/XAFLAG), and **FEAT_MOPS** memory
  copy/set (CPYx/CPYFx/SETx, Option A, incl. the unprivileged forms and
  the EC 0x27 mismatch exception — in chroot mode with the kernel-side
  restart contract).
- **FP/SIMD**: scalar floating-point (convert int↔fp, FMOV, FADD/FSUB/FMUL/FDIV,
  FABS/FNEG/FSQRT, FCMP/FCCMP, FCSEL, FCVT, FRINT, FMADD/FMSUB/FNMADD/FNMSUB);
  the comprehensive Advanced-SIMD surface (loads/stores, modified immediate,
  copy, three-same, three-different, across-lanes, two-register misc,
  by-element, shift-by-immediate) with **FEAT_FP16** half-precision arithmetic;
  ARMv8 **crypto** (AES/PMULL/SHA1/SHA256 + **SHA512/SHA3**); and the v8.1–8.4
  ecosystem extensions **FEAT_RDM** (SQRDMLAH/SQRDMLSH), **FEAT_DotProd**
  (SDOT/UDOT), **FEAT_FCMA** (FCMLA/FCADD), **FEAT_FHM** (FMLAL/FMLSL) and
  **FEAT_JSCVT** (FJCVTZS) — each differential-tested against qemu-aarch64
  before its ID nibble was advertised. FPCR rounding modes are honored and
  **FPSR is live**: the cumulative exception flags (IOC/DZC/OFC/UFC/IXC), the
  sticky QC saturation bit, and the quiet-vs-signaling FCMP/FCMPE IOC split
  all accumulate architecturally across all three engines.
- **Where guest threads run concurrently** (chroot mode) the atomics and
  exclusives are host compare-and-swap and the barriers host fences; a full
  system's single CPU needs neither.
- **SP alignment** is checked in every engine: a load or store whose base is
  a stack pointer not 16-byte aligned takes the SP-alignment fault — in boot
  mode as the guest kernel sets `SCTLR_EL1.SA`/`SA0`, in chroot mode always,
  since Linux sets `SA0`, and as the kernel does, the process gets `SIGBUS`
  (`BUS_ADRALN`, `si_addr` the SP). A prefetch never checks. qemu-aarch64
  checks SP alignment in neither mode, so a program that misaligns SP runs
  under it but not on hardware or here.
- **Exclusive, ordered and atomic accesses** must be aligned to their size, a
  pair to the whole pair (the CPU has no FEAT_LSE2): a misaligned LDXR/STXR,
  LDAR/STLR, LDAPR/LDAPUR/STLUR, LD\<op\>, SWP, CAS or CASP takes an alignment
  fault in every engine and both modes, ahead of translation, and a
  store-exclusive whatever the monitor holds — in chroot mode `SIGBUS`
  (`BUS_ADRALN`, `si_addr` the address). A plain load or store is never
  checked. A CPU with FEAT_LSE2 lets an atomic or ordered access misaligned
  within 16 bytes run, so code that relies on it runs there but not here, as
  on any CPU without it.

What remains unimplemented is deliberately unadvertised (PAuth/BTI/MTE/SVE —
see [todo/TODO_OPCODES.md](todo/TODO_OPCODES.md)).

### Diagnostics

The CPU's step driver carries diagnostics that work the same in both modes,
all off by default. All except `--max-insn` run per instruction, so they force
the plain interpreter (`--jit` says so and steps aside):

- `-d/--debug`: a per-instruction trace, `<pc>: <insn>  [elN nzcv=....]`.
- `--reg-trace`: a compact one, `P<pc> <x0> … <x30> S<sp>` a line, for
  diffing against another engine or QEMU.
- `--max-insn N`: stop after N instructions, dump the CPU state (and the ring
  buffer), exactly at N in every engine, the JIT included: compare two
  engines' dumps to find the first instruction where they diverge.
- `ARM64EMU_PROF=1`: the 25 hottest PCs.
- `ARM64EMU_RING=1`: each CPU's last 96 instructions, printed when the run is
  cut short.
- `ARM64EMU_TPC=0xPC`: the registers, each time the pc hits that address.
- `ARM64EMU_COV=FILE`: stop at the first PC (after 1000 instructions) missing
  from a QEMU PC set and print the ring.

In boot mode there is one CPU and one run: the profile is printed at exit and
on SIGINT/SIGTERM, the ring on those signals, at a halt with no wake source
and wherever the run stops early.

Chroot mode runs one CPU per guest thread, in as many processes as the guest
makes:

- `--max-insn` counts per thread, from the start of the program the thread
  runs. An execve restarts the count, a new thread starts at 0, and a forked
  child carries on its parent's count.
- The first thread to reach the limit ends its process as SIGKILL would, so a
  guest parent never takes a program that was cut short for one that
  succeeded. The coverage finder ends the process the same way.
- Each thread keeps its own ring, printed when its process dies of a signal.
- The profile is one per program, counted across all its threads. It is
  printed, headed `[prof] pid <pid> <path>`, at the execve that replaces the
  program and when the process ends, a fatal signal included. A forked child
  starts with an empty profile.
- Each trace line is written whole, so concurrent threads never tear one.

## Embedding

Built with `-DANDROID_JNI`, `main()` becomes `arm64emu_main()`, which takes the
mode's word as `argv[1]` as the command line does, and fatal paths leave
through `_exit()`, so a host process can link the emulator in and enter it in
a `fork()`ed child — no `exec`, which is what Android's W^X rule requires.
`make test-arm64emu-embed` builds that configuration and runs boot mode's
suite against it.

Build it as its own shared object with `-fvisibility=hidden`: `arm64emu_main`
is the only symbol marked visible, and it is the whole interface, so the
emulator's global names stay its own whatever else the host links.

## Performance notes

The plain interpreter runs at **~40 MIPS** (boot mode's firmware boot prefix,
`-O2`, single core). A pure interpreter spends its time on the per-instruction
work itself — fetch, operand extraction, the ALU/memory operation — not on
classifying the opcode.

### Predecoded tier (the default)

A direct-threaded executor over a per-CPU cache of the *decoded* form of each
instruction (opcode id + pre-extracted operands), which self-validates against
the fetched word, so self-modifying or remapped code needs no flush; ~200 hot
forms run there and everything else falls back to `exec_a64`. ~2.46× the plain
interpreter. See [pd.md](pd.md).

### JIT (`--jit`, off by default)

An opt-in basic-block JIT translates guest code to host x86-64, AArch64, i686
or ARM32 code: **~370 MIPS (≈10×)** on boot mode's Linux-boot workload, >99.9%
of instructions native in steady state. The interpreter stays the reference —
`make test-arm64emu-boot-jit` requires byte-identical CPU state between the
engines on a deterministic-clock boot (`ARM64EMU_RTCLOCK=0`, which the gate
pins), and chroot mode's `make test-arm64emu-chroot-jit` runs its whole
differential suite under it. Design, coherence with each mode's memory (SMC
store-tracking, block/context keying, IRQ safepoints in a full system; munmap
and IC IVAU in user space) and debug knobs: [jit.md](jit.md) and
[chroot/jit.md](chroot/jit.md).

### Decoded-instruction cache — evaluated, not adopted

A **decoded-instruction cache** (the classic "memoize the decode" interpreter
optimization, *not* a JIT) was implemented and benchmarked, then reverted. It was
a direct-mapped table keyed by PC that cached the resolved group-handler function
pointer for each instruction word, re-validated against the live word on every
hit (so it stayed correct for self-modifying / decompressed code with no flush).

It was **correct** — full CPU state stayed byte-for-byte identical to the plain
interpreter over 50M firmware instructions, at a ~99.99% hit rate — but it **did
not improve throughput; it was ~5–6% slower**. Findings, kept here so the
experiment isn't blindly repeated:

- This decode tree is **shallow and branch-predictable** (a 4-bit top-level
  `switch`, then short per-group chains). The classification a cache hit skips is
  only a handful of correctly-predicted branches — cheaper than what the cache
  adds: an **indirect call** through the cached pointer (mispredicts) plus the
  **data-cache pressure** of a ~1 MB table.
- At ~90 cycles/instruction the hot loop is extremely sensitive: merely *defining*
  the function-pointer classifier inside `decode.c` (taking the handlers'
  addresses) perturbed that translation unit's codegen by ~6%, even when the cache
  was disabled.
- The real lever for a pure interpreter is **operand pre-decode** — caching a
  fully decoded form dispatched via a dense `switch`, so a hit skips operand
  extraction too, not just classification. That is exactly the predecoded
  tier above. A function-pointer decode cache, by contrast, is not the path
  forward.

## Architecture

Deeper notes live alongside this guide: the engines ([pd.md](pd.md),
[jit.md](jit.md), [parity.md](parity.md)), and chroot mode's design under
[`chroot/`](chroot/internals.md) — the guest memory model and host-ordering
discipline, the syscall layer and `--fake-id`, signal/job-control and the
process model, and a catalog of host-portability pitfalls.

```
lib/arm64/        the AArch64 emulation both modes run:
  types.h         fixed-width types and small helpers
  cpu.h           CPU state, PSTATE bits, core interpreter API; the CPU's mode
                  (a full system or user mode, one CPU or concurrent guest
                  threads) and ops (what the library asks of the mode); run
                  control (the engines' contract with the run loop)
  cpu.c           fetch/decode/execute driver, interrupt delivery, SPSR
                  pack/unpack (ERET), condition codes, dump, the step
                  diagnostics: register trace, hot-PC profiler, per-CPU ring
                  buffer, coverage-divergence finder
  mem.h           guest memory as the core reaches it: typed accesses, fetch and
                  mem_host_ptr inline over the CPU's translation caches (D-TLB,
                  fetch cache); the slow paths are the mode's
  decode.c        A64 decoder: integer/branch/load-store, CRC32, AdvSIMD ld/st,
                  LSE atomics and exclusives (host-atomic where guest threads
                  run concurrently)
  predecode.{h,c} the pre-decode classifier (word -> PDEnt) both faster tiers use
  pdrun.{h,c}     the predecoded tier, the default direct-threaded engine
  jit/            the JIT: jit.c (the runtime: per-CPU code cache, block
                  chaining, jump cache, invalidation by page, W^X/memfd
                  fallback), ir.h, frontend.c (per-block decode -> linear IR ->
                  liveness / flag fusion), backend_x86_64.c and backend_a64.c
                  (register-allocating single-pass emitters), backend_x86_32.c
                  and backend_arm32.c (ILP32 emitters: guest 64-bit values as
                  host register-pair halves, only destinations allocated)
  sysreg.{h,c}    MSR/MRS, ID registers, generic-timer registers, DC ZVA, TLBI,
                  AT, what EL0 may run
  exec_fpsimd.c   FP/Advanced-SIMD/crypto (+ FPCR rounding, 32-bit-host-safe)
  esr.h           ESR_ELx exception classes + syndrome construction
src/arm64emu/     what both modes share:
  main.c          the entry: hands the command line to the mode it names
  cli.{h,c}       the help renderer, option helpers, version, engine options
                  (the diagnostics' --reg-trace, --max-insn and environment
                  among them)
  embed.h         the embedded build's exit -> _exit shadow
src/arm64emu/boot/  boot mode:
  main.c          its CLI, image loading, the run loop, signal-flush of
                  diagnostics
  diag.{h,c}      guest-kernel diagnostics on the step driver's hooks: the
                  mm-syscall trace and the musl-heap tracker
  mmu.c           AArch64 stage-1 walk (4 KB granule) + software TLB, and the
                  slow paths of lib/arm64's mem.h (D-TLB and fetch-cache fills)
  exception.c     exception entry (VBAR vectors, ESR/FAR/ELR/SPSR, DAIF masking)
  memory.c        physical bus: RAM + NOR-flash CFI command set + MMIO dispatch
  tty.c           raw-terminal serial console via termios
  console.c       host endpoints the console devices speak through: the shared
                  process terminal, or an inherited fd pair
  platform.c      device instantiation, DTB placement, kernel load, tick/WFI
  devices/        gicv2, timer, pl011, pl031, psci, fwcfg, virtio_blk (--drive),
                  virtio_net (--net), virtio_9p (--virtfs), virtio_console (--console)
  fdt/            device tree: virt.dts (QEMU virt tree; compiled to virt_dtb.h,
                  embedded) + fdt.c, which sizes its /memory node to --memory
  jit/            coherence.c: the JIT's coherence with physical memory
  net/            usernet, the built-in user-mode NAT stack behind virtio-net
                  (core/udp/tcp; no TAP, TUN, privileges or external libraries)
src/arm64emu/chroot/  chroot mode:
  mmu.h mem.c     guest address space: 2-level software page table
                  (guest 4 KB page -> host pointer | prot) behind lib/arm64's
                  mem.h (its slow paths, each thread's translation caches), guest
                  mmap/brk/mprotect, copy_to/from_guest, guest_lend (a large
                  transfer's backing handed to the host syscall, pinned). Portable
                  to 32-bit hosts: guest VAs never become host pointers except
                  through the table.
  exception.c     pending-exception recorder (SVC/abort/undef/BRK -> run loop,
                  through the CPU's attention)
  loop.c          run loop + exception dispatch + signal delivery point +
                  the thread call-out safepoint (stop_gen) + EL0's view of the
                  ID registers (the kernel's MRS emulation) + the step
                  diagnostics' ends (--max-insn's stop, the profile per
                  program, the ring at a fatal signal)
  jit/            the JIT's coherence with the guest address space:
                  coherence.{h,c}, the thread registry, the sticky code-page
                  map, invalidation by VA range, execve/fork
  elf.c           ELF64 loader, PT_INTERP, initial stack/auxv/HWCAP, sigtramp page
  path.c          rootfs containment resolver handing every syscall a pinned
                  parent-directory fd rather than a re-resolvable path string,
                  bind table (runtime mount/umount/pivot_root; process-shared,
                  private per faked mount namespace), tmpfs backing dirs,
                  /proc & /dev special cases
  l2s.c           --link2symlink: the host lib/link2symlink runs on
  syscall.c sys_*.c   dispatcher + per-area handlers (~225 syscalls)
  strace.c        --strace-full argument decoder (flags, strings, structs, errno)
  sys_procfs.c    synthesized guest /proc (maps, cmdline, mounts, stat, the
                  writable id maps of a faked user namespace — own or a
                  child's, per-process status rebuilt line by line, every
                  task's personality, ...)
  sys_seccomp.c   seccomp(2): classic-BPF evaluator over guest seccomp_data,
                  run by the dispatcher for every guest syscall
  proctab.c       shared-memory guest-PID registry (cross-process ps/top view,
                  the id maps of a faked user namespace, so a parent can write
                  its child's, each process's seccomp state, which its status is
                  read for, the host tasks in its thread group that are not
                  guest threads, which the guest must not be shown, its
                  threads' personalities, and a siginfo inbox for a host that
                  cannot carry a queued siginfo whole)
                  + unified IPC broker: portable System V IPC
                  — memfd-backed shared memory, semaphores (blocking semop,
                  SEM_UNDO), message queues — and any thread personality that
                  differs from its process's
  ptracetab.c     cross-process ptrace(2): tracer<->tracee link registry + futex
                  mailbox (tracee serves PEEK/POKE/GETREGSET/CONT while stopped)
  signal.c        host capture -> guest rt_sigframe / rt_sigreturn
  thread.h        per-thread state (CPU is per-thread; Machine is shared)
  guest_abi.h     arm64 syscall numbers + explicit guest struct layouts
  main.c          its CLI, rootfs setup, initial exec
tests/arm64emu/boot/    self-checking assembly tests, the engines' consistency
                        and boot-log gates, the fuzzer, QEMU differential helpers
tests/arm64emu/chroot/  asm + C differential tests, run_tests.sh; hostenv.sh picks
                        the compiler and the oracle (qemu-aarch64, or an AArch64
                        host's CPU)
docs/arm64emu/          this guide; jit.md, pd.md, parity.md, todo/; chroot/
```

**Execution.** Three tiers share one run loop and return contract in each
mode. By default the **predecoded tier** direct-threads ~200 hot instruction
forms and everything else falls back to `exec_a64`. `--no-predecode` and any
per-instruction debug facility select the plain `exec_a64` step. The optional
`--jit` adds a translating tier above both: it compiles guest basic blocks to
native host code and falls back to `exec_a64` for anything it does not emit.
It is off by default and the interpreter stays the source of truth. See
[jit.md](jit.md), [chroot/jit.md](chroot/jit.md) and
[chroot/architecture.md](chroot/architecture.md).

**Chroot mode's guest memory model.** A 47-bit guest address space is mapped
by a two-level page table whose leaves are `host_pointer | R/W/X` flag bits.
Because guest and host addresses are fully decoupled, the same layout works on
a 32-bit host (where a leaf is a 4-byte `uintptr_t`). The instruction-fetch
fast path caches one page's host pointer per thread.

**Portability.** No 128-bit host types are required by chroot mode (64×64→128
multiply and the saturating-SIMD helpers use pure 64-bit arithmetic);
little-endian host assumed (all four targets qualify). The `make arm64emu32`
build runs natively on x86-64 so ILP32-host correctness — struct conversions,
`uintptr_t` leaves, wide atomics — is exercised continuously, not just on real
32-bit hardware.

## License

**Apache License 2.0** — see [LICENSE](../../LICENSE) and [NOTICE](../../NOTICE).

arm64emu is original, clean-room code (C11, libc/POSIX only) and shares no
source with QEMU, EDK2, or the Linux kernel; it interoperates with them purely
through their public hardware/software interfaces. The instruction set,
FP/Advanced-SIMD, reciprocal estimates, and cryptographic extensions are
implemented from the *Arm Architecture Reference Manual* pseudocode;
`qemu-aarch64` and `qemu-system-aarch64` are used only as differential-testing
oracles and are neither included nor linked. EDK2 firmware and Linux
kernel/initramfs images are supplied by you at runtime and are neither bundled
nor distributed here. Apache-2.0 was chosen for its permissive terms plus an
explicit patent grant. Each source file carries an `SPDX-License-Identifier:
Apache-2.0` tag; `src/arm64emu/boot/fdt/virt.dts` is QEMU-generated
device-tree data and is exempt (see NOTICE).
