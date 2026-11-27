# M25: descriptors and the guest view (sourced by tests/arm64emu/chroot/native/run.sh).
#
# A directory descriptor is a place to resolve names from, and the kernel
# resolves them with no rootfs in the way. The dispatcher passed a plain name
# against a dirfd to the kernel on the strength of "the dirfd already points
# inside the guest view", and for a dirfd it could not map back it passed
# every name — so `openat(dirfd("/proc"), "../etc/passwd")` read the host's,
# the components after an fd magic link were the kernel's to walk from wherever
# the descriptor pointed, and a directory fd the launcher leaked across the
# exec was an open door. Now a descriptor on a directory the guest has no name
# for never enters its table (closed at startup, on receipt over a socket, and
# after pidfd_getfd), the zones' directories are named so a walk from one is
# contained like any other, and what a name reaches through an fd link is
# walked by the resolver rather than by the kernel.
echo "== M25: descriptors and the guest view =="

# End to end: a guest with the launcher's leaks in its table (fd 7 a host
# directory, fd 8 a host file, both outside the rootfs), and every route a
# relative name used to take out of the view. Without --keep-fds no
# descriptor above 2 is inherited at all; with it, the directory still goes. The rootfs supplies the marker
# the escapes must land on instead, and a /dev so "/dev/pts/../.." has a
# parent to walk through.
FVR=$(mktemp -d); FVO=$(mktemp -d)
# "Outside" has to be true: where the guest binds a directory that contains the
# scratch space (Termux's $TMPDIR is inside the bound $PREFIX), a name there is
# one the guest HAS, and no descriptor on it is a leak.
FVO=$(cng_outside_binds "$FVO")
mkdir -p "$FVR/etc" "$FVR/sub" "$FVR/dev"
printf 'GUEST-MARKER' > "$FVR/etc/marker"
ln -s /etc/marker "$FVR/sub/lnk"
printf 'HOST-LEAK' > "$FVO/file"
if ! guest_xlate_ready "descriptor legs"; then
    :
