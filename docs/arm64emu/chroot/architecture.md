# Architecture

## The core / new-code split

arm64emu's chroot mode is built around a decode/execute core it shares with
its boot mode, the full system. It lives once, in `lib/arm64/` (`types.h`,
`cpu.h`, `cpu.c`, `decode.c`, `sysreg.c`, `exec_fpsimd.c`, `esr.h`, and the
predecoded tier, `pdrun.c`), and every CPU runs it in user mode (`as_cpu_attach` sets the CPU's
mode, `c->eng`: `sys` 0 has `sysreg_init` set EL1 up as Linux does, so the
EL1-only instructions and checks are what they are at EL0 under Linux --
UNDEFINED, or never enabled; `smp` 1 makes LSE atomics, exclusives,
LDAR/STLR and barriers host atomics and fences, since guest threads run
concurrently). So is the JIT (`lib/arm64/jit/`), whose coherence with the
guest address space is this program's (`src/arm64emu/chroot/jit/coherence.c`).
Everything below the memory seam and above the exception seam is rewritten for
user mode.

```
    shared (lib/arm64/)                                    new (src/arm64emu/chroot/)
 ┌───────────────────────────────────────────────┐   ┌──────────────────────────────┐
 │ types.h esr.h cpu.h sysreg.h                   │   │ loop.c   run loop + dispatch │
 │ decode.c      A64 integer/branch/load-store,   │   │ syscall.c + sys_*.c  (~190)  │
 │               LSE atomics, exclusives          │   │ path.c   rootfs containment  │
 │ exec_fpsimd.c FP / Advanced-SIMD / crypto      │   │ signal.c guest sigframes     │
 │ cpu.c pdrun.c the step driver, the pd tier     │   │ elf.c    loader + stack/auxv │
 │ sysreg.c      MRS/MSR, FPCR/FPSR, DC ZVA, CNT* │   │ main.c   CLI                 │
 └───────┬───────────────────────────────┬───────┘   └──────────────────────────────┘
   mem_read/write/128, mem_ifetch         │ exception_take()
         │  (the MEMORY seam)             │  (the EXCEPTION seam)
 ┌───────▼─────────────────┐   ┌──────────▼───────────────┐
 │ mmu.h + mem.c           │   │ exception.c              │
 │ 2-level software page   │   │ records a pending        │
 │ table, guest mmap/brk,  │   │ exception into g_tls;    │
 │ copy_{to,from}_guest,   │   │ never vectors to EL1     │
 │ mem_host_ptr            │   │                          │
 └─────────────────────────┘   └──────────────────────────┘
```

The core never references `struct Machine`, devices, or physical memory. It only
knows `CPU` (its register file) and the two seams.

## The two seams

### 1. Memory seam (`mmu.h`)

The core performs every guest memory access through four functions plus one
inline, all taking a guest virtual address and returning `bool` (`false` = a
fault was raised and recorded):

```c
bool mem_read (CPU*, u64 va, unsigned size, u64 *out);
bool mem_write(CPU*, u64 va, unsigned size, u64 val);
bool mem_read128 (CPU*, u64 va, V128 *out);
bool mem_write128(CPU*, u64 va, const V128 *val);
static inline bool mem_ifetch(CPU*, u64 va, u32 *insn_out);   // fast path
```

In the system emulator these walk hardware page tables against a physical bus;
here they index a software page table straight to host memory. The core is
unchanged. See [memory.md](memory.md).

### 2. Exception seam (`cpu.h` / `exception.c`)

The core signals every synchronous event — `SVC`, data/instruction aborts,
undefined instructions, `BRK` — by calling:

```c
void exception_take(CPU*, ExcKind, u64 esr, u64 far, u64 ret_addr);
```

which is the program's, the entry of that name in the ops the program gives
each CPU (`cpu.h`, `CpuOps`). In the system emulator this banks
SPSR/ELR/ESR/FAR and vectors to the EL1 handler.
In user mode there is no guest kernel, so the rewritten `exception.c` **records**
the pending exception into thread-local state, asks for the CPU's attention and
returns; the run loop dispatches it. The CPU stays at EL0 for the process's entire life, and the core holds it to
EL0's rules: a system instruction EL1 has not opened to it (`el0_allowed` in
`sysreg.c`, judged on `SCTLR_EL1`/`CNTKCTL_EL1`, which `sysreg_init` sets as
Linux does: `DZE`, `UCT`, `UCI` and the virtual counter, never `UMA`), `ERET`,
`DRPS`, `HLT`, `HVC` and `SMC` are all UNDEFINED there, and reach the program as
the `SIGILL` a kernel sends (`tests/arm64emu/chroot/c/el0sysinsn.c`). The ID registers are among
them, and there `loop.c` plays the kernel's part: it answers an `MRS` of that
space from the kernel's sanitized view of the CPU (`emulate_id_mrs`, the tables
of `cpufeature.c`: the feature fields a kernel shows, every other field its safe
value, AArch64 only at EL0 and EL1) and advertises that it does with
`HWCAP_CPUID` (`tests/arm64emu/chroot/c/idregs.c`, `tests/arm64emu/chroot/fixtures/idregs.c`).

