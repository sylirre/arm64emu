# M6 execve-emulation tests (sourced by tests/chroot-ng/run.sh): what an execve
# a guest makes leaves behind, and what the program it starts finds.
echo "== M6: execve emulation =="

# --- M17-10: a failed exec must leave the calling program running -----------
# A binary whose PT_INTERP names a file that is not there is how an exec fails
# in a partially populated rootfs — a tree without the loader its binaries were
# linked against, a musl program under glibc — and the kernel's answer is ENOENT
# to a program that is still running: it opens and validates the ELF interpreter
# before it touches the old address space.
#
# Ours mapped the program first and looked for the interpreter afterwards. An
# ET_EXEC image goes down MAP_FIXED at its link-time vaddr — which for two
# binaries out of one toolchain is the caller's own text — so the errno came back
# to a program whose code had just been replaced. Measured: rc 139 and no output
# at all, against the kernel's exec=ENOENT / alive=1 / rc 44.
#
# Both halves of the pair have to be non-PIE for anything to be overwritten, and
# whether a toolchain links that way is not something to assume: the ELF shapes
# are checked, and a toolchain that cannot produce them skips rather than passing
# vacuously. qemu-user is no oracle for this one — it emulates execve by
# re-executing itself, so a missing interpreter is its own loader's complaint and
# not the kernel's errno — hence a host-native build for the reference side.
IFD=$(mktemp -d)
IF_NOINTERP=/cng-no-such-interp.so

# The oracle: on a host that runs AArch64 directly the guest pair is its own
# reference; otherwise the same two sources built host-native and linked alike.
if_oracle_build() {
    if [ -z "$CHROOTNG_EMU" ]; then
        IF_ORACLE=$IFD/interpfail
        IF_TARGET=$IFD/badinterp
        return 0
    fi
    for _c in ${HOSTCC:-} cc gcc clang; do
        have "$_c" || continue
        "$_c" -O2 -no-pie -o "$IFD/if_host" tests/chroot-ng/guests/interpfail.c \
            2>/dev/null || continue
        "$_c" -O2 -no-pie "-Wl,--dynamic-linker=$IF_NOINTERP" \
            -o "$IFD/bad_host" tests/chroot-ng/guests/hello.c 2>/dev/null || continue
        IF_ORACLE=$IFD/if_host
        IF_TARGET=$IFD/bad_host
        return 0
    done
    return 1
}
if_host_run() { if [ -n "$TIMEOUT" ]; then "$TIMEOUT" 60 env -u LD_PRELOAD "$@"; else kref "$@"; fi; }

if [ -z "$CHROOTNG_GUESTCC" ]; then
    skip "failed-exec survival: no AArch64 guest toolchain"
elif ! guest_cc "$IFD/interpfail" tests/chroot-ng/guests/interpfail.c -static -no-pie ||
    ! guest_cc "$IFD/badinterp" tests/chroot-ng/guests/hello.c -no-pie \
        "-Wl,--dynamic-linker=$IF_NOINTERP"; then
    skip "failed-exec survival: no non-PIE pair builds here ($(head -1 "$GUEST_CC_LOG" 2>/dev/null))"
elif [ "$(elf_type "$IFD/interpfail")" != EXEC ] ||
    [ "$(elf_type "$IFD/badinterp")" != EXEC ] ||
    ! elf_has_interp "$IFD/badinterp"; then
    skip "failed-exec survival: this toolchain links no ET_EXEC pair carrying a PT_INTERP, so nothing would be overwritten"
elif ! if_oracle_build; then
    skip "failed-exec survival: no host compiler for the differential oracle"
