# arm64emu

Guidelines for AI agents when working with arm64emu (`src/arm64emu/`). The
repository-wide ones — layout, build, test discipline, conventions, commit
messages — are in the root `AGENTS.md`; this file adds what is particular to
arm64emu.

## Overview

AArch64 emulator with two modes, one program:

* `arm64emu boot` — a full system: an AArch64 machine compatible with QEMU
  'virt' (GICv2, generic timer, PL011, PL031, PSCI, fw_cfg, flash, virtio-mmio
  devices), booting EDK2 firmware and Linux.
* `arm64emu chroot` — Linux user space: runs the programs of an AArch64 rootfs
  in unprivileged, isolated chroot environments, with directory binding, a
  fake user id and hardlink emulation via symlinks.

Both modes run the same CPU (`lib/arm64`): the plain interpreter (the
reference), the predecoded tier (the default) and the JIT (`--jit`).

Target platforms are regular Linux distributions (Musl, GNU libc) and Android
OS (Bionic, restricted by SELinux and seccomp). Chroot mode builds on 32-bit
hosts too (i686, ARM32); boot mode needs a 64-bit host.

## Tooling

Packages required to compile the program and run its suites:

* gcc or clang
* make
* aarch64-linux-gnu-gcc (aarch64-linux-gnu-gcc-13) — builds the guest programs
  of both modes' suites on an x86_64 host
* qemu-aarch64 — chroot mode's differential oracle, and proot's interpreter
  for the Alpine test rootfs (qemu-aarch64-static is preferred there when
  installed); boot mode's `make test-arm64emu-boot-jit-a64` runs under it
* qemu-system-aarch64, python3, nc, cpio, md5sum, curl — boot mode's oracle
  and network scripts
* an Alpine Linux kernel and initramfs (aarch64) — boot mode's consistency
  and boot-log gates
* proot, expect — chroot mode's test rootfs bootstrap and PTY tests
* gcc-multilib — the `make test-arm64emu-chroot32` ILP32-host build

Chroot mode's suite runs on an x86_64 host and on a real AArch64 one;
`tests/arm64emu/chroot/hostenv.sh` decides what each provides. On an AArch64
host the system `cc` already builds guest programs and the CPU is itself the
oracle, so the cross compiler, qemu-aarch64 and gcc-multilib are not needed
(`qemu-aarch64` is still used when installed; `ARM64EMU_ORACLE=native` forces
hardware). `make test-arm64emu-chroot32` there wants an armhf cross compiler and an AArch64 CPU that implements AArch32 at
EL0, and skips with a reason when either is missing.

The ARM32 code generator has no CI (the AArch64 runners do not implement
AArch32 at EL0), so on an x86_64 host it is exercised through binfmt/qemu-arm
with an armhf cross compiler — the emulator is ARM32 code under emulation while
the oracle still runs natively, so the differential comparison is intact:

```sh
make ARM64EMU_M32CC=arm-linux-gnueabihf-gcc "ARM64EMU_M32FLAGS=-static" test-arm64emu-chroot32-jit
```

Static matters: `QEMU_LD_PREFIX` is contended between the aarch64 guest sysroot
(which the suite needs for the dyn tests) and the armhf one (which a dynamic
armhf emulator needs); a static emulator needs neither. That checks the emitter;
only a real armv7 device checks that the silicon agrees with what it emits.
**Both tiers gate at zero failures in either engine** — the qemu-arm one used to
be read against a ~44-failure baseline, but every one of those turned out to be
an emulator bug rather than the tier's slowness (see docs/arm64emu/chroot/jit.md). The rows
qemu-arm itself cannot run — it asserts on `mremap(old_size=0)` (the
emulator's own use of it, and the guest's), writes nothing for
`getsockopt(SO_RCVTIMEO)`, ignores `waitid`'s rusage argument, drops an iovec
segment it cannot lock, caps POSIX timers at 32, mistakes `SO_GET_FILTER` for
an int option, refuses the subreaper/THP prctls, installs every SCM_RIGHTS
descriptor into a short control buffer, ignores `MADV_REMOVE`, grows no stack
from a system call's copy, hands an `F_SETSIG` signal no descriptor, aborts on
a select past descriptor 1023, answers ENOTTY for `TCGETS2` on a tty, copies an ioctl's argument in before the kernel can refuse the descriptor, and cannot start where `vm.mmap_min_addr` reads
0 (the full list is in tests/arm64emu/chroot/hostenv.sh) — carry a `NEEDS-HOST-SYSCALL:` marker (in a C
test or a fixture alike) and skip there, naming what is missing; they run in
full on real armv7 silicon.

## Project structure