```c
void exception_record(CPU *c, ExcKind kind, u64 esr, u64 far, u64 ret_addr) {
    g_tls.pend_exc.valid = true;
    g_tls.pend_exc.esr = esr;
    g_tls.pend_exc.far = far;
    c->pc = ret_addr;      // SVC: next insn; faults: the faulting insn
    c->excl_valid = false;
    cpu_attn(c);           // the engine returns to the run loop
}
```

`cpu_raise_sync` (undefined/BRK/alignment, `lib/arm64/cpu.c`) is a thin wrapper
that passes `cur_insn_pc` as the return address.

### The `struct Machine` trick

`cpu.h` forward-declares `struct Machine` and `CPU` carries a `struct Machine *m`.
The system emulator defines it as the board (RAM, devices). Here `machine.h`
redefines it as the **linux-user task state** — address space, fd bookkeeping,
cwd/rootfs, signal dispositions, fake-id credentials. Because the core only holds
an opaque pointer, the shared files never change even though `Machine` means
something completely different.

## Control flow

### One instruction (threaded fast path, `pd_run` in `lib/arm64/pdrun.c`)

1. `cur_insn_pc = pc`.
2. `mem_ifetch(pc, &insn)` — fast path reads through a per-thread single-page
   host-pointer cache; a miss or fault takes `mem_ifetch_slow`.
3. Decode-cache lookup: a per-CPU (so per-thread), per-PC cache of the *decoded* form
   (dense opcode id + pre-extracted operands, 16 B/entry). The entry is a pure
   function of the instruction word, so comparing it against the live fetched
   word fully validates a hit — self-modifying/remapped code needs no flushes.
   A mismatch runs the classifier (`pd_fill`) once.
4. `pc += 4`; dispatch the opcode id by computed goto. ~200 hot forms execute
   inline (transcribed from decode.c); everything else is `PD_GENERIC` →
   `exec_a64(insn)` — 4-bit top-level switch → group decoder → execute.
5. `icount++`; each handler then fetches and dispatches the *next* instruction
   itself (direct threading), so instructions run back-to-back until the CPU
   asks for attention — a recorded exception, a pending signal or call-out
   (`sig_npend_raise`), stop/halt — or a fetch faults, and control returns to
   `emu_loop`, which drops the request before it looks at the reasons (lib/arm64
   `cpu.h`, "Run control").

When any per-instruction debug facility is active (`g_debug_hooks`) the loop
instead calls the full `cpu_step` (`lib/arm64/cpu.c`), which adds the debug
hooks. `--no-predecode` selects a plain fetch → `exec_a64` step (diagnostic mode
for bisecting decode-cache suspicions).

**The classifier owes the decoder its guards.** `pd_fill` is a transcription of
`decode.c`'s classification, and its safety property is that anything not
recognized *with certainty* stays `PD_GENERIC` and re-dispatches into
`exec_a64`. That only holds if a transcribed form carries the decoder's
validation as well as its operand extraction: a classifier that extracts the
operands but drops an encoding check produces a live cache entry for a word the
decoder would have rejected, and then `--no-predecode` — the knob whose whole
job is to answer "is this bug in the fast path or the real decoder?" — reports a
different architectural result than the default engine. `pd_fill` is also the
JIT frontend's decoder, so such a gap lands in all three engines at once. When
tightening an encoding in `decode.c`, check whether `pd_fill` classifies that
form and tighten it in the same commit; `tests/arm64emu/chroot/c/undef_enc.c` runs under the
default (predecode) engine, and `make test-arm64emu-chroot-jit` covers the frontend.

