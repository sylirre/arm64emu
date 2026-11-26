# M8: the guest's own context (sourced by tests/chroot-ng/run.sh). Threads,
# vfork-style clones, interrupted waits and the stacks a guest makes its
# syscalls on: what the monitor runs inside of, and has to hand back as it was.
echo "== M8: guest context: threads, vfork, waits, stacks =="

ROOT=$(mktemp -d)
mkdir -p "$ROOT/bin" "$ROOT/etc"
# shellcheck disable=SC2086  # $CHROOTNG_GUEST_BINDS is a deliberately split arg list
m8run() { run_t 120 $CHROOTNG_GUEST_BINDS "$@"; }

# Threads that each open a guest path and do floating-point work across the
# calls: a thread that comes up with a damaged register file, or without a stack
# for the dispatcher, shows as a wrong count. Differential against the same
# program run straight under the kernel, which is what "threads work" means.
if guest_xlate_ready "threads leg" &&
    guest_cc_report "$ROOT/bin/threads" tests/chroot-ng/guests/threads.c -lpthread; then
    printf 'THREADS-OK\n' > "$ROOT/etc/threads-marker"
    want=$(emu_t 60 "$ROOT/bin/threads" "$ROOT/etc/threads-marker" 2>/dev/null)
    out=$(m8run "$ROOT" /bin/threads /etc/threads-marker 2>/dev/null); rc=$?
    check "a threaded guest runs" 0 $rc
    check_contains "every thread started, opened a guest path and kept its FP" \
        "threads: started=8 reads=8 tids=8 fp=8" "$out"
    if [ -n "$want" ]; then
        check "and matches the same program under the kernel" "$want" "$out"
    else
        skip "threads kernel differential: the reference run produced nothing"
    fi
fi

# A wait is the kernel's own unless something could owe it an emulated ptrace
# stop: a handler without SA_RESTART ends it with EINTR, one with it does not.
# Differential against the same program under the kernel.
if guest_xlate_ready "waits leg" &&
    guest_cc_report "$ROOT/bin/waitintr" tests/chroot-ng/guests/waitintr.c -lpthread; then
    want=$(emu_t 60 "$ROOT/bin/waitintr" 2>/dev/null)
    out=$(m8run "$ROOT" /bin/waitintr 2>/dev/null)
    if [ -z "$want" ]; then
        skip "interrupted waits: the reference run produced nothing"
    elif [ "$want" = "$out" ]; then
        pass=$((pass + 1))
        echo "  ok   waits are interrupted and restarted as the kernel's"
    else
        fail=$((fail + 1))
        echo "  FAIL waits diverge from the kernel"
        printf '       kernel: %s\n       guest : %s\n' \
            "$(echo "$want" | tr '\n' '|')" "$(echo "$out" | tr '\n' '|')"
    fi
fi

# An execve is emulated in-process (translated, monitor kept): re-issued raw, it
# would look the guest path up on the host and fail.
if guest_xlate_ready "execve leg" &&
    guest_cc_report "$ROOT/bin/hello" tests/chroot-ng/guests/hello.c &&
    guest_cc_report "$ROOT/bin/execer" tests/chroot-ng/guests/execer.c; then
    out=$(m8run "$ROOT" /bin/execer 2>/dev/null); rc=$?
    check "an execve exits through the exec'd guest" 42 $rc
    check_contains "exec'd program got its argv" "argv1=from-execer" "$out"
fi