elif guest_cc_report "$FVR/fdescape" tests/arm64emu/chroot/native/guests/fdescape.c; then
    # shellcheck disable=SC2086  # $ARM64EMU_NATIVE_GUEST_BINDS is a deliberately split arg list
    out=$(run_t 60 $ARM64EMU_NATIVE_GUEST_BINDS "$FVR" /fdescape 7<"$FVO" 8<"$FVO/file" 2>&1)
    check_contains "no leaked fd is inherited: the directory" \
        "inherited-dirfd=Bad file descriptor" "$out"
    check_contains "no leaked fd is inherited: the file" \
        "inherited-file=Bad file descriptor" "$out"
    # shellcheck disable=SC2086
    out=$(run_t 60 $ARM64EMU_NATIVE_GUEST_BINDS --keep-fds "$FVR" /fdescape 7<"$FVO" \
        8<"$FVO/file" 2>&1)
    check_contains "--keep-fds: a leaked directory fd is closed before the guest runs" \
        "inherited-dirfd=Bad file descriptor" "$out"
    check_contains "--keep-fds: a leaked file fd stays, for the I/O it carries" \
        "inherited-file=ok(HOST-LEAK)" "$out"
    # One of the standard three on an outside directory (`< /`) is replaced
    # by /dev/null rather than left closed: a program is entitled to find 0,
    # 1 and 2 open, and its next open must not land on one of them.
    # shellcheck disable=SC2086
    out0=$(run_t 60 $ARM64EMU_NATIVE_GUEST_BINDS "$FVR" /fdescape <"$FVO" 2>&1)
    check_contains "stdin on an outside directory becomes /dev/null" \
        "stdin=/dev/null" "$out0"
    for _leg in proc-dotdot procself-dotdot devpts-dotdot; do
        case "$out" in
        *"$_leg=unopenable"*)
            skip "$_leg: the host refuses to open that directory, so there is no zone dirfd to walk from" ;;
        *)
            check_contains "$_leg: a .. run from a zone dirfd stays in the rootfs" \
                "$_leg=ok(GUEST-MARKER)" "$out" ;;
        esac
    done
    check_contains "a .. run through a dirfd's magic link stays in the rootfs" \
        "magic-dotdot=ok(GUEST-MARKER)" "$out"
    check_contains "an absolute symlink below a dirfd's magic link is re-rooted" \
        "magic-symlink=ok(GUEST-MARKER)" "$out"
    check_contains "...through the /dev/fd spelling too" \
        "devfd-symlink=ok(GUEST-MARKER)" "$out"
    check_contains "a directory of the view still reopens through its fd link" \
        "magic-reopen-dir=ok" "$out"
    check_contains "fchdir into /proc carries the cwd with it" \
        "getcwd=/proc" "$out"
    check_contains "...and relative names resolve there" \
        "rel-self-status=ok" "$out"
    check_contains "a directory of the view survives a trip over a socket" \
        "scm-view-dir=usable" "$out"
    check_contains "so does a file" "scm-view-file=usable" "$out"
    # ...and what arrives from OUTSIDE the view, which only a host process can
    # send: a directory the guest has no name for is closed on arrival, its
    # number left in the record; a file and a directory of the view are let
    # in. Judged out of the control data as the kernel wrote it into a buffer
    # of the monitor's — the guest's own buffer, read back a syscall later,
    # held whatever another of its threads had put there — and the records
    # then handed to the guest, by recvmsg and by recvmmsg alike. Sent from a
    # Python of the host's (send_fds, 3.9+); without one the leg sits out.
    if guest_cc "$FVR/fdimport" tests/arm64emu/chroot/native/guests/fdimport.c &&
        python3 -c 'import socket; socket.send_fds' 2>/dev/null; then
        rm -f "$FVR/sock"
        # shellcheck disable=SC2086
        run_t 60 $ARM64EMU_NATIVE_GUEST_BINDS "$FVR" /fdimport /sock >"$FVO/import.out" \
            2>/dev/null &
        _bg=$!
        _n=0
        while [ ! -S "$FVR/sock" ] && [ $_n -lt 100 ]; do sleep 0.1; _n=$((_n + 1)); done
        python3 tests/arm64emu/chroot/native/send_fds.py "$FVR/sock" "$FVO" "$FVO/file" "$FVR" \
            2>/dev/null
        wait $_bg 2>/dev/null
        imp=$(cat "$FVO/import.out")
        check_contains "an outside directory arriving over a socket is closed on arrival" \
            "round=0 n=1 ctrunc=0 closed file dir" "$imp"
        check_contains "...by recvmmsg as well" \
            "round=1 n=1 ctrunc=0 closed file dir" "$imp"
        # A control buffer longer than the monitor's own bound: an AF_UNIX
        # socket's is taken at the bound (it can never fill it), so the records
        # arrive whole and judged; an AF_INET socket's stays the guest's own,
        # and the record the kernel wrote there arrives untouched.
        check_contains "...and through a control buffer past the bounce bound" \
            "round=2 n=1 ctrunc=0 closed file dir" "$imp"
        case "$imp" in
        *"inet: setup:"*) skip "no loopback for the AF_INET control leg" ;;
        *) check_contains "a family that carries no descriptors keeps its own buffer" \
            "inet: n=1 ctrunc=0 pktinfo=1" "$imp" ;;
        esac
    else
        skip "descriptor import leg: no guest build or no python3 send_fds here"
    fi
    # The hidden-process view by pid. /proc hides a host process by path, and
    # process_vm_readv/writev and pidfd_open were the routes to a process that
    # carry no path: with a ptrace policy that permits same-uid access a guest
    # could read and write the memory of a process its /proc said did not
    # exist, and a pidfd reaches everything a pidfd reaches. ESRCH now, which
    # is what /proc/<pid> answers -- where a guest process stays reachable.
    # Where the host has no process_vm_readv or pidfd_open to reach a guest
    # process with (arm64emu reaches the memory of the caller and of its
    # stopped tracees alone, EPERM for any other; an old kernel has no
    # pidfd_open) the allowed leg says so and sits out; the hidden one is
    # answered before the kernel is asked, and is ESRCH either way.
    check_contains "process_vm_readv on a hidden process is ESRCH" \
        "pvm-read-hidden=No such process" "$out"
    case "$out" in
    *"pvm-read-guest=Function not implemented"*)
        skip "process_vm_readv on a guest process: no process_vm_readv here" ;;
    *"pvm-read-guest=Operation not permitted"*)
        if [ -n "$ARM64EMU_NATIVE_EMU" ]; then
            skip "process_vm_readv on a guest process: arm64emu reaches no running process's memory but the caller's"
        else
            check_contains "process_vm_readv on a guest process is allowed" \
                "pvm-read-guest=ok" "$out"
        fi ;;
    *)
        check_contains "process_vm_readv on a guest process is allowed" \
            "pvm-read-guest=ok" "$out" ;;
    esac
    check_contains "pidfd_open on a hidden process is ESRCH" \
        "pidfd-open-hidden=No such process" "$out"
    case "$out" in
    *"pidfd-open-guest=Function not implemented"*)
        skip "pidfd_open on a guest process: no pidfd_open here" ;;
    *)
        check_contains "pidfd_open on a guest process is allowed" \
            "pidfd-open-guest=ok" "$out" ;;
    esac
fi
rm -rf "$FVR" "$FVO"