The invariant is also enforced directly, by the `chaos` mode of
`tests/arm64emu/chroot/fixtures/insnfuzz.c`: it runs random instruction words through the
decode cache, `--no-predecode` and `--jit` and requires all three to produce
the same result. qemu cannot referee that comparison — it rejects encodings we
still execute and implements extensions we do not — but the three engines
refereeing each other needs no oracle at all, and a guard that reaches
`decode.c` alone shows up there immediately. Its sibling `conform` mode covers
the other half: a table of real allocated encodings, replayed against varying
input state, where qemu *is* the oracle and the match must be exact.

A third mode, `seq`, runs 2..24 allocated instructions as a single basic
block under the same three-engine rule. Single-instruction stubs can only
test decoding and arithmetic; `seq` is what reaches the machinery a
translator runs *between* instructions — register allocation and spilling,
the lazy-flag window between a flag producer and its consumer, fused
memory runs behind one D-TLB probe, and the block-local vector-register
cache. Each block is written to a fresh page, because rewriting one page is
an `IC IVAU` per iteration and the JIT abandons a page after 32
invalidations and interprets it — which would leave the comparison passing
while comparing the interpreter with itself.

The optional `--jit` inserts one more rung above `pd_run`: `jit_run`
(`src/arm64emu/chroot/jit/`) executes native translations of guest basic blocks and honors the
same return contract, handing control back to `emu_loop` on the same rare
events. It is off by default and only compiled-in for AArch64/x86-64/i686/ARM32 hosts; the
debug ladder still wins, so `-d` forces the interpreter. See [jit.md](jit.md).

### One syscall (the run loop, `src/arm64emu/chroot/loop.c`)

```
emu_loop:
  if c->stop: return
  run instructions (pd_run burst; cpu_step under debug; plain step under --no-predecode)
  drop the CPU's attention request
  if g_tls.pend_exc.valid:
      switch on ESR.EC:
        EC_SVC64            -> syscall_dispatch(c)          // x8=nr, x0..x5 args, x0=ret
        EC_DABORT/EC_IABORT -> sig_deliver_fault(SIGSEGV)   // MAPERR vs ACCERR from mem.c;
                                                            // SIGBUS: BUS_ADRERR past EOF,
                                                            // BUS_ADRALN misaligned exclusive,
                                                            // ordered or atomic access
        EC_PC_ALIGN         -> sig_deliver_fault(SIGBUS)    // BUS_ADRALN, si_addr = PC
        EC_SP_ALIGN         -> sig_deliver_fault(SIGBUS)    // BUS_ADRALN, si_addr = SP
        EC_BRK64            -> sig_deliver_fault(SIGTRAP)
        EC_UNKNOWN/default   -> sig_deliver_fault(SIGILL)
      (each but EC_SVC64 and EC_BRK64 first sets the thread's fault record:
       fault_note_abort / fault_note, which every later frame reads)
  if g_sig_npend:  sig_deliver_pending(c)   // host-caught guest signals, at a safe point
```

`syscall_dispatch` (`src/arm64emu/chroot/syscall.c`) indexes a table of ~190 handlers grouped
into `sys_*.c` by area; unknown numbers return `-ENOSYS` with a one-shot warning
(except a "designed-ENOSYS" set that libcs probe and fall back from). See
[syscalls.md](syscalls.md).

## Module map