```
Makefile                  (repository root) `make arm64emu` -> build/arm64emu,
                          both modes (boot mode on a 64-bit host only); also
                          arm64emu32 (ILP32 host), arm64emu_asim (Android
                          behavior), arm64emu-embed, arm64emu-a64.
                          C11 + libc/POSIX + -lm/-lpthread only, no external
                          deps. CC/CFLAGS/ARM64EMU_OPT overridable
lib/arm64/                the AArch64 emulation both modes run (compiled once
                          per host compiler; includes no program header):
  types.h                 u8..u64 / bool / small helpers
  cpu.h                   CPU state struct, PSTATE bits, core interpreter API;
                          the CPU's mode (c->eng.sys/smp: a full system or
                          user mode, one CPU or concurrent guest threads) and
                          ops (c->eng.ops, CpuOps: what the library asks of
                          the program -- exception entry, the memory slow
                          paths, the timer, PSCI, the JIT's coherence);
                          g_env_prefix/arm64_getenv (the knobs' prefix);
                          run control: the engines' contract with the run
                          loop (the attention byte, cpu_irq_poll)
  cpu.c                   reset, cpu_step (fetch/decode/execute driver),
                          interrupt delivery (cpu_take_interrupt), SPSR
                          pack/unpack, condition codes, cpu_dump, and the
                          step diagnostics: register trace, coverage-
                          divergence finder, per-CPU ring buffer, hot-PC
                          profiler (one per process, atomic), the program's
                          hooks
  mem.h                   guest memory as the core reaches it: typed accesses,
                          fetch and mem_host_ptr, inline over the CPU's
                          translation caches (c->eng: the D-TLB, whose entry
                          layout the JIT probes inline, and the fetch cache);
                          the slow paths and TLB maintenance are the program's
                          (its ops)
  sysreg.h                MSR/MRS/SYS
  sysreg.c                system-register file: MSR/MRS/SYS/MSR-immediate, ID
                          registers (what features the guest kernel probes),
                          generic-timer registers, DC ZVA, TLBI, AT, CPACR FP
                          trap; each mode's reset configuration (sysreg_init)
  decode.c                A64 decoder/executor, part 1 — DP-immediate,
                          DP-register, branches/exceptions/system, the whole
                          load/store family, exclusives, LSE atomics,
                          FEAT_MOPS, CRC32, AdvSIMD load/store.
                          exec_a64() is the top-level dispatch and the
                          correctness reference for all three engines.
                          The CPU's mode picks the atomics' algorithm
  predecode.h             PDEnt: dense opcode id + pre-extracted operands
  predecode.c             the pre-decode classifier (mirrors decode.c; anything
                          uncertain stays PD_GENERIC -> exec_a64), the
                          predecoded tier's and the JIT frontend's decoder
  pdrun.h, pdrun.c        pd_run, the default computed-goto tier over the
                          CPU's decode cache of PDEnts (pinned to -O2 here)
  jit/                    the JIT (--jit, off by default):
    jit.h                 public interface: jit_run, the program's uses of a
                          CPU's JIT, and the JitCoherence it supplies
    jit_priv.h            internals shared by the runtime, the backends and
                          the program's hooks: JitEnv, the block tables
    jit.c                 the runtime: per-CPU code cache, block tables,
                          dispatch loop, chaining, jump cache, memory slow
                          paths, the <prefix>JIT_* knobs (ARM64EMU_JIT_* here)
    ir.h                  the IR: op set, virtual registers, block layout
    frontend.c            basic-block discovery + PDEnt -> IR translation
                          (guest-shaped concerns are all resolved here)
    backend_x86_64.c      x86-64 code generator
    backend_a64.c         AArch64 code generator (same-ISA host)
    backend_x86_32.c, backend_arm32.c  the ILP32 hosts' (chroot mode's: the
                          only one a 32-bit host builds)
  exec_fpsimd.c           A64 decoder/executor, part 2 — scalar FP, the full
                          Advanced-SIMD surface (three-same/-diff/-extra,
                          by-element, across-lanes, two-misc, shift-imm, copy,
                          permute/TBL/EXT, scalar AdvSIMD), FP16, ARMv8 crypto
                          (AES/PMULL/SHA1/SHA256/SHA512/SHA3), FPSR flag fold
  esr.h                   ESR_ELx exception classes + syndrome construction
src/arm64emu/             what both modes share:
  main.c                  the entry: hands the command line past its first
                          word to the mode it names (boot_main, chroot_main);
                          the program's --help; arm64emu_main in the embedded
                          (-DANDROID_JNI) build
  cli.h, cli.c            both modes' command line: the help renderer, the
                          option helpers, the version, the engine options
                          (-d/--debug, -j/--jit, --no-predecode, --reg-trace,
                          --max-insn), the step diagnostics' environment
                          (ARM64EMU_PROF/RING/TPC/COV) and the engines' knobs
                          in --help (ARM64EMU_JIT_*, PD_MAX)
  embed.h                 the -DANDROID_JNI build's exit -> _exit shadow,
                          forced into its every unit (-include)
src/arm64emu/boot/        boot mode: a full system, the QEMU 'virt' machine
                          (`arm64emu boot`)
  main.c                  the mode's entry (boot_main): its CLI, --help text (the
                          option and env-var reference), image loading, the run loop,
                          signal-flush of diagnostics, ARM64EMU_* debug env vars
  diag.h, diag.c          guest-kernel diagnostics on the step driver's hooks:
                          the mm-syscall trace (ARM64EMU_SYS) and the musl-heap
                          tracker (ARM64EMU_HEAP)
  exception.c             exception entry (exception_vector, the ops'
                          exception_take): VBAR vectors, ESR/FAR/ELR/SPSR
                          banking, DAIF masking
  mmu.h                   the MMU behind lib/arm64's mem.h: translation, the
                          CPU's mode, ops and caches (mmu_cpu_init: a
                          4096-entry D-TLB tagged with the TLB flush
                          generation)
  mmu.c                   stage-1 walk (EL1&0, 4 KB granule), software TLB,
                          mem.h's slow paths (D-TLB/fetch-cache fills, the
                          bus), TLBI/AT; the machine's CpuOps (system_ops)
  machine.h               Machine struct + virt platform constants
                          (FLASH/RAM base and size, device windows)
  memory.c                physical bus: RAM, Intel CFI NOR flash command set,
                          MMIO dispatch, block read/write
  platform.c              platform wiring: instantiate the virt devices, place
                          the DTB, load kernel/initrd via fw_cfg,
                          machine_reset / machine_tick / WFI wait
  tty.h, tty.c            raw-terminal serial console (termios), window size
  console.h, console.c    host endpoints for the guest's console devices: the
                          shared process terminal (via tty.c) or an inherited
                          fd pair with pushed-in geometry and buffered,
                          never-blocking output
  devices.h               device structs + virt MMIO map and INTID constants
  devices/
    gicv2.c               GICv2 distributor + CPU interface (single CPU)
    timer.c               architected generic timer (host clock, or the
                          deterministic instruction-count clock at ARM64EMU_RTCLOCK=0)
    pl011.c               PL011 UART: TX -> stdout, RX <- stdin, RX/TX IRQs
    pl031.c               PL031 RTC (host wall clock or fixed epoch)
    psci.c                PSCI over the SMC/HVC conduit, incl. SYSTEM_RESET
    fwcfg.c               QEMU fw_cfg: data port + DMA, legacy kernel keys
    virtio_blk.c          --drive, repeatable (disks take slots 1,2,3…)
    virtio_net.c          --net / --netfwd, backed by src/arm64emu/boot/net/ (slot 0)
    virtio_9p.c           --virtfs host directory share (9P2000.L)
    virtio_console.c      --console virtio / ,count=N: hvc0..hvcN-1, one guest
                          terminal per device, each on its own host endpoint
  fdt/
    virt.dts              device tree source (QEMU virt tree)
    virt_dtb.h            dtc output, embedded in the binary — regenerate,
                          never hand-edit
    fdt.h, fdt.c          minimal in-place FDT editing: sizes the /memory node
                          to --memory, in the built-in tree and in a --dtb file
  jit/
    coherence.c           the JIT's coherence with physical memory (lib/arm64
                          jit.h's JitCoherence, jit_phys_coherence): the
                          code-page bitmap that keeps
                          stores to translated pages off the D-TLB fast path,
                          IC IVAU by physical address, warm reboot
  net/                    usernet: built-in user-mode NAT stack (no libslirp)
    usernet.h             device-facing API (input/poll/output callback)
    usernet_priv.h        internals: fixed topology, shared helpers
    usernet_core.c        Ethernet/ARP/IPv4, ICMP echo, poll driver, pcap, stats
    usernet_udp.c         DHCP server, UDP NAT flow table, DNS redirect
    usernet_tcp.c         terminating TCP proxy over host sockets
src/arm64emu/chroot/      chroot mode: Linux user space (`arm64emu chroot`)
  mmu.h mem.c             Guest address space: 2-level software page table
                          (guest 4 KB page -> host pointer | prot) behind
                          lib/arm64's mem.h (its slow paths; each thread's CPU
                          set up by as_cpu_attach: its mode -- user mode, guest
                          threads running concurrently --, the process's CpuOps
                          (user_ops) and translation caches), guest
                          mmap/brk/mprotect, copy_to/from_guest, guest_lend (a
                          large transfer's backing handed to the host syscall,
                          pinned). Portable to 32-bit hosts: guest VAs never
                          become host pointers except through the table.
  exception.c             Pending-exception recorder (exception_record, the ops'
                          exception_take: SVC/abort/undef/BRK -> run loop,
                          through the CPU's attention)
  loop.c                  Run loop + exception dispatch + signal delivery point
                          + the thread call-out safepoint (stop_gen) + EL0's
                          view of the ID registers (the kernel's MRS emulation)
                          + the step diagnostics' ends: --max-insn's stop (the
                          process as SIGKILL ends it), ARM64EMU_PROF per
                          program, the ring at a fatal signal
  jit/                    The optional --jit's coherence with the guest address
                          space (AArch64, x86-64, i686 & ARM32 hosts):
    coherence.c coherence.h
                          lib/arm64 jit.h's JitCoherence (jit_va_coherence): the
                          thread registry and cross-thread interrupt, the sticky
                          code-page map, invalidation by VA range
                          (munmap/mprotect/IC IVAU), execve/fork
  elf.c                   ELF64 loader, PT_INTERP, initial stack/auxv/HWCAP,
                          sigtramp page
  path.c                  Rootfs containment resolver: the walk, plus the pin
                          that hands each syscall a parent-directory fd + final
                          component instead of a path string the host would re-
                          resolve (no concurrent rename can redirect it); bind
                          table (mount/umount/pivot_root; process-shared,
                          private per faked mount namespace), tmpfs backing
                          dirs, /proc & /dev special cases
  l2s.c                   --link2symlink: the host the shared engine
                          (lib/link2symlink) runs on -- every host path it names
                          mapped back to its guest name and pinned like a guest
                          path
  syscall.c sys.h         Dispatcher (x8=nr, x0..x5=args) + helpers shared by
                          all sys_*.c
  strace.c strace.h       --strace-full argument decoder: per-syscall arg-type
                          table -> symbolic flags, quoted strings, struct
                          pretty-printers, errno-named returns
  sys_file.c              File & fd syscalls (every path arg via resolve_pin
                          containment); xfer_begin/xfer_end, where every
                          read/write/send/recv finds host memory for its bytes
  sys_mm.c                Memory-management syscalls over the guest address
                          space (mem.c)
  sys_ipc.c               System V IPC syscalls (shm + semaphores + message
                          queues) over the portable IPC broker; shm maps segment
                          fds with guest_map_file, no host SysV IPC or /dev/shm
  sys_proc.c              Process syscalls (fork/exec/wait/kill, CLONE_VM
                          threads; vfork as a fork whose parent waits and whose
                          child's writes are carried back); the guest thread
                          registry (every live thread's robust-list head and
                          personality); execve's cooperative de_thread
                          (rendezvous siblings at a safepoint, land the new
                          image on the main thread); a main thread that exit(2)s
                          while siblings run parks as the kernel's zombie leader
                          instead of ending the process
  sys_sig.c               Signal syscalls (rt_sigaction / sigprocmask
                          dispositions -- the guest's blocked set is the host
                          thread's, signalfd is a host signalfd with translated
                          records)
  sys_time.c              Time / clock / timerfd syscalls
  sys_net.c               Socket syscalls (rootfs-aware AF_UNIX paths, abstract-
                          socket isolation)
  sys_netlink.c sys_netlink.h
                          AF_NETLINK / NETLINK_ROUTE emulation (proot-style):
                          AF_UNIX fallback when the host denies netlink (probed
                          incl. a write, per-message-type LSM policies),
                          covering read/write and their vector forms as well as
                          the socket calls, replies per-socket and delivered a
                          datagram at a time (NLMSG_DONE on its own), with
                          readiness carried by the self-connected stand-in so
                          poll/select/epoll work, plus rtnetlink-refusal-to-ack
                          rewriting for a guest whose CLONE_NEWNET was faked
  sys_misc.c              Misc syscalls: randomness, rlimits (AS/DATA/STACK held
                          per-process and enforced against the guest's own
                          address space, never the host's -- capping the host
                          caps the emulator), sysinfo, futex basics
  sys_procfs.c            Synthesized guest /proc (maps, cmdline, mounts, stat,
                          writable uid_map/gid_map/setgroups of a faked user
                          namespace -- its own or, the usual arrangement, a
                          child's, per-process status rebuilt line by line so
                          TracerPid/Seccomp/Sig*/Cap* describe the guest and not
                          the emulator, every task's personality, ...)
  sys_ptrace.c            ptrace(2) syscall shim (arm64 ABI decode onto the
                          ptracetab.c control channel)
  sys_seccomp.c           seccomp(2): classic-BPF evaluator over guest
                          seccomp_data, run by the dispatcher for every guest
                          syscall (a host filter would see the emulator's own
                          syscalls instead)
  proctab.c               Shared-memory guest-PID registry (cross-process ps/top
                          view, plus the id maps of a faked user namespace,
                          which a parent writes for its child, plus each
                          process's seccomp state, which anyone reading its
                          status needs, plus the host tasks in its thread group
                          that are not guest threads -- an interposer's own --
                          so the guest is never shown one, plus its main
                          thread's and default personality, plus its siginfo
                          inbox, which a sender on a host that cannot carry a
                          queued siginfo whole posts the siginfo to) + unified
                          IPC broker daemon backing System V IPC: owns shm
                          memfd/file backings (handed out over SCM_RIGHTS) and
                          all semaphore/message-queue state, parks blocking
                          semop/msgsnd/msgrcv waiters, applies SEM_UNDO at
                          process death, holds any thread personality that
                          differs from its process's, self-cleans on idle
  ptracetab.c ptrace.h    Cross-process ptrace(2): shared tracer<->tracee link
                          registry + futex mailbox (tracee services
                          PEEK/POKE/GETREGSET/CONT about itself while parked at
                          a stop)
  signal.c                Host capture -> guest rt_sigframe / rt_sigreturn
  machine.h thread.h      Per-process shared Machine state + per-thread state
                          (CPU is per-thread; Machine is shared)
  guest_abi.h             ARM64 syscall numbers + explicit guest struct layouts
  main.c                  the mode's entry (chroot_main): its CLI, --help text,
                          rootfs setup, initial exec
tests/arm64emu/boot/
  run_tests.sh            builds and runs the asm suite (ARM64EMU_EMU_FLAGS picks the
                          engine; ARM64EMU_EMU/ARM64EMU_RUNNER pick an alternate binary)
  run_pd_consist.sh       predecoded tier vs interpreter: identical serial
                          output + CPU state at exact --max-insn checkpoints
  run_jit_consist.sh      same comparison for --jit
  run_bootlog_gate.sh     full 1.6B-insn firmware+Linux boot-log diff
                          (mandatory after JIT/pd frontend changes)
  run_fuzz_engines.sh     cross-engine differential fuzzing over random blocks
  asm/                    self-checking tests; x0=0 on success, else check id.
                          Flat --bin images unless the test needs MMIO devices,
                          which only exist under --bios (marked below; the list
                          lives in run_tests.sh's per-name case). "dual-mode"
                          tests also build as a Linux user binary with
                          -DUSERMODE so qemu-aarch64 can vet the expected values
    m1_int.S              integer ISA
    m2_mmu.S              MMU enable, VA access, SVC exception
    m4_fpsimd.S           MOVI/DUP/INS/UMOV/SMOV + vector load/store
    m5_ldsingle.S         AdvSIMD single-structure / replicate load-store
    m6_cow.S              copy-on-write fault handling via data aborts
    m7_crypto.S           REV + SHA-1/SHA-256/AES
    m8_simd.S             AUTO-GENERATED AdvSIMD differential vs qemu
    m9_simd_int.S         AUTO-GENERATED integer AdvSIMD differential
    m10_simd_scalar.S     AUTO-GENERATED scalar AdvSIMD differential
    m11_reboot.S          PSCI SYSTEM_RESET warm reboot (RAM survives)
    m12_vcon.S            virtio-console probe (--bios --console virtio)
    m13_lse.S             FEAT_LSE atomics, CAS/CASP, LDAPR (dual-mode)
    m14_spalign.S         SP-alignment fault (SCTLR_EL1.SA)
    m15_rtc.S             PL031 match/alarm interrupt (--bios)
    m16_laxdecode.S       unallocated encodings must UNDEF
    m17_at_unpriv.S       LDTR/STTR EL0 view + AT S1E*R/W -> PAR_EL1
    m18_fptrap.S          CPACR_EL1.FPEN traps (EC 0x07), EL0 drop
    m19_v84ext.S          LRCPC2 / JSCVT / FLAGM+FLAGM2 (dual-mode)
    m20_ext_simd.S        RDM / DotProd / FCMA / FHM + FRINTX/I (dual-mode)
    m21_mops.S            FEAT_MOPS CPYx/CPYFx/SETx contract (dual-mode)
    m22_fpsr.S            FPSR cumulative flags, QC, FCMP-vs-FCMPE (dual-mode)
    m23_jitpar.S          engine-parity regressions found by the fuzzer
    m24_gic.S             GICv2 distributor + CPU interface (--bios)
    m25_timer.S           generic timer reads, IRQ delivery, WFI (--bios)
    m26_irq.S             IRQ/sync exception entry, banking, ERET (--bios)
    m27_excl.S            exclusive monitor LDXR/STXR/LDXP/STXP/CLREX
    m28_uart.S            PL011 flags, masks, TX IRQ via the GIC (--bios)
    m29_psci.S            PSCI conduit queries over HVC
    m30_sysreg.S          ID-register constants + MSR/MRS round-trip
    m31_mmu2.S            MMU corners: TBI, EPD, TxSZ fault, 4-level walk
    m32_dtbmem.S          the DTB's /memory node tracks --memory (--bios,
                          run with --memory 384)
    m33_simdshift.S       AdvSIMD SLI/SRI shift-insert extremes, every lane
                          width incl. SRI by the element width (dual-mode)
    m34_fpcrfz.S          FPCR.FZ / FPCR.FZ16 flush-to-zero
    m35_el0priv.S         what EL0 may execute of the system instructions
    m36_sevl.S            SEV / SEVL set the event register in every tier
    m37_align.S           the alignment fault of a misaligned exclusive,
                          ordered or atomic access (no FEAT_LSE2), EL1 and EL0
  scripts/
    console_mux.c         dev harness / worked example for --console-fd and
                          --ctrl-fd: a socketpair per guest terminal, output
                          demultiplexed by tab (run by hand, needs an image)
    fuzz_gen.c            random-block generator for run_fuzz_engines.sh
    diff_qemu.sh          per-instruction PC-stream diff vs QEMU (dev oracle)
    net_smoke.sh          end-to-end usernet test (dhcp/dns/wget/fwd/upload)
  net/                    guest side of net_smoke.sh
    mk_guest_initrd.sh    build the BusyBox test initramfs from a stock Alpine one
    guest-init            guest /init; runs the phase named by aetest=
    udhcpc.script         minimal DHCP event script
tests/arm64emu/chroot/    chroot mode's differential suite: asm + C tests and
                          fixtures against an oracle, run_tests.sh;
                          hostenv.sh picks the compiler and the oracle
                          (qemu-aarch64, or an AArch64 host's own CPU)
docs/arm64emu/            README.md, the user guide (both modes); jit.md,
                          pd.md, parity.md, todo/ (the engines); chroot/
                          (chroot mode's design and internals)
```