else
    if_out_k=$(if_host_run "$IF_ORACLE" "$IF_TARGET" 2>/dev/null); if_rc_k=$?
    if_out_g=$(run_t 90 "$IFD" /interpfail /badinterp 2>/dev/null); if_rc_g=$?
    if [ "$if_out_k" = "$if_out_g" ] && [ "$if_rc_k" = "$if_rc_g" ]; then
        pass=$((pass + 1))
        printf '  ok   a missing ELF interpreter is an errno and a live caller, as the kernel has it\n'
    else
        fail=$((fail + 1))
        printf '  FAIL a missing ELF interpreter does not match the kernel\n'
        printf '       kernel: %s (rc %s)\n' "$(echo "$if_out_k" | tr '\n' '|')" \
            "$if_rc_k"
        printf '       cng   : %s (rc %s)\n' "$(echo "$if_out_g" | tr '\n' '|')" \
            "$if_rc_g"
    fi

    # The other shape of the same failure: an interpreter that is there but is
    # not a loadable ELF, which fails in the header pass rather than in the
    # resolver. The caller has to be alive to be told, which is what this
    # asserts; which errno it is told is the table below.
    printf 'not an ELF\n' >"$IFD$IF_NOINTERP"
    chmod 755 "$IFD$IF_NOINTERP"
    check_contains "an unloadable interpreter also leaves the caller running" \
        "alive=1" "$(run_t 90 "$IFD" /interpfail /badinterp 2>&1)"
fi
rm -rf "$IFD"

# ...and WHICH errno. There are five, not the two we used to answer, and the two
# roles do not answer alike: a header too short to be one is EIO for an
# interpreter and ENOEXEC for a program, a header that does not check out is
# ELIBBAD or ENOEXEC, and an image that is a directory or carries no execute bit
# is EACCES for both — before its contents are looked at at all. It was
# invisible while an interpreter failure killed the caller; now the caller
# survives to read it.
#
# Differential, not a table of numbers: the same nine victims built for the host
# and run with no chroot-ng in the way. qemu-user is no oracle for an exec (it
# re-execs itself, so a broken interpreter is its own loader's complaint), so
# the reference side is always the host-native build — including its PT_INTERP
# strings, which have to be absolute host paths where the guest's are rootfs
# ones pointing at the same files.
EED=$(mktemp -d)
mkdir -p "$EED/brk" "$EED/host" "$EED/p_dir" "$EED/host/p_dir" "$EED/brk/dir"
: >"$EED/brk/short.tmp"
head -c 20 /bin/sh >"$EED/brk/short" 2>/dev/null
head -c 4096 /dev/zero | tr '\0' 'x' >"$EED/brk/junk"
cp "$EED/brk/junk" "$EED/p_junk"
cp "$EED/brk/junk" "$EED/host/p_junk"
chmod 755 "$EED/brk/short" "$EED/brk/junk" "$EED/p_junk" "$EED/host/p_junk"
rm -f "$EED/brk/short.tmp"

# One victim per way of being broken; the interpreter ones differ only in the
# PT_INTERP they carry. EE_CASES is the argument list both sides are given.
EE_INTERP="missing dir noexec short junk"
EE_PROG="p_noexec p_junk p_dir p_missing"

ee_build() { # ee_build CC OUTDIR INTERP_PREFIX
    _cc=$1; _dir=$2; _pfx=$3
    $_cc -O2 -static -o "$_dir/execerrno" tests/chroot-ng/guests/execerrno.c 2>/dev/null ||
        return 1
    for _n in $EE_INTERP; do
        $_cc -O2 "-Wl,--dynamic-linker=$_pfx/brk/$_n" -o "$_dir/v_$_n" \
            tests/chroot-ng/guests/hello.c 2>/dev/null || return 1
    done
    $_cc -O2 -static -o "$_dir/p_noexec" tests/chroot-ng/guests/hello.c 2>/dev/null ||
        return 1
    chmod 644 "$_dir/p_noexec"
    return 0
}

ee_args() { # the paths to hand the runner, rooted at $1
    for _n in $EE_INTERP; do printf '%s/v_%s ' "$1" "$_n"; done
    for _n in $EE_PROG; do printf '%s/%s ' "$1" "$_n"; done
}

ee_hostcc=""
for _c in ${HOSTCC:-} cc gcc clang; do have "$_c" && { ee_hostcc=$_c; break; }; done

if [ -z "$CHROOTNG_GUESTCC" ]; then
    skip "failed-exec errno: no AArch64 guest toolchain"
