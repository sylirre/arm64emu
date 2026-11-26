# M12: System V shared memory (sourced by tests/chroot-ng/run.sh).
#
# Two layers. First a differential leg: the same guest binaries run once under
# chroot-ng's emulation and once with
# no emulation at all, where they get the host kernel's REAL SysV shm — stdout
# must match byte for byte, which is the strongest statement available that the
# emulation is indistinguishable. Finally the namespace scope, which
# has no kernel counterpart to diff against (that is the point) and is asserted
# directly.
#
# The programs print only semantic outcomes — never shmids, addresses or
# timestamps — since those legitimately differ between the host's global
# namespace and ours. See tests/chroot-ng/guests/shm_sysv.c.
echo "== M12: System V shared memory =="

# --- differential against the host kernel's real SysV shm -------------------
# The reference side is the same binary run with no emulation at all, so it needs
# the host to actually provide SysV shm: Android does not (bionic drops the API
# and the app domain gets no IPC), so there the differential legs sit out and the
# dispatcher self-test above is what covers the emulation.
SG=$(mktemp -d)
# shellcheck disable=SC2086  # $CHROOTNG_GUEST_BINDS is a deliberately split arg list
m12run() { run $CHROOTNG_GUEST_BINDS "$@"; }

# --- what the guest can see of the broker's own startup ---------------------
# The daemon is started by a double fork from whichever guest process makes the
# first SysV IPC call, and the middle child of that fork is a child of the
# guest's process. Its exit used to send the guest a SIGCHLD for a process the
# guest never started (and could be reaped by a wait() on another thread). It is
# cloned with no exit signal now, which the kernel neither signals nor shows to
# an ordinary wait. Needs no host SysV shm — the emulation is the whole
# subject — so it sits outside the differential block below.
if ! guest_xlate_ready "broker spawn visibility"; then
    :
elif ! guest_cc_report "$SG/brokerchld" tests/chroot-ng/guests/brokerchld.c; then
    :
else
    # shellcheck disable=SC2086  # $CHROOTNG_GUEST_BINDS is a deliberately split arg list
    bc_out=$(run_t 60 $CHROOTNG_GUEST_BINDS "$SG" /brokerchld 2>/dev/null)
    # The control: this run can see a SIGCHLD and reap a child at all.
    check_contains "m12 a guest's own child is still its own" \
        "own: chld=1 reaped=1 status=3" "$bc_out"
    check_contains "m12 the first SysV call starts the broker" \
        "broker: got=1" "$bc_out"
    case "$bc_out" in
    *"quiet: 1"*)
        check_contains "m12 starting the broker leaves the guest nothing to see" \
            "broker: got=1 chld=0 wait=-1 e=10" "$bc_out" ;;
    *)
        skip "m12 broker spawn visibility: no exit-signal-less clone here (qemu-user implements clone(SIGCHLD) and nothing else)" ;;
    esac
fi

# --- POSIX shm: the other shared memory, which is a directory ---------------
# shm_open() has no syscall behind it: it is an open() under /dev/shm, so a
# guest has POSIX shared memory exactly when it has a writable one. Android has
# no such directory at all, and the synthesized mount table said it did, so a
# glibc or musl guest was told tmpfs and then handed ENOENT. chroot-ng serves
# one from a per-uid directory under $TMPDIR there; this asks a real libc
# whether that is enough, and asks it against the stand-in specifically, so a
# host with its own /dev/shm exercises the same path.
#
# guest_cc rather than guest_cc_report on purpose: bionic has no shm_open at
# all (Android uses ashmem), so on a Termux host the program cannot build and
# that is a skip, not a failure. The rootfs guests this matters for — Alpine,
# Debian — are the ones that have it.
if ! guest_xlate_ready "POSIX shm"; then
    :
elif ! guest_cc "$SG/shm_posix" tests/chroot-ng/guests/shm_posix.c; then
    skip "POSIX shm: no shm_open in this guest libc (bionic has none)"
else
    m12_want="posix: created
