# M26: the resolved path is pinned for its syscall (sourced by tests/chroot-ng/run.sh).
#
# The walk turned a guest name into a host path and the syscall was then made
# on that string, which the kernel resolved again from scratch: a directory on
# the way swapped for an absolute symlink between the two was followed from
# the HOST root, and the call landed outside the rootfs. Every path-bearing
# syscall had the window. Now the directory the walk reached is held open for
# the call and the last component is given to the kernel against it, with the
# family's NOFOLLOW; a link that appears in the race is refused, never
# followed (src/chroot-ng/monitor/pin.c).
echo "== M26: pinned resolution =="

# shellcheck disable=SC2086  # $CHROOTNG_GUEST_BINDS is a deliberately split arg list
m26run() { run_t 120 $CHROOTNG_GUEST_BINDS "$@"; }
M26D=$(mktemp -d)

if ! guest_xlate_ready "pinned-resolution legs"; then
    :
else
    # Two threads: one flips /w/d between a directory and a symlink to a host
    # directory the guest cannot name, the other loops over the families and
    # counts what each answered. A single answer from the host's files is
    # the escape. The mutating families aim at names only the host side
    # has, and leave their mark there if they ever get through.
    if guest_cc "$M26D/pathrace" tests/chroot-ng/guests/pathrace.c -pthread; then
        R=$(mktemp -d); H=$(mktemp -d)
        # The host side must be a place the guest has no name for: where a bind
        # contains the scratch space (Termux's $TMPDIR is inside the bound
        # $PREFIX) it would be reachable by right, and every leg below would
        # be measuring the bind.
        H=$(cng_outside_binds "$H")
        mkdir -p "$R/bin" "$R/w/d" "$H/hostdir" "$H/sub"
        cp "$M26D/pathrace" "$R/bin/pathrace"
        printf 'guest\n' > "$R/w/d/x"; chmod 755 "$R/w/d/x"
        ln -s GUEST "$R/w/d/lnk"
        printf 'host\n' > "$H/x"; chmod 644 "$H/x"
        ln -s HOST "$H/lnk"
        : > "$H/victim"
        head -c 100 /dev/zero > "$H/big"
        out=$(m26run "$R" /bin/pathrace "$H" 4 2>/dev/null)
        check_contains "m26 the race never reaches the host: no family answers for it" \
            " host=0 " "$out"
        check_absent "m26 ...and the run exercised the families at all" \
            "iters=0 " "$out"
        check "m26 the host's file survived the unlinks" 0 \
            "$([ -e "$H/victim" ]; echo $?)"
        check "m26 the host's directory survived the rmdirs" 0 \
            "$([ -d "$H/hostdir" ]; echo $?)"
        check "m26 nothing was made in the host's directory" 1 \
            "$([ -e "$H/hostdir/made" ]; echo $?)"
        check "m26 the host's file kept its size" 100 \
            "$(wc -c < "$H/big" | tr -d ' ')"
        rm -rf "$R" "$H"
    else
        skip "pinned-resolution race leg: could not build tests/chroot-ng/guests/pathrace.c with -pthread"
    fi

    # A pathname socket is bound through the pinned directory, so what the
    # kernel stores is the monitor's spelling of the name — and a datagram
    # service answers at the source address its recvfrom reports. Server and
    # client are two invocations, neither forked from the other, each with a
    # pathname of its own: the server must read the client's name back as
    # the guest bound it, and its reply must arrive. The server stays bound
    # until the client is done, as a service does: a spelling resolves only
    # while its binder holds the directory, and a server gone right after its
    # reply left the client's readback to whichever process the scheduler ran
    # first (always the server on a 4.19 Pixel 5).
    if guest_cc "$M26D/uxreply" tests/chroot-ng/guests/uxreply.c; then
        R=$(mktemp -d); mkdir -p "$R/bin" "$R/run"
        cp "$M26D/uxreply" "$R/bin/uxreply"
        m26run "$R" /bin/uxreply server /run/srv.sock >"$M26D/srv.out" \
            2>/dev/null &
        m26bg=$!
        sleep 2
        cl=$(m26run "$R" /bin/uxreply client /run/srv.sock /run/cli.sock \
            2>/dev/null)
        wait $m26bg 2>/dev/null
        sv=$(cat "$M26D/srv.out")
        check_contains "m26 a server reads an unrelated client's bound name back" \
            "server: got=ping from=/run/cli.sock reply=1" "$sv"
        check_contains "m26 ...and its reply to that name arrives, from the server's" \
            "client: reply=pong from=/run/srv.sock" "$cl"
        check_absent "m26 ...with none of the monitor's spelling shown" \
            "/proc/" "$sv$cl"
        rm -rf "$R"
    else
        skip "pinned bind readback leg: could not build tests/chroot-ng/guests/uxreply.c"
    fi
fi
rm -rf "$M26D"