elif [ -z "$ee_hostcc" ]; then
    skip "failed-exec errno: no host compiler for the differential oracle"
# shellcheck disable=SC2086  # $CHROOTNG_GUESTLD is a flag or deliberately empty
elif ! ee_build "$CHROOTNG_GUESTCC" "$EED" "" ; then
    skip "failed-exec errno: the guest toolchain links no dynamic victim here"
elif ! ee_build "$ee_hostcc" "$EED/host" "$EED"; then
    skip "failed-exec errno: the host toolchain links no dynamic victim here"
else
    # The interpreter each victim names, made broken in its own way. `missing`
    # is the one file that must NOT exist.
    cp "$EED/host/execerrno" "$EED/brk/noexec" 2>/dev/null
    chmod 644 "$EED/brk/noexec"
    # shellcheck disable=SC2046  # ee_args is a deliberately split argument list
    ee_k=$(if_host_run "$EED/host/execerrno" $(ee_args "$EED/host") 2>/dev/null)
    # shellcheck disable=SC2046
    ee_g=$(run_t 90 "$EED" /execerrno $(ee_args "") 2>/dev/null)
    if [ -n "$ee_k" ] && [ "$ee_k" = "$ee_g" ]; then
        pass=$((pass + 1))
        printf '  ok   a failed exec answers the errno the kernel does, all five\n'
    else
        fail=$((fail + 1))
        printf '  FAIL a failed exec answers an errno the kernel does not\n'
        printf '       kernel: %s\n' "$(echo "$ee_k" | tr '\n' '|')"
        printf '       cng   : %s\n' "$(echo "$ee_g" | tr '\n' '|')"
    fi
fi
rm -rf "$EED"


# --- how long a single argv string may be -----------------------------------
# fs/exec.c bounds each argv/envp string at MAX_ARG_STRLEN, which is 32 *pages*:
# 128 KiB where a page is 4 KiB and 512 KiB on the 16 KiB kernels the Android
# devices run. The emulation walks argv itself — the kernel never sees it — so
# the bound is ours to get right, and a constant 32 * 4096 refused strings a
# real execve on such a device takes, with an -E2BIG of our own invention.
#
# Referenced against the same binary with no chroot-ng under it, so the number
# is the running kernel's rather than one written down here: on an AArch64 host
# or device that is the kernel directly, elsewhere arm64emu's, which answers as
# a kernel with 4 KiB pages does.
AMD=$(mktemp -d)
if ! guest_xlate_ready "argv string bound"; then
    :
elif ! guest_cc_report "$AMD/argmax" tests/chroot-ng/guests/argmax.c; then
    :
else
    am_k=$(emu_t 90 "$AMD/argmax" "$AMD/argmax" 2>/dev/null)
    am_g=$(run_t 90 / "$AMD/argmax" "$AMD/argmax" 2>/dev/null)
    if [ -n "$am_k" ] && [ "$am_k" = "$am_g" ]; then
        pass=$((pass + 1))
        printf '  ok   a single argv string is bounded where the kernel bounds it (%s)\n' \
            "$(echo "$am_k" | tr '\n' '|')"
    else
        fail=$((fail + 1))
        printf '  FAIL the argv string bound is not the one the kernel keeps\n'
        printf '       kernel: %s\n' "$(echo "$am_k" | tr '\n' '|')"
        printf '       cng   : %s\n' "$(echo "$am_g" | tr '\n' '|')"
    fi
fi
rm -rf "$AMD"

# --- what an exec chain costs the address space -----------------------------
# A real execve throws the whole mm away. The emulated one cannot — the monitor
# lives in that address space — so it gives back what its own loader mapped for
# the program being replaced (the image, the interpreter's image, the stack
# built for it), and then sweeps whatever else in the address space was never
# the monitor's. Left behind, those accumulated: 66.8 MB of address space per
# generation, measured, 64 MiB of it the stack.
#
# Compared against the same chain with no emulation under it rather than against
# a constant, since VmSize is whatever the host underneath happens to need. The
# slack covers what a generation genuinely does keep — the emulator's own
# per-image bookkeeping — while leaving no room for a whole leaked generation.
#
# This one is the guest the harness builds, whatever link mode that is; the leg
# below it builds a dynamic one deliberately, because a program's own library
# mappings are the half no recorded extent describes.
ECD=$(mktemp -d)
if ! guest_xlate_ready "exec-chain address space"; then
    :
