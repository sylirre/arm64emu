# chroot-ng — design

A `proot`-like tool that emulates a chroot environment and bind mounts for
**rootless, SELinux-restricted Android without user namespaces**, but without
paying `proot`'s per-syscall `ptrace` overhead.

## Target environment (locked)

- **Arch:** AArch64 only.
- **Kernel floor:** Linux 3.5+ is the real floor (seccomp-BPF needs 3.5). 3.4
  is best-effort at most (would require a pure-ptrace tier we are not building
  first).
- **Rootless**, SELinux-confined app domain, **no `CONFIG_USER_NS`**.
- **Binaries live on a true `MNT_NOEXEC` mount** (SD/USB/FUSE), so neither
  `execve` nor file-backed `PROT_EXEC` mmap works on them. The only in-process
  way to run such code is anonymous executable memory, which depends on the
  SELinux `execmem` permission (the W^X-compliant `mmap(RW)` → `mprotect(RX)`
  flow used by ART's JIT). **This is the pivotal prerequisite** — `probe`
  checks it.
- **Fidelity beyond chroot+bind:** fake user identity (`-u`/`--fake-id`), `/proc`
  emulation (`--no-proc` to disable), and `link2symlink` (`-l`/`--link2symlink`).

## Threat model — containment, not a sandbox

chroot-ng runs the guest **in its own address space**: the monitor's code, its
`svc` gate and the path-translation state are ordinary pages of the same
process. A guest that sets out to defeat translation needs no syscall to do it
— a store into the monitor's `.data`, or executable memory placed over the gate
range the seccomp filter allowlists, is enough, and neither is something a
syscall filter can see. Interception here is therefore **containment for guests
that are not attacking it**, which is the standing `proot` has and for the same
reason.

What that does and does not buy:

- A guest's *mistakes* are contained exactly: a path escaping the rootfs, a
  `..` run, an absolute symlink, a dirfd opened outside the view, an untranslated
  name reaching the host. Those are the bugs the path layer exists for.
- A guest's *malice* is not. Hostile code sharing an address space with its own
  monitor is not confined by it; put a kernel boundary — a container, a VM, a
  separate uid — around the whole invocation instead.
- The same holds one level down, for a guest racing itself. What the kernel
  installs into the shared descriptor table it installs before anything of the
  monitor's runs: a descriptor received over a socket, imported with
  `pidfd_getfd` or opened by a scoped `openat2` that landed on a hidden
  process exists in the table from the kernel's install until `cng_fd_admit`
  closes it, and a thread that uses its number in that interval — a `dup()`,
  a name resolved against it — keeps what the close was for. There is no
  receiving into a table the guest does not share, so the interval is not the
  monitor's to remove; what is the monitor's is the judgement, and that is
  made from its own copy of the kernel's record (the control data of a
  `recvmsg` is received into a buffer of the monitor's), so nothing a thread
  writes into the guest's own buffers changes which descriptor is closed.

One rule follows for the implementation: chroot-ng must never be the instrument.
Wherever a **guest-chosen address** reaches a `MAP_FIXED` of ours — an `ET_EXEC`
guest's link-time vaddr (`src/chroot-ng/loader/elf.c`), `shmat(SHM_REMAP)`
(`src/chroot-ng/monitor/shm.c`) and the mmap hook's anonymous stand-in
(`src/chroot-ng/monitor/execmap.c`) — the range is checked against everything that is the
monitor's and refused (`ENOEXEC` / `EINVAL` / the kernel's own answer):
`cng_hits_monitor()` covers the image (`__ehdr_start` to `_end`, wherever the
loader put it),
every region the own-map registry records (the floor, the registries, the argv
snapshot an exec stands on) and the scratch stacks with the signal frame a
dispatch returns through — the same three answers the exec sweep uses to tell
the monitor's memory from the outgoing program's. A monitor mapped over by its
own hand has nothing left to report the failure with. That is what the image's
placement (a PIE now, once a 64 GiB link base: see the 0x400000 entry in
`docs/chroot-ng/STATUS.md`) keeps out of the way of, and `p_vaddr` is a field in a
file, so the placement alone is not a guarantee — and the image is not the only
mapping a file can name.

## Why not the obvious approaches

- **LD_PRELOAD / linker interception** (termux-exec): fails for static musl/glibc,
  Go, Rust, and any program issuing raw `svc` — exactly our constraint.