# A vfork-style clone becomes a COW fork because our execve is emulated
# in-process; the child comes back through the dispatcher's frames on the
# scratch stack and has to leave onto the stack the guest handed it — glibc's
# vfork hands the parent's own sp, posix_spawn a stack of its own.
if guest_xlate_ready "vfork leg" &&
    guest_cc_report "$ROOT/bin/hello" tests/chroot-ng/guests/hello.c &&
    guest_cc_report "$ROOT/bin/vforker" tests/chroot-ng/guests/vforker.c; then
    want=$(emu_t 60 "$ROOT/bin/vforker" "$ROOT/bin/hello" 2>/dev/null |
        grep -v '^guest: ')
    out=$(m8run "$ROOT" /bin/vforker /bin/hello 2>/dev/null); rc=$?
    check "a vforking guest runs" 0 $rc
    check_contains "a bare vfork child exits on the parent's stack" \
        "vfork-exit: exited 7" "$out"
    check_contains "a vfork child execs" "vfork-exec: exited 42" "$out"
    case "$out" in
    *"posix_spawn: none in this libc"*)
        skip "posix_spawn: the guest's libc has none (bionic before API 28)" ;;
    *)
        check_contains "a posix_spawn child resumes on its own stack and execs" \
            "spawn-exec: exited 42" "$out" ;;
    esac
    check_contains "the exec'd program got its argv" "argv1=from-vforker" "$out"
    if [ -n "$want" ]; then
        check "and the outcomes match the same program under the kernel" \
            "$want" "$(printf '%s\n' "$out" | grep -v '^guest: ')"
    else
        skip "vforker kernel differential: the reference run produced nothing"
    fi
fi

rm -rf "$ROOT"

# An exec chain with BOTH programs non-PIE. Two ET_EXEC images share the
# link-time vaddr 0x400000, so the MAP_FIXED load of the new one lands on the
# .rodata/heap holding the very argv strings the caller passed — and those reach
# the new stack only after the load. The exec'd program still ran and still
# exited 42, so nothing but an argv assertion catches it. Built here with an
# explicit -static -no-pie rather than whatever link mode the harness picked,
# since the collision needs the fixed vaddr: on the usual static-PIE guest each
# image gets its own base and the bug is invisible.
XR=$(mktemp -d); mkdir -p "$XR/bin"
if [ -n "$CHROOTNG_GUESTCC" ] &&
    $CHROOTNG_GUESTCC -static -no-pie -O2 -o "$XR/bin/hello" tests/chroot-ng/guests/hello.c \
        2>"$GUEST_CC_LOG" &&
    $CHROOTNG_GUESTCC -static -no-pie -O2 -o "$XR/bin/execer" tests/chroot-ng/guests/execer.c \
        2>>"$GUEST_CC_LOG"; then
    check "exec chain: both programs are fixed-address ET_EXEC" EXEC \
        "$(elf_type "$XR/bin/execer")"
    out=$(run_t 60 "$XR" /bin/execer 2>/dev/null); rc=$?
    check "ET_EXEC execs ET_EXEC at the same vaddr: exit code" 42 $rc
    check_contains "argv0 survives the colliding load" "argv0=/bin/hello" "$out"
    check_contains "argv1 survives the colliding load" "argv1=from-execer" "$out"
    # envp goes through the same snapshot, so guard it too — though not against
    # this bug: execer passes the environ it inherited (the environment chroot-ng
    # built from -E), which lives on the original stack and no image load
    # touches. This one is a check on the copy itself.
    out=$(run_t 60 -E CHROOTNG_TEST=collide "$XR" /bin/execer 2>/dev/null)
    check_contains "the environment still reaches the exec'd program" \
        "CHROOTNG_TEST=collide" "$out"
else
    skip "fixed-vaddr exec-collision leg: no -static -no-pie AArch64 toolchain"
fi
rm -rf "$XR"

# A translated syscall must stay inside the stack the guest was running on. The
# dispatcher is deep — ~24 KiB for cng_dispatch's frame alone, ~66 KiB for a
# whole openat translation — so the handler switches to a scratch stack of its
# own before entering it; all that goes on the guest's is the kernel's signal
# frame. A guest that runs syscalls on a small stack (musl's 128 KiB threads,
# Go's ~8 KiB goroutines, any sigaltstack) would have the rest written off its
# end, and not as a crash: a frame bigger than a guard page steps over the guard
# into ordinary guest memory. The guest sets SP into a small region itself and
# counts how many bytes of a canary below it moved.
SSR=$(mktemp -d)
mkdir -p "$SSR/etc"
printf 'guest\n' > "$SSR/etc/hostname"
if guest_xlate_ready "small-stack syscall leg" &&
    guest_cc_report "$SSR/smallstack" tests/chroot-ng/guests/smallstack.c; then
    for _k in 8 16 64; do
        # shellcheck disable=SC2086  # $CHROOTNG_GUEST_BINDS is a deliberately split list
        out=$(run_t 60 $CHROOTNG_GUEST_BINDS "$SSR" /smallstack $_k 2>/dev/null)
        check "a syscall on a ${_k} KiB guest stack writes nothing below it" \
            "clobbered=0" "$out"
    done
    # ...and again with no slot in the scratch table to be had: 256 live threads
    # already holding one, an mmap the host refused, or an outer dispatch on this
    # thread that is on the slot already (a nested trap, which a guest with its
    # own sigaltstack reaches from inside its own signal handler). Falling back
    # to the interrupted stack is the very failure the legs above measure.
    # CHROOTNG_SCRATCH_NONE forces the case, since a working host does not reach
    # it on its own.
    for _k in 8 16 64; do
        # shellcheck disable=SC2086  # $CHROOTNG_GUEST_BINDS is a deliberately split list
        out=$(CHROOTNG_SCRATCH_NONE=1 run_t 60 $CHROOTNG_GUEST_BINDS "$SSR" /smallstack $_k \
            2>/dev/null)
        check "...and still nothing with no scratch slot to be had (${_k} KiB)" \
            "clobbered=0" "$out"
    done