## Conventions and rules

In addition to the root `AGENTS.md`:

* Keep in sync the interpreter and JIT parts of emulator where possible.
* What both modes share is shared: an option, a knob or a facility both have
  is one piece of code (`cli.c`, `lib/arm64`), not one per mode.
* Keep in sync the code and documentation (`docs/arm64emu/`).
* Keep in sync the file structure in the architecture section of
  `docs/arm64emu/README.md` and the structure section of this file.
* New implemented syscalls must also be defined in `src/arm64emu/chroot/strace.c` argdefs, so they can be decoded by `--strace-full`.

## Testing

### Boot mode

Basic testing which enough for most cases to ensure no regressions:

* `make test-arm64emu-boot`: asm base suite, predecoded tier disabled
* `make test-arm64emu-boot-pd`: asm suite on the default predecoded tier
* `make test-arm64emu-boot-jit`: asm suite under JIT mode

Extras:

* `make test-arm64emu-boot-pd-full`: the above plus run_bootlog_gate.sh (full 1.6B-insn boot log diff)
* `make test-arm64emu-boot-jit-full`: same as previous but under JIT
* `make fuzz-arm64emu`: 200 random-block seeds across interpreter/pd/jit/jit-slowmem/nofuse/novra
* `make test-arm64emu-boot-jit-a64`: cross-builds a static aarch64 binary (clang+lld) and runs the JIT suite under qemu-aarch64
* `make test-arm64emu-embed`: builds the `-DANDROID_JNI` embedded configuration (`arm64emu_main`, `_exit` on fatal paths) behind a stub main and runs the asm suite against it