- **User namespaces** (bubblewrap-style): unavailable on Android.
- **Plain ptrace** (classic proot): works but is the overhead we are removing —
  two stops + a context switch to a separate tracer per syscall.

## Architecture: a layered, ptrace-free engine

Two hard shared components sit under a tiered interception mechanism.

### Shared component 1 — userland ELF loader (`ul_exec`)

Read the target ELF as *data* (works on `noexec`), map its `PT_LOAD` segments,
load its `PT_INTERP` (`ld.so`) the same way for dynamic binaries, build the
initial stack (`argv`/`envp`/`auxv`, `AT_PHDR`/`AT_ENTRY`/`AT_BASE`/`AT_RANDOM`),
set up TLS, and jump to the entry point — **no kernel `execve`**. Two mapping
strategies:

- **anonymous** — `mmap(RW)` → copy → `mprotect(RX)`. Defeats a true `noexec`
  mount, but needs `execmem`. **Gotcha found on device:** `PR_SET_NO_NEW_PRIVS`
  (mandatory for the unprivileged seccomp filter) *revokes* anon executable
  memory on Android — so this works at startup (before the monitor) but the
  `mprotect(RX)` is denied for programs started via emulated `execve` afterward.
- **file-backed** — `mmap` each segment `PROT_EXEC` straight from the file (+
  anon BSS). Needs no `execmem`; works on an exec-permitted mount (e.g. Termux
  app-data, which forbids `execve` but allows file-backed execute — the actual
  reason chroot-ng is needed there). Doesn't defeat a true `noexec` mount.

We probe anon exec memory right after installing the monitor and, if it's been
revoked, switch to file-backed for subsequent loads (with a per-load fallback as
a backstop). This single component also:

1. defeats `noexec` (the whole reason we can't just `execve`),
2. is libc-agnostic by construction (we are the loader — glibc/musl/static/
   dynamic all work with no version pinning),
3. lets the in-process interception survive `execve` (we emulate `execve` by
   re-running the loader while keeping our monitor resident).

### Shared component 2 — path translation core

The well-trodden `proot`-equivalent logic, independent of interception
mechanism: enumerate the ~40 path-bearing syscalls, canonicalize, apply the
bind list + guest rootfs, guard against symlink escape, and track the virtual
cwd/root. `/proc` is its own zone: it passes through to the host (a rootfs
directory tree has none), non-guest pids are hidden, the magic links
(`exe`/`cwd`/`root`, and the `fd` links' targets) are answered in guest terms,
and the files that would otherwise describe chroot-ng — `cmdline`, `environ`,
`auxv`, `maps`, the mount tables — are served from the guest's own view (and
the global files Android's SELinux hides, `version`, `loadavg`, `uptime`, `stat`
and `sys/kernel/overflow{u,g}id`,
answer `stat` and `access` as well as `open`: a refused call on a name we serve
gets the attributes of a `/proc` regular file). Because
we run the guest in this process and never `execve`, those files are the kernel's
record of *our* invocation, so this is a correctness requirement, not polish.
`cmdline` and `environ` are the exception that can be fixed at the source, as
comm is with `PR_SET_NAME`: where the kernel accepts `PR_SET_MM_MAP` its record
is pointed at the guest's strings, and where it does not (Android) the ranges
the loader recorded are read out of guest memory under the kernel's rules, so a
title written over argv shows either way (M100).
See `src/chroot-ng/monitor/procfs.c` (synthesis) and `src/chroot-ng/monitor/procreg.c` (the
fork-inherited registry that tells a guest pid from a host one). The calls that
name a process by pid rather than by path follow the same rule: a hidden pid
is ESRCH to `process_vm_readv`/`writev`, `pidfd_open` (M46) and the signal
family, and `kill`'s group and broadcast forms reach only the guest processes
in them (M85).

The walk produces a host path; the syscall is not made on it. A string handed
to the kernel is resolved again from scratch, following whatever is there by
then, and between the walk and the call the tree is the guest's to change: a
directory on the way swapped for an absolute symlink is followed from the host
root, and the call lands outside the rootfs — the time-of-check-to-time-of-use
of every string-based translator, `proot` included. So every host path is
*pinned* for its syscall (`src/chroot-ng/monitor/pin.c`): the directory the walk reached
is opened `O_PATH` and verified to be that directory (the kernel's own name for
it, read back through its fd link, against the walk's symlink-free spelling;
where the two differ in spelling alone, as on a case-insensitive filesystem,
a descriptor-by-descriptor walk of the components from the rootfs or bind
prefix decides), and the call is made against that descriptor with the last
component as a plain name and the family's NOFOLLOW — an open as an `openat2`
with `RESOLVE_NO_SYMLINKS`, which leaves no flag on the description, where the
host has it. The walk followed everything that was to be followed, so NOFOLLOW
changes nothing for a tree at rest and refuses exactly the link that appeared
in the race. Families with no NOFOLLOW to give, and names with a trailing
slash (which the kernel follows regardless), go through the last component's
own `O_PATH` descriptor, checked not to be a symlink, by its fd link. A
pathname `bind` carries the pinned directory's link in `sun_path`, and the
binder keeps that descriptor open for as long as the socket is, so the stored
spelling reads back to the guest's name from any process (`unixsock.c`). The
loader's opens and `-l`'s own bookkeeping take the same route. What is not
pinned: a relative name against the guest's own dirfd (already a descriptor;
the NOFOLLOW is all it needs), the host's `/proc`, and a `/dev` whitelist
node named as itself.