posix: read POSIX-SHM-OK
posix: after-unlink gone
posix: done"
    M12T=$(mktemp -d)
    # shellcheck disable=SC2086  # $CHROOTNG_GUEST_BINDS is a deliberately split list
    m12_got=$(TMPDIR="$M12T" CHROOTNG_DEVSHM_FORCE_TMP=1 run_t 60 $CHROOTNG_GUEST_BINDS \
        "$SG" /shm_posix 2>/dev/null); m12_rc=$?
    check "m12 a POSIX shm round trip on the /dev/shm stand-in exits 0" 0 "$m12_rc"
    if [ "$m12_got" = "$m12_want" ]; then
        pass=$((pass + 1))
        printf '  ok   m12 ...create, map, reopen by name, read back, unlink\n'
    else
        fail=$((fail + 1))
        printf '  FAIL m12 POSIX shm round trip on the stand-in\n'
        printf '    want: %s\n' "$(echo "$m12_want" | tr '\n' '|')"
        printf '    got : %s\n' "$(echo "$m12_got" | tr '\n' '|')"
    fi
    # And it really was the stand-in: the object is created and unlinked inside
    # the run, so what is left behind is the directory chroot-ng made for it.
    if [ -n "$(find "$M12T" -maxdepth 1 -type d -name 'chroot-ng-shm.v1.*' \
        2>/dev/null)" ]; then
        pass=$((pass + 1))
        printf '  ok   m12 ...in the per-uid directory under $TMPDIR\n'
    else
        fail=$((fail + 1))
        printf '  FAIL m12 the stand-in directory was not made under $TMPDIR\n'
        find "$M12T" | sed 's/^/    /' | head -5
    fi
    rm -rf "$M12T"
fi

# --- a Termux process attaches a guest's segment ----------------------------
# Termux's X servers (termux-x11, Xvfb, Xvnc) carry libandroid-shmem, and an
# XShmAttach makes them shmat the client's id through it: the id's high half
# names the creator's "/dev/shm/%08x" socket, which hands the backing over. The
# guest's ids used to be 1, 2, 3..., which libandroid-shmem takes for its own
# and refuses, so every MIT-SHM client in the guest (mpv --vo=x11, GTK, cairo)
# got BadAccess. The oracle is the consumer itself: a host program linked
# against Termux's libandroid-shmem attaches what the guest made, reads it, and
# writes back. Termux only, where that library is.
if [ "$CHROOTNG_TERMUX" != 1 ]; then
    skip "m12 libandroid-shmem attach: not a Termux host"
elif ! guest_xlate_ready "libandroid-shmem attach"; then
    :
elif ! guest_cc_report "$SG/shm_ashv" tests/chroot-ng/guests/shm_ashv.c; then
    :
elif ! guest_cc "$SG/ashv_attach" tests/chroot-ng/guests/ashv_attach.c -landroid-shmem
then
    skip "m12 libandroid-shmem attach: it does not link here ($(head -1 "$GUEST_CC_LOG" 2>/dev/null))"
else
    AD=$SG/ashv
    mkdir -p "$AD"
    # shellcheck disable=SC2086  # $CHROOTNG_GUEST_BINDS is a deliberately split list
    run_t 60 $CHROOTNG_GUEST_BINDS "$SG" /shm_ashv /ashv >"$AD/guest.out" 2>&1 &
    AJ=$!
    _n=0
    until [ -s "$AD/id" ] || [ $_n -ge 300 ]; do
        sleep 0.1
        _n=$((_n + 1))
    done
    if [ -s "$AD/id" ]; then
        ah_out=$(emu_t 30 "$SG/ashv_attach" "$(cat "$AD/id")" 2>&1)
    else
        ah_out="no id published"
    fi
    : >"$AD/done"
    wait "$AJ"
    check_contains "m12 a Termux process attaches a guest segment through libandroid-shmem" \
        "host: saw=guest-wrote-this stat=0 size-ok=1" "$ah_out"
    check_contains "m12 ...and the guest sees what it wrote back" \
        "guest: id-positive=1 back=host-wrote-this" "$(cat "$AD/guest.out")"
fi

m12_kbase=
m12_ready=0
if guest_xlate_ready "SysV shm differential"; then
    m12_ready=1
    for p in shm_sysv shm_stat shm_exec shm_key; do
        guest_cc "$SG/$p" "tests/chroot-ng/guests/$p.c" || m12_ready=0
    done
    if [ "$m12_ready" = 0 ]; then
        skip "SysV shm differential: the guest programs do not build here ($(head -1 "$GUEST_CC_LOG" 2>/dev/null))"
    else
        # Android drops SysV IPC from bionic and denies it to app domains, so
        # there is no kernel reference to diff against there.
        m12_kbase=$(emu_t 60 "$SG/shm_sysv" 2>/dev/null)
        case "$m12_kbase" in
        *done*) ;;
        *)
            m12_ready=0
            skip "SysV shm differential: this host has no working SysV shm to diff against"
            ;;
        esac
    fi