The consistency and full-boot log gates boot firmware + Linux and want a kernel
and initrd (`ARM64EMU_KERNEL`, `ARM64EMU_INITRD`; default `$HOME/Image.gz`,
`$HOME/initrd`); without them they report SKIP and only the asm suites run.

### Chroot mode

Basic testing which enough for most cases to ensure no regressions:

* `make test-arm64emu-chroot`: full suite vs the oracle, interpreter mode.
* `make test-arm64emu-chroot-jit`: FP jit-vs-interpreter consistency + entire suite with --jit.
* `make test-arm64emu-chroot32`: suite against the 32-bit ILP32-host build (skips, with the reason named, where the host has no runnable 32-bit toolchain).
* `make test-arm64emu-chroot32-jit`: the same build with `--jit` — the gate for the 32-bit code generators (and, where there is none yet, for the interpreter fallback).

The oracle is `qemu-aarch64`, or on an AArch64 host the CPU itself when qemu
is not installed; `ARM64EMU_ORACLE=qemu|native|recorded` pins it and `ARM64EMU_CC`
overrides the compiler. `make test-arm64emu-chroot-pack` records a full run (binaries + every
oracle answer) into `build/arm64emu-chroot-testpack.tar.gz`; a host with no toolchain
and no possible oracle — a 32-bit ARM device — unpacks it at the same commit
and replays with `ARM64EMU_ORACLE=recorded make test-arm64emu-chroot` (host-state tests skip; the
Alpine-backed sections want any aarch64 rootfs symlinked at
build/cache/arm64emu/chroot/rootfs/alpine). Against hardware, a test needing an optional extension the CPU lacks
skips instead of failing — it declares what it needs in a `REQUIRES:` marker
naming HWCAP strings (see `tests/arm64emu/chroot/hostenv.sh`). Add one to any new test that
uses an instruction outside ARMv8.0-A.