elif ! guest_cc_report "$ECD/execchain" tests/chroot-ng/guests/execchain.c; then
    :
elif elf_has_interp "$ECD/execchain"; then
    skip "exec-chain address space: the dynamic form of this leg is below, with the binds an ld.so needs"
else
    # CHROOTNG_EXEC_RECLAIM_FORCE: qemu-user runs a thread of its own (call_rcu)
    # beside the guest's, so the process never reads as single-threaded from
    # inside it and the reclaim would sit out. On a host that runs AArch64
    # directly the flag changes nothing — there really is one thread.
    ec_k=$(emu_t 90 "$ECD/execchain" 8 2>/dev/null)
    ec_g=$(CHROOTNG_EXEC_RECLAIM_FORCE=1 run_t 90 "$ECD" /execchain 8 2>/dev/null)
    case "$ec_k$ec_g" in
    *growth_kb=*growth_kb=*)
        ec_kn=${ec_k##*growth_kb=}
        ec_gn=${ec_g##*growth_kb=}
        if [ "$ec_gn" -le $((ec_kn + 8192)) ]; then
            pass=$((pass + 1))
            printf '  ok   an exec chain gives the address space back (%s kB over 8, kernel %s kB)\n' \
                "$ec_gn" "$ec_kn"
        else
            fail=$((fail + 1))
            printf '  FAIL an exec chain leaks address space: %s kB over 8 execs, kernel %s kB\n' \
                "$ec_gn" "$ec_kn"
        fi
        ;;
    *)
        skip "exec-chain address space: no reference run ($(echo "$ec_k$ec_g" | tr '\n' '|'))"
        ;;
    esac
fi
rm -rf "$ECD"

# --- execve from a multithreaded process --------------------------------------
# A real execve kills every other thread (de_thread) and, when the caller is
# not the group leader, gives it the leader's identity, so what comes out of
# the exec is one thread whose tid is its pid. The emulation used to do
# neither: the old threads went on running the old program beside the new one
# (a Go program's runtime threads hit EBADF on their CLOEXEC-closed netpoll fd
# and aborted the NEW program), and a non-leader exec'er kept its own tid.
# Now every sibling is told to die through the one signal a guest can never
# block, and a non-leader hands its exec to the leader to carry. The helpers
# are parked in every shape there is, including the waits that install a full
# signal mask (ppoll, pselect, epoll_pwait, sigsuspend, sigwait): those are
# trapped so SIGSYS can be taken out of the mask, or the exec would wait for
# them forever — so completing in time is part of the answer. Differential
# against the kernel, or the emulator the guests run under, on both sides.
TED=$(mktemp -d)
if ! guest_xlate_ready "multithreaded execve"; then
    :
elif ! guest_cc "$TED/threadexec" tests/chroot-ng/guests/threadexec.c -pthread; then
    skip "multithreaded execve: could not build tests/chroot-ng/guests/threadexec.c with -pthread"
else
    mkdir -p "$TED/bin"; cp "$TED/threadexec" "$TED/bin/threadexec"
    for mode in leader nonleader; do
        te_k=$(cd "$TED/bin" && emu_t 60 ./threadexec $mode 2>/dev/null)
        # shellcheck disable=SC2086  # $CHROOTNG_GUEST_BINDS is a deliberately split arg list
        te_g=$(run_t 90 $CHROOTNG_GUEST_BINDS "$TED" /bin/threadexec $mode 2>/dev/null)
        if [ -n "$te_k" ] && [ "$te_k" = "$te_g" ]; then
            pass=$((pass + 1))
            printf '  ok   execve by the %s thread matches the kernel (%s)\n' "$mode" "$te_g"
        else
            fail=$((fail + 1))
            printf '  FAIL execve by the %s thread diverges from the kernel\n' "$mode"
            printf '       kernel: %s\n       guest : %s\n' "$te_k" "$te_g"
        fi
    done
    # ...and the waits themselves still behave as the kernel's: the mask the
    # call installs is in force for the wait, the caller's own mask is back
    # afterwards, the result and errno are the kernel's, a full mask still
    # works, and a set the kernel would refuse is refused as it would. The
    # handler cannot run the wait itself and bounces it through a second svc
    # in the gate. Whether the filter lets that svc through depends on where
    # the gate ends relative to the instruction after it, which the kernel
    # reports as the call's ip: with the gate one instruction short, every
    # masked wait (musl's select() is one — it passes a {NULL, size} pair)
    # trapped again from the stub, each round a record lower on the guest's
    # stack, until it was gone (a SEGV on Android, a hang under a VM's TCG).
    if guest_cc "$TED/maskwait" tests/chroot-ng/guests/maskwait.c -pthread; then
        cp "$TED/maskwait" "$TED/bin/maskwait"
        mw_k=$(emu_t 60 "$TED/maskwait" 2>/dev/null)
        # shellcheck disable=SC2086  # $CHROOTNG_GUEST_BINDS is a deliberately split arg list
        mw_g=$(run_t 90 $CHROOTNG_GUEST_BINDS "$TED" /bin/maskwait 2>/dev/null)
        if [ -n "$mw_k" ] && [ "$mw_k" = "$mw_g" ]; then
            pass=$((pass + 1))
            echo "  ok   the mask-taking waits answer as the kernel's, SIGSYS taken out"
        else
            fail=$((fail + 1))
            echo "  FAIL the mask-taking waits diverge from the kernel"
            printf '       kernel: %s\n       guest : %s\n' \
                "$(echo "$mw_k" | tr '\n' '|')" "$(echo "$mw_g" | tr '\n' '|')"
        fi
    else
        skip "masked waits: could not build tests/chroot-ng/guests/maskwait.c with -pthread"
    fi
    # The bounce is not for masked waits alone: every trapped call that can
    # block is made from guest context, or a signal cannot reach it. Made in
    # the SIGSYS handler, with every signal masked, an accept, a receive, or a
    # FIFO's open ignored the guest's own handler until it returned, never
    # failed with EINTR, and outlived SIGTERM. Against the kernel: the handler
    # runs while the call blocks, SA_RESTART makes the call again and it goes on
    # waiting, its absence is EINTR, and SIGTERM kills. The AF_UNIX receive with
    # an address to map back is the one that runs the whole dispatch there. On
    # the SIGSYS tier the handler must also run on the thread's own stack, as
    # the kernel delivers it, which takes the dispatch's blocking svc being made
    # there (`sp`).
    if guest_cc "$TED/blkcall" tests/chroot-ng/guests/blkcall.c -pthread; then
        cp "$TED/blkcall" "$TED/bin/blkcall"
        mkdir -p "$TED/tmp"
        bc_k=$(emu_t 90 "$TED/blkcall" "$TED/tmp" sp 2>/dev/null)
        # shellcheck disable=SC2086  # $CHROOTNG_GUEST_BINDS is a deliberately split arg list
        bc_g=$(run_t 120 $CHROOTNG_GUEST_BINDS "$TED" /bin/blkcall /tmp sp 2>/dev/null)
        if [ -n "$bc_k" ] && [ "$bc_k" = "$bc_g" ]; then
            pass=$((pass + 1))
            echo "  ok   a signal against a blocking call acts as the kernel's"
        else
            fail=$((fail + 1))
            echo "  FAIL a signal against a blocking call diverges from the kernel"
            printf '       kernel: %s\n       guest : %s\n' \
                "$(echo "$bc_k" | tr '\n' '|')" "$(echo "$bc_g" | tr '\n' '|')"
        fi
    else
        skip "blocking calls: could not build tests/chroot-ng/guests/blkcall.c with -pthread"
    fi
fi
rm -rf "$TED"

# Every way to name what an exec runs, and what it takes with it: #! scripts and
# their chains, the fd links and execveat's forms, an unreadable program run by
# descriptor, the descriptors, timers and signal state an exec keeps or drops.
# The block is a kernel's, every line of it measured; how deep a #! chain may go
# is not one number across kernels, so only four (runs) and eight (ELOOP) are
# asked.
XVD=$(mktemp -d)
if guest_xlate_ready "exec variants" &&
    guest_cc_report "$XVD/execvar" tests/chroot-ng/guests/execvar.c; then
    xv_want='plain: argv w show BB exe=/execvar
symlink: argv w show BB exe=/execvar
shebang: argv /execvar sheb /xv/s1 AA exe=/execvar
shebang 4 deep: argv /execvar sheb /xv/s1 /xv/s2 /xv/s3 /xv/s4 AA exe=/execvar
shebang 8 deep: errno 40
a script that is not executable: errno 13
empty file: errno 8
missing: errno 2
a directory: errno 13
relative: argv w show BB exe=/execvar
fd path, program: argv w show BB exe=/execvar
fd path, script: argv /execvar sheb /proc/self/fd/77 AA exe=/execvar
fd path, no descriptor: errno 2
O_PATH fd of an unreadable program: argv w show BB exe=/xv/unreadable
execveat relative: argv w show BB exe=/execvar
execveat above the root: argv w show BB exe=/execvar
execveat AT_EMPTY_PATH: argv w show BB exe=/execvar
execveat nofollow, a link: errno 40
execveat nofollow, a file: argv w show BB exe=/execvar
execveat unknown flag: errno 22
execveat empty name: errno 2
descriptors: argv w show BB exe=/execvar kept=1 closed=1
timers: argv w show BB exe=/execvar timers_gone=1
signals: argv w show BB exe=/execvar blocked=1 ignored=1 handler_reset=1'
    # shellcheck disable=SC2086  # $CHROOTNG_GUEST_BINDS is a deliberately split arg list
    xv_got=$(run_t 120 $CHROOTNG_GUEST_BINDS "$XVD" /execvar 2>/dev/null)
    if [ "$xv_got" = "$xv_want" ]; then
        pass=$((pass + 1))
        echo "  ok   every way to name an exec answers as the kernel's"
    else
        fail=$((fail + 1))
        echo "  FAIL every way to name an exec answers as the kernel's"
        printf '%s\n' "$xv_want" >"$CHROOTNG_TMP/xv.want"
        printf '%s\n' "$xv_got" >"$CHROOTNG_TMP/xv.got"
        diff "$CHROOTNG_TMP/xv.want" "$CHROOTNG_TMP/xv.got" | sed 's/^/    /'
    fi
fi
rm -rf "$XVD"

# ...and from a process with more siblings than the de_thread's table held,
# and from one whose siblings keep making threads. The siblings used to be
# listed once into a table of 4096 and those told: the 4097th and everything
# after it went on running the old program beside the new one, without a
# word — and so did a thread a sibling cloned between being listed and taking
# its request, which nothing ever told. The list is now the kernel's own,
# read afresh until its count of threads says nothing but the exec'er (and
# what cannot take a signal at all: under qemu-user, the emulator's own
# thread) remains. Differential against the kernel; the chain shape keeps 16
# chains cloning, where one missed newcomer keeps its chain going for good.
SED=$(mktemp -d)
if ! guest_xlate_ready "de_thread beyond a table"; then
    :
elif ! guest_cc "$SED/spawnexec" tests/chroot-ng/guests/spawnexec.c -pthread; then
    skip "de_thread beyond a table: could not build tests/chroot-ng/guests/spawnexec.c with -pthread"
else
    mkdir -p "$SED/bin"; cp "$SED/spawnexec" "$SED/bin/spawnexec"
    for shape in "many 4500" "chain 16"; do
        # shellcheck disable=SC2086  # the shape is two words
        se_k=$(cd "$SED/bin" && emu_t 180 ./spawnexec $shape 2>/dev/null | grep '^report:')
        # shellcheck disable=SC2086
        se_g=$(run_t 240 $CHROOTNG_GUEST_BINDS "$SED" /bin/spawnexec $shape 2>/dev/null | grep '^report:')
        if [ -n "$se_k" ] && [ "$se_k" = "$se_g" ]; then
            pass=$((pass + 1))
            printf '  ok   execve reaches every sibling (%s) as the kernel does (%s)\n' "$shape" "$se_g"
        else
            fail=$((fail + 1))
            printf '  FAIL execve leaves siblings behind (%s)\n' "$shape"
            printf '       kernel: %s\n       guest : %s\n' "$se_k" "$se_g"
        fi
    done
    # ...and with each request held at its delivery for a few milliseconds,
    # as the scheduler holds a thread on a loaded device. A request that has
    # been delivered is no longer pending, so another one is delivered on top
    # of it: told every round, a held thread collected a signal frame a round
    # until there was no room for the next, and the kernel killed the whole
    # process with a SIGSEGV (a few chain runs in a hundred on Android 8,
    # where bionic's alternate stack is 16 KiB). The tracer that holds them is
    # a native program (dtstall.c): under an emulator a guest's tracer sees the
    # emulated ptrace's stops, not the kernel's delivery, so a host that
    # emulates AArch64 has no such run.
    if [ -n "$CHROOTNG_EMU" ]; then
        skip "de_thread with each request held at delivery: the tracer that holds them needs the kernel's own signal delivery, not an emulator's"
    elif ! guest_cc "$SED/dtstall" tests/chroot-ng/guests/dtstall.c; then
        skip "de_thread with each request held at delivery: could not build tests/chroot-ng/guests/dtstall.c"
    else
        se_k=$(cd "$SED/bin" && emu_t 180 ./spawnexec chain 16 2>/dev/null | grep '^report:')
        # shellcheck disable=SC2086  # $CHROOTNG_GUEST_BINDS is a deliberately split arg list
        se_g=$(emu_t 240 "$SED/dtstall" 2 "$CHROOTNG_BIN" $CHROOTNG_GUEST_BINDS "$SED" /bin/spawnexec chain 16 2>/dev/null)
        se_rc=$?
        case "$se_g" in
        SKIP:*)
            skip "de_thread with each request held at delivery: ${se_g#SKIP: }"
            ;;
        *)
            se_g=$(printf '%s\n' "$se_g" | grep '^report:')
            if [ -n "$se_k" ] && [ "$se_k" = "$se_g" ]; then
                pass=$((pass + 1))
                printf '  ok   execve reaches every sibling with each request held at delivery (%s)\n' "$se_g"
            else
                fail=$((fail + 1))
                printf '  FAIL execve with each request held at delivery (rc %s)\n' "$se_rc"
                printf '       kernel: %s\n       guest : %s\n' "$se_k" "$se_g"
            fi
            ;;
        esac
    fi