Directory descriptors are part of the same containment: a name relative to a
dirfd is resolved by the kernel with no rootfs in the way, so every directory
the guest can hold a descriptor on must have a guest name — inside the view,
or one of the two zones — and the walk from it is ours. A descriptor on a
directory the guest cannot name is never let into its table: the launcher's
are closed before the first program loads, and one arriving later over a
socket or from `pidfd_getfd` is closed on arrival (`cng_fd_admit`,
`src/chroot-ng/monitor/dispatch.c`). On arrival is as early as it can be — the kernel
has installed the descriptor by then, and for the interval up to the close it
is in the shared table for any thread of the guest to use; the threat model
above says what that is worth. The numbers judged are the kernel's own: the
control data of a `recvmsg` or `recvmmsg` is received into a buffer of the
monitor's and handed to the guest afterwards, so a thread of the guest cannot
rewrite the record between the kernel's write and the judgement. Files are let
in for the I/O they carry; a file is not a place to resolve a name from.

A file descriptor is the whole capability it is under `chroot(2)`, and
deliberately so. What the launcher hands over by descriptor — a redirected
stdin, a script or a data file on fd 3, one received over a socket — the
guest can read, write as the description's mode allows, execute
(`execveat(fd, "", AT_EMPTY_PATH)`, `fexecve`, `/proc/self/fd/N`) and give
a name of its own with `linkat(fd, "", …, AT_EMPTY_PATH)` where the kernel
lets an unprivileged process link by descriptor, exactly as a chrooted
process can with a descriptor it inherited. None of that resolves a name
against the host, so none of it is the walk's to refuse — and refusing it
would break every launcher that passes a program or its input by
descriptor. A launcher that must not let the guest execute, or keep, a
host file does not hand it the descriptor.

### Shared component 3 — the IPC broker

A detached per-namespace daemon (`src/chroot-ng/monitor/broker.c`) that owns shared state
no guest process can hold itself, because **host fd == guest fd** here: anything
we keep open is visible to — and closable by — the guest. Over one abstract-socket
rendezvous it serves the `--shared-proc` PID table (as a memfd) and the whole of
**System V IPC**, which Android leaves us no choice but to emulate: every syscall
in the family is denied, and there is no writable tmpfs for `/dev/shm`.

Shared memory needs only an owner for the pages: each segment is an ashmem
region (where `/dev/ashmem` opens) or an anonymous memfd the daemon holds and
hands to attachers over `SCM_RIGHTS`, and `shmat` maps it `MAP_SHARED` — into our
own address space, which is also the guest's — then closes the fd at once, so a
process holds a segment only as a mapping (`src/chroot-ng/monitor/shm.c`). Host processes
reach it too, through libandroid-shmem's protocol: a shmid's high half names a
`"/dev/shm/%08x"` socket the daemon answers on, which is how a Termux X server
attaches the segment a guest X client hands it in `XShmAttach`. Semaphore sets and message queues instead live *entirely*
in the daemon (`src/chroot-ng/monitor/ipcreg.c`), because they need an arbiter rather than
a landlord: with every operation an RPC, all mutation is single-threaded, a
multi-operation `semop` is atomic for free, and a guest that dies mid-call cannot
leave the registry torn. An operation that must sleep parks its connection there
and is answered when it can proceed, when its deadline passes, when the object is
removed, or when the caller cancels it — which is how a blocking `semop` stays
interruptible even though our SIGSYS handler runs with every other signal masked.