fi
if [ "$m12_ready" = 1 ]; then
    # shm_diff <desc> <prog>: emulated vs the real kernel, stdout + rc.
    shm_diff() {
        out_k=$(emu_t 60 "$SG/$2" 2>/dev/null); rc_k=$?
        # shellcheck disable=SC2086  # $CHROOTNG_GUEST_BINDS is a deliberately split list
        out_e=$(run_t 60 $CHROOTNG_GUEST_BINDS "$SG" "/$2" 2>/dev/null); rc_e=$?
        if [ "$out_k" = "$out_e" ] && [ "$rc_k" = "$rc_e" ]; then
            pass=$((pass + 1)); printf '  ok   %s\n' "$1"
        else
            fail=$((fail + 1)); printf '  FAIL %s\n' "$1"
            printf '    kernel: %s (rc %s)\n' "$(echo "$out_k" | tr '\n' '|')" "$rc_k"
            printf '    cng   : %s (rc %s)\n' "$(echo "$out_e" | tr '\n' '|')" "$rc_e"
        fi
    }
    shm_diff "m12 shm_sysv matches the real kernel byte-for-byte" shm_sysv
    shm_diff "m12 shm_stat (ipcs enumeration) matches the real kernel" shm_stat
    # The emulated execve keeps the address space, so it has to drop attaches
    # itself; a real execve gets it for free by replacing the address space.
    shm_diff "m12 execve detaches attachments as a real one does" shm_exec
    # Three legs whose reference cannot be qemu-aarch64: it implements shmat
    # and shmdt itself (it has to manage the guest address space), so on those
    # it referees its own reading of the rules rather than the kernel's. Their
    # oracle is a host-native build of the same program instead, as M18 does
    # for ptrace and M22 for msgsnd; on a host that runs the guest build
    # directly it is that build, with no chroot-ng in the way. What they print
    # is protocol only (errno names, counts), so the comparison holds across
    # architectures.
    #
    # m12_host_diff <desc> <prog> <fail-desc> <timeout>: build both sides, run
    # both, and compare stdout. A side that cannot be built is a skip.
    m12_host_diff() {
        _oracle=$SG/$2
        if ! guest_cc "$SG/$2" "tests/chroot-ng/guests/$2.c"; then
            skip "m12 $2: the guest program does not build here"
            return
        fi
        if [ -n "$CHROOTNG_EMU" ]; then
            _oracle=
            for _c in ${HOSTCC:-} cc gcc clang; do
                have "$_c" || continue
                if "$_c" -O2 -o "$SG/$2.host" "tests/chroot-ng/guests/$2.c" 2>/dev/null
                then
                    _oracle=$SG/$2.host
                    break
                fi
            done
            if [ -z "$_oracle" ]; then
                skip "m12 $2: no host compiler for the differential oracle"
                return
            fi
        fi
        if [ -n "$TIMEOUT" ]; then
            _k=$("$TIMEOUT" "$4" "$_oracle" 2>/dev/null)
        else
            _k=$("$_oracle" 2>/dev/null)
        fi
        # shellcheck disable=SC2086  # $CHROOTNG_GUEST_BINDS is a deliberately split list
        _e=$(run_t "$4" $CHROOTNG_GUEST_BINDS "$SG" "/$2" 2>/dev/null)
        if [ "$_k" = "$_e" ]; then
            pass=$((pass + 1))
            printf '  ok   %s\n' "$1"
        else
            fail=$((fail + 1))
            printf '  FAIL %s\n' "$3"
            printf '    kernel: %s\n' "$(echo "$_k" | tr '\n' '|')"
            printf '    cng   : %s\n' "$(echo "$_e" | tr '\n' '|')"
        fi
    }

    # The corner cases, each of which had a divergence when this was ported:
    # SHM_EXEC's permission check, SHM_LOCK, SHM_RND's rounding. See shm_edge.c.
    # A host oracle because of SHM_REMAP without an address: the kernel refuses
    # it (nothing to replace), qemu-user attaches anyway, and so did the
    # emulation while qemu was the referee — the first native run caught it.
    m12_host_diff "m12 shmat/shmctl corner cases match the real kernel" \
        shm_edge "m12 shmat/shmctl corner cases diverge from the kernel" 60

    # SHM_REMAP: qemu tracks attachments in a table of its own keyed by start
    # address and detaches the whole recorded region, which is the very mistake
    # under test. Measured with no chroot-ng in the picture — one page remapped
    # over the front of a 16-page attachment, then shmdt: the kernel keeps the
    # covered segment's nattch at 1 and leaves the tail mapped, qemu-user
    # reports 0 and unmaps all 64 KiB.
    m12_host_diff "m12 SHM_REMAP retires what it replaced" \
        shm_remap "m12 SHM_REMAP bookkeeping diverges from the kernel" 60

    # Both tables the emulation tracks attachments in used to be fixed-size, and
    # neither limit exists in the kernel: 128 attachments per process (past which
    # shmdt answered EINVAL for an address the kernel detaches) and 32 attaching
    # processes per segment (past which a dead attacher's count stayed on nattch
    # for good). qemu's own shm bookkeeping is 32 regions wide, so it cannot
    # referee this either.
    m12_host_diff "m12 attachment tracking has no limit the kernel does not" \
        shm_many "m12 attachment tracking runs out where the kernel does not" 120

    # ...and again over the file-backed tier, which must be indistinguishable
    # from the memfd one (only the broker's backing differs).
    out_k=$m12_kbase
    out_e=$(CHROOTNG_SHM_FORCE_FILE=1 m12run "$SG" /shm_sysv 2>/dev/null)
    check_contains "m12 file-backed shm_sysv still matches" "$out_k" "$out_e"

    # The guest's calls really are being emulated, not passed to the host: the
    # dispatcher traces every one under CHROOTNG_DEBUG.
    n=$(CHROOTNG_DEBUG=1 m12run "$SG" /shm_sysv 2>&1 | grep -c 'sysv-shm')
    if [ "$n" -ge 8 ]; then
        pass=$((pass + 1)); printf '  ok   %s\n' "m12 the guest's shm syscalls reach the emulation ($n)"
    else
        fail=$((fail + 1)); printf '  FAIL %s\n' "m12 guest shm syscalls not emulated (traced $n)"
    fi

    # --- namespace scope ----------------------------------------------------
    # Default: one namespace per invocation, so a keyed segment does not leak
    # from one launch to the next (two containers do not share IPC).
    K=51ab0001
    m12run "$SG" /shm_key $K create first-invocation >/dev/null 2>&1
    out=$(m12run "$SG" /shm_key $K find 2>/dev/null)
    check_contains "m12 a keyed segment does not leak between invocations" \
        "found=0" "$out"

    # --shared-proc widens the namespace to the rootfs, the same way it widens
    # the guest process view.
    K=51ab0002
    m12run --shared-proc "$SG" /shm_key $K create shared-invocation >/dev/null 2>&1
    out=$(m12run --shared-proc "$SG" /shm_key $K find 2>/dev/null)
    check_contains "m12 --shared-proc shares segments between invocations" \
        "found=1 text=shared-invocation" "$out"
    m12run --shared-proc "$SG" /shm_key $K rmid >/dev/null 2>&1
fi
rm -rf "$SG"

# --- guest-shell smoke ------------------------------------------------------
# ipcs(1) is what a user reaches for; busybox's applet drives SHM_INFO/SHM_STAT
# exactly as shm_stat.c does, so this only has to show the tool runs and reports
# an empty namespace the way it would against a kernel with no segments.
M12_ALPINE="${M12_ALPINE:-$CHROOTNG_ALPINE}"
if [ -n "$M12_ALPINE" ] && [ -x "$M12_ALPINE/bin/busybox" ]; then
    SR=$(mktemp -d); cp -a "$M12_ALPINE/." "$SR"
    out=$(run "$SR" /bin/sh -c 'ipcs -m; echo rc=$?' 2>/dev/null)
    check_contains "m12 ipcs -m runs in a guest shell" "rc=0" "$out"
    rm -rf "$SR"
else
    skip "ipcs guest-shell scenario: no alpine rootfs"
fi