Specialized tests focused at Android OS compatibility:

* `make test-arm64emu-chroot-android-sim`: Android-behavior build (statx ENOSYS fallback + Bionic keyring gate + the emulated-hardlink scheme, which is compiled and forced only here). It also runs the proot-group rows (`tests/arm64emu/chroot/l2s_proot.sh`: a rootfs installed through proot — proot-distro's — read, presented and kept by `--link2symlink`, and the hostile layouts around it; `ARM64EMU_L2S_BUILD=1` tells the harness the scheme is in).
* `make test-arm64emu-chroot-seccomp`: suite with the emulator under an Android-Oreo SECCOMP_RET_TRAP filter (no device).

Clean test environment with `make clean-testenv`.

After intrusion into design of interpreter or JIT it is highly adviced to run real-workloads tests in Alpine Linux environment, examples:

* Use Golang toolchain to build a simple hello-world program.
* Run a Node.js script (e.g. `npm --version`).

No `-E` is needed for these: the guest gets PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin and HOME=/root by default. Pass `-E` only to test a different environment.

### Acceptance criteria

* Test suites pass, both modes': interpreter + JIT, and chroot mode's 32-bit
  host build.
* Real-workload runs produce desired results.
* No crashes.
* No hangups.

**Important**: do not run any of tests above in parallel with each other.

## Diagnostics

Command line options (all but the two `--strace` ones are both modes'):

* `--strace` (chroot mode): log every guest system call in format `PID name(nr,a0..a5) = ret`, also does a full register dump when the guest dies from a fatal signal.
* `--strace-full` (chroot mode): the same trace decoded strace-style — symbolic flags, quoted strings, `execve` argv/envp arrays, read/write buffer contents and output strings (`getcwd`, `readlinkat`), errno-named returns, and `{field=...}` common-struct pretty-printing (decoder in `src/arm64emu/chroot/strace.c`). Plain `--strace` keeps its compact qemu-diffable layout.
* `--debug`: per-instruction trace in format `<pc>: <insn-word>  [elN nzcv=....]`, forces the interpreter.
* `--reg-trace`: compact per-instruction register trace, `P<pc> <x0> ... <x30> S<sp>` a line, forces the interpreter.
* `--max-insn N`: stop after N retired instructions and dump the CPU state (and the ring, with `ARM64EMU_RING`), at N exactly in every engine — bisect it to find the first instruction two engines disagree on. In chroot mode each guest thread counts its own from the start of its program (execve restarts it, a new thread starts at 0, a fork child carries on its parent's count), the first to reach N ends its process as SIGKILL would, and the JIT runs unchained meanwhile (generated code checks no deadline in user mode).
* `--no-predecode`: disable decoded-instruction cache, use it to decide whether a bug lives in the predecode fast path or the real decoder.

The step driver's diagnostics (`lib/arm64/cpu.c`, both modes; all force the interpreter):

* `ARM64EMU_PROF=1`: hot-PC profiler, the 25 hottest PCs. Boot mode prints it at exit and on SIGINT/SIGTERM; chroot mode once per program, headed `[prof] pid <pid> <path>`, at the execve that replaces it and at its process's end (a fatal signal included), counted atomically across the program's threads; a fork child starts empty.
* `ARM64EMU_RING=1`: each CPU's last 96 instructions (pc, word, x0..x2), printed when the run is cut short: --max-insn, a coverage divergence, SIGINT/SIGTERM or a halt with no wake source in boot mode, a fatal signal in chroot mode (the dying thread's).
* `ARM64EMU_TPC=0xPC`: dump the registers each time the pc hits this address.
* `ARM64EMU_COV=FILE`: coverage-divergence finder — the first PC, past the first 1000 instructions, missing from a QEMU PC set (one hex PC per line) prints the ring and stops the run (chroot mode: the process, as SIGKILL would).

Every line these print is one write, so concurrent guest threads never tear one another's.

JIT debugging environment variables:

* `ARM64EMU_JIT_STATS`: at exit, ranks the instruction words still falling back to the exec_a64 interpreter helper (i.e. what the JIT couldn't translate). ARM64EMU_JIT_STATS=/path dumps the ranking to a file instead of stderr.
* `ARM64EMU_JIT_DUMP=PREFIX`: writes each translated block to a sparse code-cache image `PREFIX.<pid>.<tid>.code` plus a .map, so you can disassemble the native code the JIT produced.
* `ARM64EMU_JIT_PDMAX=N`: forces predecode ops with id > N through the interpreter helper. This is the bisection knob: sweep N to pin a codegen bug down to one instruction class.
* `ARM64EMU_PD_MAX=N`: the predecoded tier's analogue: only predecode ops with id <= N keep their native handlers, everything above runs through exec_a64 (0 = the plain interpreter).
* `ARM64EMU_JIT_SLOWMEM`: forces every inline memory access down its slow helper branch, isolating fast-path memory codegen bugs.
* `ARM64EMU_JIT_NOFUSE`: disables instruction / D-TLB-probe fusion (all backends).
* `ARM64EMU_JIT_NOFP16`: disables FP16 native codegen on the AArch64 backend (falls back to helper).
* `ARM64EMU_JIT_NOVRA`: disables the V-register cache/allocator.
* `ARM64EMU_JIT_SSE=2`: forces SSE2-baseline capability answers on x86-64 hosts (test lower-ISA codegen paths).

Behavior fallbacks:

* `ARM64EMU_PATHFAST_OFF`: resolves every guest path by walking it component by component (a readlink each) instead of folding it lexically and letting the pin certify that no component was a symlink. Same answers, more syscalls — the suite runs the path-race test over both routes. (The fold declines any path whose `..` cancels a component: the pin certifies only what the fold left, and `link/..` climbs from the link's target — `tests/arm64emu/chroot/c/pathdotdot.c`.)
* `ARM64EMU_PATHFAST_VERIFY`: resolves every guest path BOTH ways and aborts on any disagreement. Development knob, and only for a quiescent tree: the two walks run at different instants, so a guest racing its own mounts/renames (tests/arm64emu/chroot/fixtures/bindrace.c) makes them differ for a reason that is not a bug.
* `ARM64EMU_PINWALK_FORCE_LOOP`: pins a resolved path's parent one component at a time (openat/O_NOFOLLOW) instead of in a single `openat2(RESOLVE_NO_SYMLINKS)` — the tier a host kernel older than 5.6 is served by, and the one a seccomp filter over openat2 puts a modern host on. Same containment, one syscall per component instead of one per path — and two descriptors open at once instead of one, which is why the guest's `RLIMIT_NOFILE` is judged from the walk's first allocation and not its last (`tests/arm64emu/chroot/c/fdlimit.c`, whose `(loop-tier)` row runs over this).
* `ARM64EMU_PROCSTAT_FORCE_SYNTH`: forces the synthetic /proc/stat fallback.
* `ARM64EMU_PROCFS_FORCE_OLD`: hides from the host's own /proc what a kernel older than the advertised one does not have — `/proc/<pid>/stat` past field 44 (start_data..exit_code are 3.3) and `status`'s RssAnon/RssFile/RssShmem lines (4.5) — so the synthesized views must supply them rather than rewrite them; the tier an Android 7 device (3.1) is permanently on, where a guest could otherwise not find its own argv, its own brk, or the parts of its own resident set. The suite's `vmreport (old-procfs tier)` rows run over it.
* `ARM64EMU_PROCSTAT_HOTPLUG_SIM`: walks the online-CPU count down on every
  /proc/stat sample, simulating a host that hotplugs cores (Android does, for
  power) — the condition under which the synthesized counters must stay
  monotonic, and one no ordinary CI machine ever produces.
* `ARM64EMU_OVERFLOWID_FORCE_SYNTH`: forces the synthetic /proc/sys/kernel/overflow{u,g}id fallback.
* `ARM64EMU_PROCSYNTH_FORCE_FAIL`: makes the synthesized /proc views find no anonymous backing (no memfd, no writable dir) — the per-process files are then denied, never served from the host's own /proc.
* `ARM64EMU_PROCSYNTH_FORCE_STAT_DENY`: makes the host refuse stat, statx, access and statfs of every synthesized /proc file with EACCES — the tier Android's SELinux policy serves, which denies an app getattr on the very files (version, stat, loadavg, ...) whose open the emulator answers itself; the guest is then answered from the synthesized view (root-owned 0444 regular files of size 0, a process's own files owned by the emulator's user). Pair it with `ARM64EMU_PROCSTAT_FORCE_SYNTH` / `ARM64EMU_OVERFLOWID_FORCE_SYNTH` to reach the try-host-first names; the suite's `procsynth_stat (stat-denied tier)` rows run over it.
* `ARM64EMU_NETLINK_FORCE_BLOCK`: forces the netlink fallback path.
* `ARM64EMU_PROCTAB_FORCE_FILE`: skips the diskless guest-PID registry broker (memfd over an abstract socket) for the named-file tier — what a host with neither memfd_create nor abstract sockets is permanently on. The file's name is fixed and its directory is world-writable, so it is opened `O_NOFOLLOW` and refused unless it is a regular file of this user with no other link and no group/other access; a refusal degrades to the per-invocation anonymous table and says so on stderr.
* `ARM64EMU_SHM_FORCE_FILE`: forces System V shm segments onto file backing (an unlinked `mkstemp` file in the first writable dir) instead of an anonymous memfd, exercising the fallback tier.
* `ARM64EMU_GETRANDOM_FORCE_DEV`: forces guest getrandom(2), and the guest's AT_RANDOM seed, onto the /dev/urandom / /dev/random fallback — the tier a host kernel without getrandom(2) (< 3.17, e.g. Android 7's 3.x) is served by.
* `ARM64EMU_GETRANDOM_FORCE_OLD`: refuses `GRND_INSECURE` on the host getrandom(2) path the way a 3.17–5.5 kernel does (every Android 10/11 kernel) — the tier such a host is served by, where the flag the guest's 6.1 ABI promises is answered from /dev/urandom instead of with the host's EINVAL; the suite's `c/getrandom(old-getrandom tier)` row runs over it.
* `ARM64EMU_PAGEPROBE_FORCE_PIPE`: probes the pages of a grown file mapping with a pipe write instead of process_vm_readv — the tier a host kernel older than 3.2 (e.g. Android 7's 3.1) is served by, since the syscall does not exist there.
* `ARM64EMU_MINCORE_FORCE_FAIL`: makes the resident-set sample fail — the tier a host that will not answer mincore(2) (a sandbox, a seccomp policy) is served by, where what a guest reads about its own memory keeps the guest's sizes and falls back to the host's Rss figures, bounded by what the guest mapped. That is the same tier every reader of *another* process is permanently on; the suite's `vmreport (no-mincore tier)` row runs over it.
* `ARM64EMU_MEMFD_FORCE_FILE`: forces guest memfd_create(2) onto the unlinked-file fallback tier with broker-held seals (what a host kernel without memfd_create is served by); the suite's `(memfd-tier)` rows run over it.
* `ARM64EMU_MEMFD_SEAL_FORCE_OLD`: refuses a read-only shared mapping of a write-sealed memfd the way a kernel older than 6.x does -- the tier every current LTS kernel puts a host on, where the deny-writable reference the seal takes is counted against every shared mapping before the kernel asks whether that mapping could write at all. The emulator serves the guest the 6.x mapping from backing of its own (the file cannot change under that seal, so nothing about it is guest-visible), and the suite's `(old-seal-mmap tier)` row runs over it.
* `ARM64EMU_SIGRT_MAX=N`: reserves the emulator's own three host signals (the control-channel kick and the guest-32/33 carriers) below N instead of at the top of the RT range — the tier a host that accepts but cannot deliver its top RT signals is served by, `qemu-user` being one; the suite's `(low-rt-tier)` rows run over it.
* `ARM64EMU_OWNFD_FORCE_DENY`: refuses the path spelling of the emulator's own fds (`/proc/self/fd/N`) — the tier Android's SELinux policy serves, where a memfd cannot be re-opened through that link, so exec and open must reach the image through the descriptor instead; harsher than the device (stat and access are refused too), and the suite's `(path-denied tier)` rows run over it.
* `ARM64EMU_MEMFD_CHMOD_FORCE_DENY`: refuses every mode change on a memfd — the tier Android's SELinux policy serves, where an app has no setattr on one, so a guest that takes the execute bit off a memfd it owns would otherwise be told EACCES by a call Linux allows and the exec check would go on judging the 0777 the kernel handed out; the guest-set mode is held in the broker registry beside the seals instead, and the suite's `(chmod-denied tier)` rows run over it.
* `ARM64EMU_SIGQ_MAX=N`: caps a thread's pending-signal queue at N entries — pinning it at its floor makes every signal flood go through the back-pressure gate (the queue blocks the signals in the mask its capture handler returns to, leaving the rest of the flood in the host kernel's own queue until it drains); the suite's `(gate-tier)` row runs over it.
* `ARM64EMU_TERMIOS2_FORCE_DENY`: refuses the termios2 ioctls (`TCGETS2`/`TCSETS2`/`TCSETSW2`/`TCSETSF2`) with EACCES — the tier Android's SELinux policy serves, where an app may not issue them on its pty and a glibc 2.42+ guest (whose `tcgetattr`, hence `isatty`, uses them) would take its terminal for none, so an interactive bash printed no prompt; the emulator then answers them from `TCGETS`/`TCSETS*`, keeping an arbitrary (`BOTHER`) baud rate per terminal itself. The suite's `termios2 (denied tier)` row runs over it.
* `ARM64EMU_TIOCGSID_FORCE_DENY`: refuses `TIOCGSID` (`tcgetsid(3)`: login, script, agetty) with EACCES — the tier Android's SELinux policy serves on a pty slave and on a pipe (not on the ptmx master), where a kernel answers the session or ENOTTY; the emulator then works the answer out from `TCGETS` (a terminal at all?), `TIOCGPGRP` (the kernel's own "is it the caller's controlling terminal" check) and `getsid`. The suite's `tiocgsid (denied tier)` row runs over it.
* `ARM64EMU_TTYIOCTL_FORCE_DENY`: refuses terminal ioctls with EACCES the way Android's SELinux policy does — every terminal command (`TCGETS`, `TIOCGPGRP`, `TIOCGWINSZ`, the termios2 commands, `TIOCGSID`, ...) and `FIOCLEX`/`FIONCLEX` on a pipe, a socket, a regular file or a directory (a socket's `SIOCOUTQ` aside), and on a pty slave every one but the policy's short list (`TCGETS`, `TCSETS*`, `TCFLSH`, `TIOCSCTTY`, `TIOC[GS]PGRP`, `TIOC[GS]WINSZ`, `TIOCOUTQ`). On the first four the kernel answers ENOTTY (a guest's `tcgetpgrp(3)` of a pipe read "Permission denied"), and `FIOCLEX`/`FIONCLEX` are served with `F_SETFD`; on a pty slave the refused commands are served from the allowed ones — `tcdrain`/`tcsendbreak`/`tcflow` through the background-job check `TCFLSH` makes, `TIOC[GS]SOFTCAR` through `TCGETS`/`TCSETS` with SIGTTOU blocked, the modem/packet/console commands ENOTTY — except `TCXONC`'s `TCOOFF` and `TIOCNOTTY` on the caller's own terminal, which nothing allowed can do. The suite's `ttynotty`, `ptsioctl` and `ioctlorder` `(denied tier)` rows run over it.
* `ARM64EMU_TLBPUB_MAX=N`: caps the D-TLB published-epoch table at N slots — the tier a guest with more live threads than the table holds is served by: a thread the quarantine cannot account for stops it draining at all, so retired host backing is never released out from under a stale translation.
* `ARM64EMU_SIGCHLD_DELAY=MS`: holds a guest thread MS milliseconds as it starts on each `SIGCHLD` notice the host kernel sends — what a loaded host does to a thread it preempts there, which no idle CI machine does on cue. The emulator's SIGCHLD watcher is never held, and its look for a gone tracer (every 100 ms) runs meanwhile: a SIGKILLed tracer's held children are released and the watcher takes that release while the parent's thread is still at the tracer's own notice, which must still reach the guest after the child's, the kernel's order (`cw_overtake`). The suite's `heldzombie (slow-notice tier)` row runs over it.
* `ARM64EMU_SICODE_FORCE_KNOWN`: sends the emulator's own signals between processes with si_codes of a layout the kernel knows only — the tier a host that cannot carry a private si_code is served by (qemu-user: E2BIG for an unknown layout, si_code cut to 16 bits, si_errno zeroed): a queued siginfo the host would not carry whole (a thread-directed one, one with an si_errno or a code of its own, a job-control signal's with a payload) is posted to the receiver's inbox in its registry entry and sent as an SI_QUEUE naming the slot, and the other job-control signals to a traced process ride the kick as SI_QUEUE with a descriptor in si_value. The suite's `(known-layout-tier)` ptrace, sqiread and carrystale rows run over it.
* `ARM64EMU_NOCLDWAIT_FORCE_EMULATE`: reaps an SA_NOCLDWAIT parent's children and drops an SA_NOCLDSTOP parent's stop and continue notices in the emulator's capture instead of leaving them to the host — the tier a host that ignores the two flags (qemu-user) is served by; the suite's `sigchldflags (nocld-tier)` row runs over it.
* `ARM64EMU_STACKGROW_FORCE_PIECE`: reserves no host room below a stack and never extends its backing downward, so a stack that grows goes on in a new piece of backing of its own below it — the tier a host whose address space under a stack is taken is served by. Nothing mapped moves, so another thread's pointer into the stack, a host-atomic access and a transfer lent to the host all survive the growth; the suite's `growsdown (piece-tier)` and `growsnomove (piece-tier)` rows run over it.

Tuning:

* `ARM64EMU_JIT_MB`: JIT code-cache size in MiB per guest CPU — per guest thread in chroot mode (default 32, clamped 1–128; 1–32 on ARM32 hosts).
* `XDG_RUNTIME_DIR`, `TMPDIR`: first writable one holds the backing directories of emulated tmpfs mounts (removed when the session ends; a session killed before it could clean up is swept by the next one), plus the --shared-proc *fallback* registry file and the System V shm segment backings when the diskless broker (memfd) is unavailable and /dev/shm isn't writable (Termux). The shm backings are unlinked the moment they are created (the broker hands attachers the descriptor, never a name), so only the registry file has one — and it is refused unless it is a regular file owned by this user that nobody else can reach.