fi
rm -rf "$SSR"

# A thread that frees its own stack on the way out. musl's __unmapself and
# bionic's _exit_with_stack_teardown are an munmap of the stack under the caller
# and an exit, with nothing between that touches the stack — which is how every
# detached thread of those libcs ends (the JVM's all are). A trap of the exit
# has no stack to put its frame on, and the kernel kills the whole process for
# it. The sequence is written out as libc has it, on a stack the guest mapped,
# and each of twenty threads is started on a fresh one.
UMD=$(mktemp -d)
if guest_xlate_ready "stack-teardown leg" &&
    guest_cc_report "$UMD/unmapself" tests/chroot-ng/guests/unmapself.c; then
    for _v in musl bionic; do
        # shellcheck disable=SC2086  # $CHROOTNG_GUEST_BINDS is a deliberately split list
        out=$(run_t 60 $CHROOTNG_GUEST_BINDS "$UMD" /unmapself $_v 2>/dev/null)
        check_contains "threads freeing their own stack survive ($_v's sequence)" \
            "survived=20" "$out"
    done
fi
rm -rf "$UMD"

# The program break. The kernel's is the host libc's heap, and the guest's is
# kept apart from it (monitor/brk.c) by the kernel's rules: exactly the break
# asked, pages given back on a shrink and zero-filled when they come back, a
# growth refused into the next mapping and up against it, RLIMIT_DATA, a fork
# child's own, none of it left after an exec, and [heap] in maps naming the
# guest's heap and no other. The expected block is a kernel's (6.8, static and
# dynamic glibc alike); arm64emu's suite runs the same program.
BKD=$(mktemp -d)
if guest_xlate_ready "program-break leg" &&
    guest_cc_report "$BKD/brkheap" tests/chroot-ng/guests/brkheap.c; then
    want=$(cat <<'EOF'
brk(0) answers the break: yes
below start_brk: unchanged
grow by 100: exactly as asked
grow by three pages: exactly as asked, mapped to the break
shrink: exactly as asked, pages given back
grow back: exactly as asked, zero-filled
unaligned break kept: yes
into a mapping: refused
up against a mapping: refused
a page short of a mapping: allowed
the mapping is untouched: yes
past every address: refused
past RLIMIT_DATA: refused
RLIMIT_DATA 0: refused
limit lifted: allowed
maps [heap]: named, end to end, ending at the break
fork: the child grew its own, the parent's break unchanged, its heap its own
exec: the old heap is gone
EOF
)
    # shellcheck disable=SC2086  # $CHROOTNG_GUEST_BINDS is a deliberately split list
    out=$(run_t 60 $CHROOTNG_GUEST_BINDS "$BKD" /brkheap 2>/dev/null)
    if [ "$want" = "$out" ]; then
        pass=$((pass + 1))
        echo "  ok   the guest's program break answers as the kernel's"
    else
        fail=$((fail + 1))
        echo "  FAIL the guest's program break diverges from the kernel's"
        printf '       kernel: %s\n       guest : %s\n' \
            "$(echo "$want" | tr '\n' '|')" "$(echo "$out" | tr '\n' '|')"
    fi
fi
rm -rf "$BKD"