fi
rm -rf "$SED"

# ...and the same chain by a program with far more mappings than one sweep
# pass holds. The sweep collects a table's worth of victims per walk of
# /proc/self/maps and walks again while a pass fills it; the passes were capped
# at eight of forty-eight, so the 385th reclaimable mapping and everything after
# it survived every exec — a JIT, a program that dlopens by the hundred, an
# allocator that reserves by the arena. This guest maps 1500 pages that cannot
# merge (3001 VMAs with the gaps between them) and execs itself six times;
# under the cap that leaked 68 MB, against a kernel run that gains nothing.
ECM=$(mktemp -d)
if ! guest_xlate_ready "many-mapping exec chain"; then
    :
elif ! guest_cc_report "$ECM/manymaps" tests/chroot-ng/guests/manymaps.c; then
    :
elif elf_has_interp "$ECM/manymaps"; then
    skip "many-mapping exec chain: needs a static guest (the dynamic form's binds are covered below)"
else
    em_k=$(emu_t 90 "$ECM/manymaps" 1500 6 2>/dev/null)
    em_g=$(CHROOTNG_EXEC_RECLAIM_FORCE=1 run_t 90 "$ECM" /manymaps 1500 6 2>/dev/null)
    case "$em_k$em_g" in
    *growth_kb=*growth_kb=*)
        em_kn=${em_k##*growth_kb=}
        em_gn=${em_g##*growth_kb=}
        if [ "$em_gn" -le $((em_kn + 8192)) ]; then
            pass=$((pass + 1))
            printf '  ok   an exec chain gives back more mappings than one pass holds (%s kB over 6, kernel %s kB)\n' \
                "$em_gn" "$em_kn"
        else
            fail=$((fail + 1))
            printf '  FAIL an exec chain leaks past the sweep ceiling: %s kB over 6 execs, kernel %s kB\n' \
                "$em_gn" "$em_kn"
        fi
        ;;
    *)
        skip "many-mapping exec chain: no reference run ($(echo "$em_k$em_g" | tr '\n' '|'))"
        ;;
    esac
fi
rm -rf "$ECM"

# ...and the same chain with a guest whose OWN ld.so maps its libraries. That
# memory is not the loader's and no recorded extent describes it, so it is the
# half the sweep exists for — and the half that used to hurt. Measured on an
# Android 13 device before it: 10,790,480 kB per generation, most of it one
# PROT_NONE reservation bionic's scudo makes at libc init and the next
# generation makes again. A musl guest cost 20 kB a generation and hid the whole
# thing, which is why this leg names a dynamic guest rather than trusting
# whichever one the harness happens to link.
#
# No kernel differential here, unlike the leg above: a dynamic bionic guest's
# VmSize swings ±80 MB between runs of the SAME binary (scudo sizes its regions
# from what it finds), which is wider than that leg's whole slack and would make
# the comparison a coin toss. One leaked generation is three orders of magnitude
# above that swing, so the chain is simply run long enough that a leak cannot
# survive it: sixty-four generations at the old cost is 690 GB against a 512 GB
# address space, and the run before the sweep died exactly there — "Fatal signal
# 6 (SIGABRT)" out of scudo, after 21 seconds of mapping. It now finishes in
# 0.2 s. Completing at all is most of this assertion; the bound is the rest.
ECDD=$(mktemp -d)
cng_dyn_binds
if [ -z "$CHROOTNG_GUESTCC" ]; then
    skip "dynamic exec-chain address space: no AArch64 guest toolchain"
elif ! $CHROOTNG_GUESTCC -O2 -o "$ECDD/execchain" tests/chroot-ng/guests/execchain.c \
    2>"$GUEST_CC_LOG"; then
    fail=$((fail + 1)); printf '  FAIL could not build a dynamic execchain\n'
    sed 's/^/    /' "$GUEST_CC_LOG"
elif ! elf_has_interp "$ECDD/execchain"; then
    skip "dynamic exec-chain address space: this toolchain links even -O2 guests statically"
else
    # shellcheck disable=SC2086  # both are deliberately split arg lists
    ecd_g=$(CHROOTNG_EXEC_RECLAIM_FORCE=1 run_t 200 $GUEST_DYN_BINDS \
        "$ECDD" /execchain 64 2>/dev/null)
    case "$ecd_g" in
    *growth_kb=*)
        ecd_gn=${ecd_g##*growth_kb=}
        # A generation of this guest costs gigabytes; the run-to-run swing is
        # tens of megabytes. Anything under a gigabyte over sixty-four execs is
        # therefore "no generation was kept", with room to spare on both sides.
        if [ "$ecd_gn" -lt 1048576 ]; then
            pass=$((pass + 1))
            printf '  ok   a dynamic chain gives its libraries back too (%s kB over 64)\n' \
                "$ecd_gn"
        else
            fail=$((fail + 1))
            printf '  FAIL a dynamic exec chain leaks address space: %s kB over 64 execs\n' \
                "$ecd_gn"
        fi
        ;;
    *)
        fail=$((fail + 1))
        printf '  FAIL a dynamic exec chain of 64 did not survive (%s)\n' \
            "$(echo "$ecd_g" | tr '\n' '|')"
        ;;
    esac
fi
rm -rf "$ECDD"