It is also the far end of every **emulated rtnetlink socket**
(`src/chroot-ng/monitor/netlink.c`). The guest's socket is one end of an AF_UNIX
sequenced-packet pair and the daemon holds the other, polls it, and answers each
request the moment it arrives — the kernel answers inside the `sendmsg` that
carries one, and a client may `write(2)` a request and `read(2)` the reply with no
call in between that the seccomp tier traps, which is what Mozilla's `mtu` crate
does for Firefox's HTTP/3. A send the monitor does trap waits for the daemon to
have served it, so its replies are queued when the call returns. The pair's
hang-up tells the daemon when the guest has closed its socket, since `close(2)` is
not trapped.

The daemon uses those registries as its own liveness signal and exits once
nothing is left, leaving no file and no socket name.

### Interception mechanism — tiered, auto-selected

| Tier | Mechanism | Min kernel | Notes |
|------|-----------|-----------|-------|
| primary | seccomp `RET_TRAP` → in-process `SIGSYS` | 3.5 | one signal per path syscall, no second process; handler translates into its own buffer and re-issues via the gate |
| upgrade | seccomp `RET_USER_NOTIF` → supervisor | 5.0 | out-of-process, sheds SIGSYS/execve/clobber fragility where available |
| fallback| ptrace + seccomp `RET_TRACE` | 3.5 (3.4 plain) | correctness backstop; cannot defeat true `noexec` |

**Correctness floor = seccomp `RET_TRAP`/`SIGSYS`.** One BPF filter traps the
path-bearing syscalls unless the syscall's `instruction_pointer` is inside our
gate `[__cng_gate_start, __cng_gate_end)`. Every raw `svc` from anywhere —
glibc, musl, Go's runtime, Rust, JIT — traps synchronously, in-process, to the
`SIGSYS` handler, which reads args from the `ucontext`, translates the path into
its own buffer, re-issues the real syscall through the gate, and writes the
result back into the return register. Non-path syscalls run natively. A trapped
call that can block — a socket call on a blocking descriptor, a FIFO's open,
a tracee's read — is not made in the handler, which runs with every signal
masked and would leave it reachable by nothing but `SIGKILL`: the handler
returns into a stub in the gate that makes the call from the guest's own
context, the guest's call itself or the whole dispatch, and a second trap puts
the registers back (`sigsys.c`, the bounce).

**The mmap hook is the choke point** for `noexec`-defeat of dynamically-loaded
code: when `ld.so` tries to `mmap(PROT_EXEC)` a `.so` from the `noexec` rootfs
(which fails natively), we intercept it and read+map the file into anon RW→RX
(`src/chroot-ng/monitor/execmap.c`). The filter tests the arguments rather than the
syscall — `PROT_EXEC` set, `MAP_ANONYMOUS` clear — so every anonymous
allocation a guest makes stays untrapped and only a library's text mapping
reaches the handler. The conversion runs *after* the kernel has refused, so an
exec-permitted mount keeps its real file mapping, its page-cache sharing and
its identity in `/proc/self/maps`; only `MAP_PRIVATE` can be served this way,
since a copy cannot carry `MAP_SHARED`'s visibility.

**The host's own code is let through as the gate is.** The libc chroot-ng runs
on makes syscalls of its own once the monitor calls into it -- a thread's
creation, its signal masks, fork, the heap -- so every filter of ours admits a
syscall whose instruction pointer is in an executable segment of an object the
dynamic linker loaded: our image, and the libc and its loader where they are
apart from it. The vDSO is left out, and so is a section of ours, `cngtrap`,
which holds the two svcs that are to trap (the filter probe, the bounce's
completion). The guest's images are none of these: our loader maps them.