# The hidden-process view by pid, for signals (M85). kill, tkill, tgkill and
# the two sigqueue calls name a process by pid as well, and the group and
# broadcast forms of kill name every process at once: a guest's kill(0)
# reached the host shell it was started from, and kill(-1) every process of
# the user. Two host processes stand in for them (sigholder: one in the
# guest's process group, one leading a group of its own) and count the SIGURGs
# that reach them, while the guest prints what each call answered and what
# its own processes received. A --no-proc control, under which nothing is
# hidden, shows the holders do see a signal that reaches them. The guest is
# started untimed and bounds itself with alarm(), because timeout(1) would
# move it into a process group the holder is not in.
SVD=$(mktemp -d)
SV_WANT="kill-hidden-0=-1 errno=3
kill-hidden=-1 errno=3
tkill-hidden=-1 errno=3
tgkill-hidden=-1 errno=3
tgkill-hidden-tid0=-1 errno=22
sigqueue-hidden=-1 errno=3
tgsigqueue-hidden=-1 errno=3
sigqueue-hidden-forged=-1 errno=1
kill-parent=-1 errno=3
kill-hidden-group-0=-1 errno=3
kill-hidden-group=-1 errno=3
kill-intmin=-1 errno=3
kill-child=0
tgkill-self=0
tkill-thread=0
tgkill-thread=0
kill-group-own=0
kill-group-child=0
kill-all=0
kill-group-child-0=0
counts us=1 c1=2 c2=2 c3=2
done"

# Start both holders and wait until their handlers are in; SVA and SVB are
# their pids, SVJA and SVJB the shell's jobs for them.
sv_start() {
    rm -f "$SVD/a" "$SVD/b"
    emu "$SVD/sigholder" "$SVD/a" &
    SVJA=$!
    emu "$SVD/sigholder" "$SVD/b" own &
    SVJB=$!
    _n=0
    until grep -q '^ready' "$SVD/a" 2>/dev/null &&
        grep -q '^ready' "$SVD/b" 2>/dev/null; do
        [ $_n -ge 300 ] && break
        sleep 0.1
        _n=$((_n + 1))
    done
    SVA=$(sed -n 's/^ready pid=//p' "$SVD/a")
    SVB=$(sed -n 's/^ready pid=//p' "$SVD/b")
}
# Stop them; each leaves its count in its file.
sv_stop() {
    kill -TERM "$SVA" "$SVB" 2>/dev/null
    wait "$SVJA" "$SVJB" 2>/dev/null
}

if guest_xlate_ready "signals to hidden processes" &&
    guest_cc_report "$SVD/sigholder" tests/arm64emu/chroot/native/guests/sigholder.c &&
    guest_cc_report "$SVD/sigview" tests/arm64emu/chroot/native/guests/sigview.c -pthread; then
    sv_start
    if [ -z "$SVA" ] || [ -z "$SVB" ]; then
        check "signals to hidden processes: the holders start" \
            started "a=$SVA b=$SVB"
        sv_stop
    else
        # shellcheck disable=SC2086  # $ARM64EMU_NATIVE_GUEST_BINDS is a deliberately split arg list
        _got=$(run $ARM64EMU_NATIVE_GUEST_BINDS "$SVD" /sigview "$SVA" "$SVB" 2>/dev/null)
        sv_stop
        _label="a signal reaches no process the view hides, alone or by group"
        if [ "$(printf '%s\n' "$_got" | sed 1d)" = "$SV_WANT" ]; then
            pass=$((pass + 1))
            echo "  ok   $_label"
        else
            fail=$((fail + 1))
            echo "  FAIL $_label"
            printf '%s\n' "$_got" | sed 's/^/    /' | head -30
        fi
        check "the hidden process in the guest's group got nothing" \
            "urg=0" "$(grep '^urg=' "$SVD/a")"
        check "the hidden group got nothing" \
            "urg=0" "$(grep '^urg=' "$SVD/b")"
        case "$_got" in
        "holder-shares-group=1"*) ;;
        *) skip "kill(0) past a hidden member: the shell put the guest in a group of its own" ;;
        esac
    fi
    # The control: with --no-proc nothing is hidden, and the same calls reach
    # both holders -- so the zeros above are the view's doing. kill(-1) is
    # left out of it, since there it would reach every process of the user.
    sv_start
    # shellcheck disable=SC2086
    _got=$(run --no-proc $ARM64EMU_NATIVE_GUEST_BINDS "$SVD" /sigview "$SVA" "$SVB" control 2>/dev/null)
    sv_stop
    check_contains "--no-proc: a host process is signalled as on the host" \
        "kill-hidden=0" "$_got"
    for _h in a b; do
        _u=$(sed -n 's/^urg=//p' "$SVD/$_h")
        [ -n "$_u" ] && [ "$_u" != 0 ] && _u=some
        check "--no-proc: holder $_h sees the signals that reach it" some "$_u"
    done
    # Without a registry (ARM64EMU_PROCTAB_FORCE_NONE: a host that could map none) the
    # view cannot tell a guest from a host process, and the signal family runs
    # native rather than refuse every pid but the caller's.
    sv_start
    # shellcheck disable=SC2086
    _got=$(ARM64EMU_PROCTAB_FORCE_NONE=1 run $ARM64EMU_NATIVE_GUEST_BINDS "$SVD" /sigview "$SVA" "$SVB" control 2>/dev/null)
    sv_stop
    check_contains "no registry: a guest still signals its own child" \
        "kill-child=0" "$_got"
    check_contains "no registry: ...and its own process groups" \
        "counts us=1 c1=1 c2=1 c3=1" "$_got"
fi
rm -rf "$SVD"