# What a program finds at its first instruction, and again past an exec of
# itself made after setting each piece of it: no thread pointer (the libc
# chroot-ng runs on sets one), no alternate signal stack and every caught
# signal at its default (Bionic installs both before main), an ignored one
# still ignored, no rseq area (glibc registers one), and none of the
# descriptors above 2 its exec'er left it unless --keep-fds -- where one on a
# directory outside the view is closed all the same. The kernel keyrings
# answer ENOSYS unless --host-keyring, and PR_PAC_RESET_KEYS answers as the
# kernel's checks do. Freestanding, with an entry of its own: the reference is
# the same program under the kernel, with every descriptor it was given.
ESD=$(mktemp -d)
# Outside the view, which on Termux is not $TMPDIR: $PREFIX is bound.
ESO=$(cng_outside_binds "$(mktemp -d)")
mkdir -p "$ESD/in"
# Built without the link mode the suite probed, as M3's freestanding guest is:
# there is no libc, and so no self-relocation a static-pie would need. gcc
# also needs telling not to make a loop a memset call; clang has no such flag.
_esf=-fno-tree-loop-distribute-patterns
$CHROOTNG_GUESTCC $_esf -E -x c /dev/null >/dev/null 2>&1 || _esf=
if ! guest_xlate_ready "entry-state leg"; then
    :
elif ! $CHROOTNG_GUESTCC -nostdlib -static -ffreestanding -fno-builtin \
    -fno-stack-protector $_esf -O2 \
    -o "$ESD/entrystate" tests/chroot-ng/guests/entrystate.c 2>"$GUEST_CC_LOG"; then
    fail=$((fail + 1))
    printf '  FAIL could not build %s\n' tests/chroot-ng/guests/entrystate.c
    sed 's/^/    /' "$GUEST_CC_LOG" 2>/dev/null | head -8
else
    ref=$( (trap '' USR2; emu_t 60 "$ESD/entrystate" 3</dev/null 4<"$ESO" \
        5<"$ESD/in") 2>/dev/null)
    out=$( (trap '' USR2; m8run "$ESD" /entrystate 3</dev/null 4<"$ESO" \
        5<"$ESD/in") 2>/dev/null)
    for _l in "entry: tpidr_el0 zero" "entry: no alternate stack" \
        "entry: caught: 0, SIGUSR2 ignored" "exec: tpidr_el0 zero" \
        "exec: no alternate stack" "exec: caught: 0, SIGUSR2 ignored" \
        "entry: fds: none" "exec: fds: none" "keyctl: ENOSYS"; do
        check_contains "a program starts as an execve starts it: $_l" "$_l" "$out"
    done
    # The kernel's own answers, where it lets the program finish: Android's
    # filter kills it at its rseq.
    case "$ref" in
    *"entry: fds: 3 4 5"*"exec: fds: 3 4 5"*)
        want=$(printf '%s\n' "$ref" |
            sed -e 's/fds: .*/fds: none/' -e 's/^keyctl: .*/keyctl: ENOSYS/')
        check "...and its rseq and PAC answers are the kernel's" "$want" "$out" ;;
    *) skip "entry state against the kernel: the reference run did not complete" ;;
    esac
    out=$( (trap '' USR2; m8run --keep-fds "$ESD" /entrystate 3</dev/null \
        4<"$ESO" 5<"$ESD/in") 2>/dev/null)
    check_contains "--keep-fds: kept, less a directory outside the view" \
        "entry: fds: 3 5" "$out"
    check_contains "--keep-fds: ...and across an exec" "exec: fds: 3 5" "$out"
    # The keyring of the host, where the host has one to answer with. The
    # emulator under a cross host is a kernel of its own and refuses it
    # too unless told.
    if [ -n "$CHROOTNG_EMU" ]; then
        hk() { "$TIMEOUT" -s KILL 60 "$CHROOTNG_EMU" chroot --jit --keep-fds \
            --host-keyring --shared-proc -w "$PWD" / "$@"; }
    else
        hk() { "$TIMEOUT" -s KILL 60 "$@"; }
    fi
    kref_k=$(hk "$ESD/entrystate" 2>/dev/null | grep '^keyctl:')
    # shellcheck disable=SC2086  # $CHROOTNG_GUEST_BINDS is a deliberately split list
    out_k=$(hk "$CHROOTNG_BIN" --host-keyring $CHROOTNG_GUEST_BINDS "$ESD" \
        /entrystate 2>/dev/null | grep '^keyctl:')
    case "$kref_k" in
    "keyctl: answered")
        check "--host-keyring: the host's keyring answers" "$kref_k" "$out_k" ;;
    *) skip "--host-keyring: the host has no keyring to answer (${kref_k:-no run})" ;;
    esac