**Guest threads are the host's.** A clone that makes a thread is trapped and
made as a pthread of the host libc's on a stack of ours (`monitor/thread.c`),
which enters the guest by `rt_sigreturn` on a frame built from the trap: the
parent's registers with x0 = 0 and the new stack, its FP/SIMD registers, its
mask, and `CLONE_SETTLS`'s thread pointer or the parent's. The new thread first
gives back what its libc gave it that a kernel's new thread has none of (rseq,
a robust list, a clear_child_tid) and takes what the clone asked for
(`CLONE_CHILD_CLEARTID`, the tid words, a private descriptor table or fs). The
guest's robust list and clear_child_tid stay the kernel's, untrapped, so the
kernel processes them when the thread dies however it dies. A thread's exit is
trapped and unwinds to its start routine, for the libc to end the thread as it
ends its own -- the leader's is the kernel's, its code being the group's -- and
an exec's de_thread ends its siblings the same way. Each thread of ours has a
kernel alternate signal stack of ours while the guest has none, so the SIGSYS
frame of an exit made after unmapping the thread's own stack (musl's
`__unmapself`, bionic's teardown) has somewhere to go; `sigaltstack` is trapped
and answers with the guest's own. A plain fork is the libc's `fork()`, the
tid words then the guest's call's. The handler's entry switches to the host
context of the thread it lands on -- the pthread's TCB and shadow call stack --
found by tid.

**The filter is the whole of the interception.** When it cannot be installed
there is nothing left that intercepts, and the invocation — a rootfs, binds,
`-u`, `-l`, `--shared-proc` — cannot be delivered at all, so `cng_run` refuses
to enter the guest rather than run it against the host's own paths. (An `svc`
rewriting tier, `-R`, once extended translation to hosts with no filter and
patched sites to trampolines; it was dropped as slower than the trap for an
untranslated call and as the one tier that wrote the guest's text.)

### Coexisting with Android's own seccomp filter

On Android our process already carries the zygote's app seccomp filter, whose
action for non-allowlisted syscalls is `SECCOMP_RET_TRAP` (SIGSYS). Filters
stack and the most restrictive action wins, so we cannot un-block what Android
blocks (e.g. `setgid`/`setuid`/`setgroups` — apps may not change credentials).
Two consequences:

1. A guest syscall Android blocks traps to *our* SIGSYS handler (we own the
   disposition). We must not forward it — the real syscall is blocked.
2. When our handler re-issues a *translated* syscall through the gate, Android
   may block that too. Since the re-issue happens inside the handler, a naive
   design gets a masked seccomp SIGSYS → force-kill.

Two layers handle this:

- **Direct emulation (primary).** The SIGSYS handler passes `trapped=1` to the
  dispatcher. Since our filter only traps syscalls we have explicit handlers
  for, anything reaching the dispatcher's `default` was trapped by *Android* —
  so we emulate `-ENOSYS` directly instead of re-issuing. Credential setters are
  likewise emulated in place: against the fake-identity credential set under
  `--fake-id`; else, for a setter the probe below found refused, from the real
  ids and capabilities by the kernel's own rules — 0 for a call that changes
  nothing, EPERM or EINVAL as the kernel would say (M84). Nothing is re-issued,
  so this does not depend on nested signal delivery.