| File | Responsibility |
|------|----------------|
| `lib/arm64/decode.c` | A64 integer/branch/load-store decode+exec; LSE atomics; exclusives as host CAS (`c->eng.smp`). |
| `lib/arm64/exec_fpsimd.c` | Scalar FP, Advanced-SIMD, crypto; FPCR rounding; 32-bit-host-safe (no `__int128` in the hot paths). |
| `lib/arm64/cpu.c` | `cpu_step` fetch/decode/execute driver; SPSR pack/unpack; condition helpers; debug trace. |
| `lib/arm64/cpu.h` | CPU state (one struct for both emulators), register helpers, core API, run control (the engines' contract with the run loop). |
| `lib/arm64/sysreg.c` | `MRS`/`MSR`, ID registers, `FPCR`/`FPSR`, `DC ZVA`, generic-timer reads; which of them EL0 may execute. |
| `src/arm64emu/chroot/mmu.h`, `src/arm64emu/chroot/mem.c` | Guest address space + the `mem_*` seam. |
| `src/arm64emu/chroot/exception.c` | Pending-exception recorder (the exception seam). |
| `src/arm64emu/chroot/loop.c` | Run loop + exception dispatch + signal delivery point; the kernel's EL0 `MRS` emulation of the ID registers. |
| `lib/arm64/predecode.h`, `lib/arm64/predecode.c` | The classifier: an instruction word -> `PDEnt` (dense op id + operands), `PD_GENERIC` for anything uncertain; also the JIT frontend's decoder. |
| `lib/arm64/pdrun.h`, `lib/arm64/pdrun.c` | Decoded-instruction cache + direct-threaded dispatch of ~200 hot forms; `PD_GENERIC` falls back to `exec_a64`. |
| `src/arm64emu/chroot/elf.c` | ELF64 loader, `PT_INTERP`, initial stack/auxv/HWCAP, sigreturn trampoline page. |
| `src/arm64emu/chroot/path.c` | Rootfs containment resolver; `/proc` and `/dev` special-casing. |
| `src/arm64emu/chroot/syscall.c` + `src/arm64emu/chroot/sys_*.c` | Syscall dispatcher and per-area handlers (~190). |
| `src/arm64emu/chroot/sys_procfs.c` | Synthesized guest `/proc` views (`maps`, `cmdline`, `mounts`/`mountinfo`, `stat`, `loadavg`/`uptime`/`version`, `locks` minus the holders the guest cannot see, and the lines of per-process `status` that would otherwise describe the emulator). |
| `src/arm64emu/chroot/sys_netlink.c` | In-process `NETLINK_ROUTE` emulation for hosts that deny `AF_NETLINK` (Android/SELinux), reached from the socket calls in `sys_net.c` and from `read`/`write`/`readv`/`writev` in `sys_file.c`; also answers the read-only `SIOCGIF*` interface-query ioctls (ifconfig/net-tools) from the host interface table. |
| `src/arm64emu/chroot/proctab.c` | Shared-memory guest-PID registry powering the cross-process `ps`/`top` and hidden-process views, and holding the id maps of a faked user namespace (which a parent writes for its child) and each process's seccomp state. |
| `lib/arm64/jit/` | Optional `--jit` translator: `frontend.c` (decode → IR), `backend_a64.c` / `backend_x86_64.c` / `backend_x86_32.c` / `backend_arm32.c` (emitters), `jit.c` (code cache, chaining, invalidation). |
| `src/arm64emu/chroot/jit/coherence.c` | The JIT's coherence with the guest address space: thread registry, sticky code-page map, invalidation by VA range. |
| `src/arm64emu/chroot/signal.c` | Host signal capture → guest `rt_sigframe`; `rt_sigreturn`; job-control mask mirroring. |
| `src/arm64emu/chroot/thread.h` | Per-thread state (`g_tls`): pending exception, tid, syscall-restart bookkeeping. |
| `src/arm64emu/chroot/machine.h` | `struct Machine` = the shared per-process task state. |
| `src/arm64emu/chroot/guest_abi.h` | arm64 syscall numbers, auxv tags, HWCAP bits, explicit guest struct layouts. |
| `src/arm64emu/chroot/main.c` | CLI parsing, rootfs setup, initial exec. |

## What is per-thread vs per-process

- **Per-thread** (`g_tls`, thread-local; `src/arm64emu/chroot/thread.h`): the `CPU` register
  file, the pending exception, the translation caches (D-TLB and instruction-fetch
  cache, in the thread's storage and reached through its CPU, `c->eng`), the tid,
  and syscall-restart state. Each guest thread runs its own `emu_loop` on its own
  host thread.
- **Per-process** (`struct Machine`, shared across threads, copied on `fork`): the
  address space, fd metadata, cwd/rootfs, signal dispositions/mask, and the
  fake-id credential set — matching POSIX, where these are process-wide.

## Host / build matrix

One portable code path, no per-arch `#if` in the logic. Exercised across these
targets — the committed CI automates the x86-64 and arm64 `make test-arm64emu-chroot`; the
maintainer runs the rest before a release:

| Host | How it runs | Status |
|------|-------------|--------|
| x86-64 | native | primary; full suite (CI) |
| arm64 | native | full suite + the only run of the JIT's AArch64 backend (CI) |
| i386 | native (`make arm64emu32`) | ILP32 coverage |
| armhf (32-bit ARM) | `arm-linux-gnueabihf-gcc -static`, run under `qemu-arm` | full suite |

The `i386` build runs natively on the x86-64 dev host, so 32-bit-host correctness
(ILP32 struct conversion, `uintptr_t` page-table leaves, wide atomics) is
exercised by the native `make arm64emu32` build, not only on real 32-bit hardware.