fi
rm -rf "$ESD" "$ESO"

# The FP/SIMD registers across a call, which the monitor's code now spends
# (an ordinary build since 2.4): kept by the signal frame for a call answered
# in the handler, by the gate's save area for one dispatched from guest
# context (a connect of a blocking AF_UNIX socket), and handed to a guest
# handler that cuts a blocked accept short, in its frame, where what it writes
# is what the program has after. And a trap made with no thread pointer and
# garbage in x18, which the handler's entry switches away from and back. The
# expected block is a kernel's (6.8, static and dynamic glibc alike).
FKD=$(mktemp -d)
mkdir -p "$FKD/fpkeep.d"
: >"$FKD/fpkeep.d/f"
if guest_xlate_ready "FP/SIMD leg" &&
    guest_cc_report "$FKD/fpkeep" tests/chroot-ng/guests/fpkeep.c; then
    want=$(cat <<'EOF'
a call with no thread pointer and x18 garbage: answered, tpidr_el0 still 0, x18 kept
a call answered in the handler: kept (r=fd)
a call dispatched from guest context: kept (r=0)
a blocked call cut short: r=EINTR, the handler found them in its frame
...and after it: kept
EOF
)
    # shellcheck disable=SC2086  # $CHROOTNG_GUEST_BINDS is a deliberately split list
    out=$(run_t 60 $CHROOTNG_GUEST_BINDS "$FKD" /fpkeep 2>/dev/null)
    if [ "$want" = "$out" ]; then
        pass=$((pass + 1))
        echo "  ok   the FP/SIMD registers, the thread pointer and x18 come back as the kernel gives them"
    else
        fail=$((fail + 1))
        echo "  FAIL the registers across a call diverge from the kernel's"
        printf '       kernel: %s\n       guest : %s\n' \
            "$(echo "$want" | tr '\n' '|')" "$(echo "$out" | tr '\n' '|')"
    fi
fi
rm -rf "$FKD"

# A thread made by a raw clone(2), the way each libc and runtime makes one,
# now that chroot-ng makes it a pthread of the host's (monitor/thread.c):
# Go's shape (no CLONE_SETTLS, no tid words) and glibc's, the tid words
# written and CHILD_CLEARTID's cleared with a wake, a private descriptor
# table without CLONE_FILES, the parent's mask, no alternate stack, the
# kernel's refusals, and a robust mutex whose owner thread exited holding it.
# Differential against the same program under the kernel, run in $CTD as the
# guest is in its root: both have the tmp the child changes into. A cwd of
# its own without CLONE_FS is not one: the monitor keeps one guest cwd a
# process.
CTD=$(mktemp -d)
mkdir -p "$CTD/tmp" "$CTD/dev"
if guest_xlate_ready "raw-clone threads leg" &&
    guest_cc_report "$CTD/clonethr" tests/chroot-ng/guests/clonethr.c -lpthread; then
    want=$( (cd "$CTD" && emu_t 60 "$CTD/clonethr") 2>/dev/null |
        grep -v '^without CLONE_FS')
    # shellcheck disable=SC2086  # $CHROOTNG_GUEST_BINDS is a deliberately split list
    out=$(m8run -b /dev/null:/dev/null "$CTD" /clonethr 2>/dev/null |
        grep -v '^without CLONE_FS')
    if [ -z "$want" ]; then
        skip "raw-clone threads: the reference run produced nothing"
    elif [ "$want" = "$out" ]; then
        pass=$((pass + 1))
        echo "  ok   threads made by a raw clone start as the kernel's do"
    else
        fail=$((fail + 1))
        echo "  FAIL threads made by a raw clone diverge from the kernel's"
        printf '       kernel: %s\n       guest : %s\n' \
            "$(echo "$want" | tr '\n' '|')" "$(echo "$out" | tr '\n' '|')"
    fi
    skip "a thread without CLONE_FS: its cwd is the process's, one guest cwd a process (path.c)"