- **Block-list probe (for re-issued path syscalls).** Some syscalls we *do*
  re-issue (translated path syscalls) are also Android-blocked — notably
  `fchownat` and `mknodat`. At monitor install we measure the ambient filter:
  fork a child that (with only Android's filter active) invokes each candidate
  with harmless NULL args and records which trap. The dispatcher then emulates
  the blocked ones as `-ENOSYS` instead of re-issuing. This runs in the child's
  normal flow (not nested), so it does not depend on nested SIGSYS delivery, and
  off Android nothing is blocked so everything re-issues normally.
- **Gate-net (last-resort backstop).** The `SA_NODEFER` gate-net still catches a
  re-issue that the probe missed, but it needs nested seccomp SIGSYS delivery,
  which some kernels don't honor — so it is only a backstop; correctness comes
  from the two mechanisms above.

**Keeping SIGSYS ours and deliverable.** A masked seccomp SIGSYS force-kills, so
the guest must never block SIGSYS or replace our handler. musl does exactly this
at startup (`rt_sigprocmask(SIG_BLOCK, ~[a few RT sigs])`), which neutered us
until we virtualized it. We trap `rt_sigprocmask` and `rt_sigaction`: mask
changes are applied with SIGSYS forced clear (in the SIGSYS handler we edit
`uc_sigmask`, which `sigreturn` restores — re-issuing there would be undone),
`sa_mask` on installed handlers has SIGSYS stripped, and `rt_sigaction(SIGSYS)`
never reaches the kernel: it is answered from a per-process mirror of what the
guest asked for (with the kernel's own EINVAL/EFAULT checks, reset by the
emulated execve as `flush_signal_handlers` would), so the guest can't take over
our slot but sees its own disposition back. A SIGSYS that is not a seccomp trap
— `kill -SYS`, a `raise` — is delivered by our handler to that mirrored
disposition: the default action kills, SIG_IGN drops, a handler runs under its
`sa_mask` (SIGSYS itself being the one bit that stays unblocked).

For a real container you want `-u`/`--fake-id` (fake user identity, default
`0:0` root), which emulates the credential syscalls against a synthetic
credential set instead of returning ENOSYS — mirroring proot's `-0`/`-i` and the
way the reference emulator recommends `--fake-id` for apt/dpkg. Each process's
set is its own, inherited across fork, and every guest process still has the
host's ids in the kernel, which is what it tells a socket peer about. So a
process also publishes its set in the guest-PID registry, and `SO_PEERCRED` and
a received `SCM_CREDENTIALS` record name a guest peer by it. After `su - user`,
a session bus is then told its clients are that user (M102). The groups are
published with the ids, and `SO_PEERGROUPS`, another process's status file and
the owner of its `/proc/<pid>` (by which `ps` names its user) come from there
too (M104). Capabilities are all of them for a fake root and none otherwise,
and a `capset` is judged against that set by the kernel's rules, so dropping
them is allowed to anyone (M103).

### Guest ptrace

chroot-ng uses no ptrace, but guests do — `strace`, `gdb`, `proot` — and letting
those reach the host kernel is worse than refusing them: the tracer sees our own
re-issued syscalls, reads host paths with the rootfs prefix attached, and never
gets the post-`execve` `SIGTRAP`, since our execve is emulated and never enters
the kernel's exec path. So ptrace is emulated like every other kernel service
here, from inside the processes involved: a shared registry created before the
first fork holds one link per traced task, a tracee publishes its stop there and
then parks in a service loop answering `PEEK`/`POKE`/`GETREGSET`/`SETREGSET`/
resume *about itself* over a futex mailbox, and the tracer discovers stops from
its `wait4`. Nothing in that needs host ptrace permission, `/proc/pid/mem` or
`process_vm_readv`, all of which Android's policy can deny.

The stop points are exactly where the guest already reaches our code — the
SIGSYS trap, our signal handlers, the emulated execve, clone and exit — and at
each of them the full register file is addressable, because the AArch64
sigcontext's `regs/sp/pc/pstate` tail *is* a `struct user_pt_regs`. Two things
have to be added on top: a traced task stacks a second filter that traps every
syscall (the base filter only traps the path-bearing set, and strace must see
all of them), and while traced its signals are mediated by handlers of ours that
mirror the guest's own flags — which is what makes a gdb breakpoint, a `brk`
poked into read-only text, arrive as a stop rather than kill the guest. Such a
handler blocks every other signal while it works, as `get_signal` takes one
signal at a time, and runs the guest's handler under the mask the kernel would
give it. The signal a tracer resumes a stop with is delivered by its own
disposition with no stop of its own, and a stop signal's default action is a
group stop. See `src/chroot-ng/monitor/ptrace.c`, `ptsig.c` and `ptstep.c`.

A signal stop publishes the signal's whole siginfo, which `PTRACE_GETSIGINFO`
hands over: the sender, a `SIGCHLD`'s status. An emulated execve gives the
stops a kernel's gives, in its order: the entry stop, `PTRACE_EVENT_EXEC`, and
the exit stop with 0 — or, without the option, the exit stop and then the
legacy `SIGTRAP` (none for a `SEIZE`'d tracee). strace pairs entry and exit
stops by counting them, so one left out puts a whole trace out by a stop.

A stop signal sent to a tracee cannot be a real one, which would freeze the
tracee in its service loop and its tracer with it. It is taken cooperatively,
as the kernel takes one: by one thread, with its signal-delivery stop and the
sender's siginfo first, and then what the tracer resumed it with — nothing, the
group stop for a stop signal at its default (each traced thread of the group
takes one), or delivery by the signal's own disposition. The real parent of a
traced child, when it is not the tracer, is told of the group stop and of the
`SIGCONT` that ends it, as `do_notify_parent_cldstop` tells one: `SIGCHLD`,
and a report in the child's link that the parent's wait collects for
`WUNTRACED` or `WCONTINUED`, since the kernel's own wait never sees a stop the
child did not make in the kernel. That reaches a parent whose waits come to
chroot-ng — a tracer's, a tracee's; an untraced parent's wait goes to the
kernel and does not see it. `SIGCONT` itself is
sent as well, to be stopped for and delivered like any other signal. A wait the
`SIGSYS` handler polls for a tracer or a tracee is cut short by a signal the
guest takes delivery of the way `do_wait` is, with `ERESTARTSYS`: made again
under an `SA_RESTART` handler, `EINTR` without one.

`NT_ARM_SYSTEM_CALL` is the kernel's `regs->syscallno`: the number of the call
in progress — x8's low 32 bits, as the tracer left it at the entry stop — at
the call's own stops (entry, exit, the events it reports), and -1 at every
other, where what a tracer writes lasts until the kernel is entered again: the
next trap, or a frame other than the one the last such stop left, which the
guest ran on to. `orig_x0`, x0 as the trap found it -- or as the tracer leaves
it at the entry stop, for a call that is made (e057b9477232, in every stable
kernel) -- is kept with the call: `PTRACE_GET_SYSCALL_INFO` reports it, and a
restart is made from it.

A signal of the guest's also lands wherever the thread is. In guest code it is
stopped for there, the kernel's place. In a call the dispatcher makes from
guest context (a bounced one) it is not: the kernel stops
for a signal on the way back to user space, after the call's exit stop. So the
handler holds it back — pending again with its own siginfo, blocked by the
frame's mask — and the end of the dispatch lets it go at a point of ours
(`cng_pt_deliver` in the gate), where the handler puts the frame back to the
guest's registers before the stop is reported and the guest's handler runs. One
that interrupts the guest's own call (`reissue_svc`) cuts the call short as the
kernel's would: the exit stop shows the call's restart code (`-ERESTARTSYS` for
a read), and the signal's stop finds the call at its `svc` with `orig_x0`, to
be made again after it, or turned into `EINTR` by a handler without
`SA_RESTART`. One pending as that call is about to be made is let go just
before it (`cng_pt_unhold`), and the call is made after it. The bounce stubs
are put back in place, as for a kick. A stop signal a call
sends its own thread is taken once the call is over, the same way.

A running task is reached by a kick signal of ours, and the kick lands wherever
the thread is — which, for a thread blocked in a call made on its behalf, is
inside chroot-ng: a bounce stub, or the dispatcher itself.
The stop has to show what the kernel shows there, the thread's own registers
with the call's arguments in place and `pc` at its syscall instruction, and the
call has to be made again when it ends, with whatever the tracer left in the
registers. The stubs hold the guest's registers in a
record, so the kick's frame is put back to the guest's state, as if the call
were not yet made or already over. The dispatcher's frames cannot be stepped
out of: its stop reports the guest's registers from the dispatch's own record,
and a tracer that changes them abandons the call in flight, which is then made
again as the tracer left it. The kick is `SA_RESTART`, so a call it interrupts
is restarted rather than failed with `EINTR`. That covers what the kernel
restarts after any `SA_RESTART` handler. A `poll`, `select`, `nanosleep` or
timed futex the guest makes natively is one the kernel restarts only when no
handler runs at all, and the kick is a handler, so after an attach such a call
still returns `EINTR` (a known gap, recorded under M106 in STATUS.md).

The kick is an ordinary signal, and a thread can block it, where the kernel's
attach is a `SIGSTOP` no mask holds back. To a thread whose mask holds the
kick, the request also goes as a `SIGSYS` of ours, the one signal a guest can
never block, and is sent again while it is not taken. A standard signal
absorbs a trap the thread takes while it is pending, so the handler makes a
call that was skipped that way again. It judges whether the call trapped by
running the installed filters over it, and whether a handler of ours already
answered it by the frame that handler returned to.

Which filters a task carries is kept per task, but a thread or a fork child
that a traced task makes later inherits the filter from the kernel without
that record. So each filter a trace stacks tags its traps with data the
handler receives as `si_errno`, and the first tagged trap tells the new task
what it carries. A trap displaced as above has no tag. For that one the
handler asks the kernel with a call only the filter in question traps.

### Known hazards (why the tiers exist)

- `execve` erases the in-process handler → emulate `execve` via the loader.
- `execve` from a multithreaded program: the kernel's `de_thread` kills every
  other thread and gives a non-leader caller the leader's identity, and neither
  can be had from userspace directly → every sibling is sent a thread-directed
  `SIGSYS` carrying a die request (the one signal the guest can never block),
  and a non-leader caller hands its planned exec to the group leader over the
  same signal and exits, so the leader carries it and the new program is one
  thread whose tid is its pid. For that to reach every thread, the waits that
  install a mask of their own (`rt_sigsuspend`, `rt_sigtimedwait`, `ppoll`,
  `pselect6`, `epoll_pwait[2]`) and `signalfd4` are trapped when they carry a
  mask and `SIGSYS` is taken out of it; on the `SIGSYS` tier such a wait is run
  from the guest's own context through a stub in the gate, since the handler,
  which runs with every other signal blocked, cannot run it itself.
- A call that blocks inside that handler cannot be interrupted at all: a
  server in `accept` outlived `SIGTERM`, `^C` did not reach a FIFO's `open`, a
  guest handler ran only after the call returned, and a ptrace attach, whose
  kick is masked with the rest, hung its tracer → every trapped call that can
  block is bounced into guest context the same way, where the kernel's own
  rules interrupt and restart it.
- The guest (notably Go) can clobber the `SIGSYS` handler or block the signal →
  virtualize `rt_sigaction`/`rt_sigprocmask`/`seccomp`/`prctl`. A guest filter is
  layered on top of ours by the kernel and governs the syscalls the handler
  re-issues through the gate as well, so `seccomp(2)` is refused `ENOSYS` and
  `prctl(PR_SET_SECCOMP)` `EACCES`; `PR_GET_SECCOMP` and the `NO_NEW_PRIVS` pair
  report the guest's own state rather than the bits we set to install the filter
  — and the `no_new_privs` half is kept per *task*, the way `task_struct` keeps
  it, so one thread setting it does not answer for its siblings.
  The remaining prctl ops are real process state and stay untrapped (the filter
  tests `args[0]`).
- `SIGSYS` signal-stack correctness on guest-created threads → per-thread
  `sigaltstack`, validate against Go.
- `execmem` denied → no in-process path exists on a true `noexec` mount; only
  the ptrace fallback (from an exec-permitted location) remains.

## Build & test

Cross-compiled with `aarch64-linux-gnu-gcc`. On an x86_64 host it runs under
arm64emu's chroot mode, which applies a guest's seccomp filter (`RET_TRAP`,
`SIGSYS`) as the kernel does — qemu-user does not, and cannot run it at all —
and natively on AArch64; every test is a guest's, through the filter
(docs/chroot-ng/testing.md).

The binary is an ordinary PIE linked against its toolchain's libc, whose
startup runs before `main`: the monitor makes its syscalls through its own gate
and calls nothing of the libc but its mem/str, so the SIGSYS handler has no
re-entrancy hazards and no dependence on the libc it happens to be linked with.
It is an ordinary build too. Every handler of ours is entered through one
entry (`monitor/hentry.S`) that runs it on the host's thread pointer and gives
the interrupted one back on the way out; the guest's FP/SIMD registers are the
signal frame's to keep in a handler, and a save area's of the gate's in a
dispatch bounced to guest context (`rt/gate.S`, `cng_bounce_dispatch`), which
hands them back for the guest's own call and to a ptrace stop. The stack
protector's canary is the global `__stack_chk_guard`, not Bionic's TLS slot: a
bounced dispatch runs on the guest's thread pointer. Its
syscall numbers are its own table too (`src/chroot-ng/include/cng/unistd.h`, the stable
AArch64 ABI spelled out) rather than the build host's `<asm/unistd.h>`: what
the filter traps and refuses must not depend on how old the headers were where
the binary happened to be built, and `src/chroot-ng/rt/unistd_check.c` holds the table
against those headers at build time where the host has them.