fi
rm -rf "$CTD"

# SIGSYS sent from elsewhere -- a tracer's kick, a sibling's de_thread
# request, a kill(2) -- to a process that traps with an alternate signal stack
# of its own, registered without SS_AUTODISARM as bionic's and Go's are: one
# that came after a handler of ours had left that stack for its own and
# before it had disarmed it was put at the stack's top, over a guest handler's
# frame or over the trap's own (monitor/sigsys.c, scr_run). Bionic's crash
# handler died of it when crash_dump attached, one crash in twenty on a 4.19
# Pixel. The window is a few instructions; CHROOTNG_ALTSTACK_DELAY widens it
# to 50 us for the SIGSYS the guest is sent to reach it. Differential against
# the same program under the kernel.
ATD=$(mktemp -d)
if guest_xlate_ready "alternate-stack SIGSYS leg" &&
    guest_cc_report "$ATD/altsys" tests/chroot-ng/guests/altsys.c; then
    want=$( (cd "$ATD" && emu_t 120 "$ATD/altsys") 2>/dev/null)
    out=$(CHROOTNG_ALTSTACK_DELAY=50 m8run "$ATD" /altsys 2>/dev/null)
    if [ -z "$want" ]; then
        skip "alternate-stack SIGSYS: the reference run produced nothing"
    elif [ "$want" = "$out" ]; then
        pass=$((pass + 1))
        echo "  ok   SIGSYS from elsewhere leaves a guest's own alternate stack alone"
    else
        fail=$((fail + 1))
        echo "  FAIL SIGSYS from elsewhere reached a guest's own alternate stack"
        printf '       kernel: %s\n       guest : %s\n' \
            "$(echo "$want" | tr '\n' '|')" "$(echo "$out" | tr '\n' '|')"
    fi
fi
rm -rf "$ATD"

# Repeated execs from a threaded program: every thread an exec ends is given
# back -- its stacks, and the pthread's, alternate and scratch stacks chroot-ng
# kept for it (monitor/thread.c) -- so the memory the last generation can
# reach is the first's, as a kernel's exec has it (PROT_NONE left out: a
# bionic guest's linker leaves gaps of a random size). They used to be kept:
# a megabyte and more a generation.
ETD=$(mktemp -d)
if guest_xlate_ready "threaded exec leg" &&
    guest_cc_report "$ETD/execthr" tests/chroot-ng/guests/execthr.c -lpthread; then
    # shellcheck disable=SC2086  # $CHROOTNG_GUEST_BINDS is a deliberately split list
    out=$(run_t 120 $CHROOTNG_GUEST_BINDS "$ETD" /execthr 2>&1)
    check_contains "execs from a threaded program give its threads back" \
        "the address space did not grow" "$out"
fi
rm -rf "$ETD"

# ...and keep what the host's libc made for a thread of ours, which the exec
# lands on top of while it is still in use (monitor/thread.c): bionic's
# thread records on our stacks, its TLS, shadow call stack and alternate
# stack. A thread that is not the main one execs; a fork child of a threaded
# program execs, bionic's list of threads still naming the others; and one
# made by a thread that is not the main one, which goes on as the process on
# what its libc made for it. Then the new program makes threads -- the
# libc's pthread_create in the monitor, on that TLS, linking into that list.
FTD=$(mktemp -d)
if guest_xlate_ready "threads after an exec leg" &&
    guest_cc_report "$FTD/forkthr" tests/chroot-ng/guests/forkthr.c -lpthread; then
    # shellcheck disable=SC2086  # $CHROOTNG_GUEST_BINDS is a deliberately split list
    out=$(run_t 120 $CHROOTNG_GUEST_BINDS "$FTD" /forkthr 2>&1)
    check_contains "threads after an exec from a thread that is not the main one" \
        "exec from a thread: the new program's threads ran" "$out"
    check_contains "threads after the exec of a threaded program's fork child" \
        "a fork child's exec: the new program's threads ran" "$out"
    check_contains "...and of one a thread that is not the main one made" \
        "a fork child of a thread's exec: the new program's threads ran" "$out"
fi
rm -rf "$FTD"
