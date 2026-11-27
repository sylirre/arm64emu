# M22 System V semaphores and message queues (sourced by tests/arm64emu/chroot/native/run.sh).
#
# M12 gave the guest its own shared-memory namespace but left semget/semop/
# semctl/semtimedop and msgget/msgsnd/msgrcv/msgctl running natively, so on a
# desktop host the guest operated in the HOST's sem/msg namespace: it could
# attach to host semaphores, and host `ipcs -s`/`-q` listed the guest's objects.
# They were refused ENOSYS as a stop-gap; this milestone emulates them from the
# same broker daemon shm uses, which is also what makes them work at all on
# Android, where the whole SysV family is off the app seccomp allow-list.
#
# The guest programs run once under the emulation and once with no monitor in
# the way, where the same code reaches the kernel's own semget/semop/msgsnd --
# the host's, or on a foreign host arm64emu's emulation of them.
echo "== M22: System V semaphores and message queues =="

# --- differential against the host kernel's real SysV IPC -------------------
IG=$(mktemp -d)
m22_ready=0
if ! guest_xlate_ready "SysV sem/msg differential"; then
    :
else
    m22_ready=1
    for p in sem_sysv sem_block sem_undo sem_key; do
        guest_cc "$IG/$p" "tests/arm64emu/chroot/native/guests/$p.c" || m22_ready=0
    done
    if [ "$m22_ready" = 0 ]; then
        skip "SysV sem/msg differential: the guest programs do not build here ($(head -1 "$GUEST_CC_LOG" 2>/dev/null))"
    else
        # Android drops SysV IPC from bionic and denies it to app domains, so
        # there is no kernel reference to diff against there.
        m22_base=$(emu_t 90 "$IG/sem_sysv" 2>/dev/null)
        case "$m22_base" in
        *done*) ;;
        *)
            m22_ready=0
            skip "SysV sem/msg differential: this host has no working SysV IPC to diff against"
            ;;
        esac
    fi
fi
if [ "$m22_ready" = 1 ]; then
    # ipc_diff <desc> <prog>: emulated vs the real kernel, stdout + rc.
    ipc_diff() {
        out_k=$(emu_t 90 "$IG/$2" 2>/dev/null); rc_k=$?
        # shellcheck disable=SC2086  # $ARM64EMU_NATIVE_GUEST_BINDS is a deliberately split list
        out_e=$(run_t 90 $ARM64EMU_NATIVE_GUEST_BINDS "$IG" "/$2" 2>/dev/null); rc_e=$?
        if [ "$out_k" = "$out_e" ] && [ "$rc_k" = "$rc_e" ]; then
            pass=$((pass + 1)); printf '  ok   %s\n' "$1"
        else
            fail=$((fail + 1)); printf '  FAIL %s\n' "$1"
            printf '    kernel: %s (rc %s)\n' "$(echo "$out_k" | tr '\n' '|')" "$rc_k"
            printf '    cng   : %s (rc %s)\n' "$(echo "$out_e" | tr '\n' '|')" "$rc_e"
        fi
    }
    ipc_diff "m22 sem_sysv matches the real kernel byte-for-byte" sem_sysv
    # The blocking half: every way a sleeping operation can end. The EINTR leg
    # is the one that cannot work by accident -- the client half runs inside the
    # SIGSYS handler, where all signals but SIGSYS are masked, so a sleeping
    # semop is never woken by the signal arriving and the pending set has to be
    # polled against the mask the frame will restore.
    ipc_diff "m22 sem_block matches it for every way a sleeper ends" sem_block
    # SEM_UNDO with no exit hook anywhere: the broker applies it from the pid's
    # incarnation, and it has to be visible immediately after the reap.
    ipc_diff "m22 sem_undo matches it, SIGKILL included" sem_undo

    # --- msgsnd's refusal order, against a HOST-native reference -----------
    # The errno depends on where in the work the kernel stops: it reads mtype
    # with get_user() before it looks at anything else, then bounds msgsz, then
    # rejects mtype < 1, and only afterwards copies mtext. So a bad msgp beats
    # an out-of-range msgsz, and a bad mtype beats an unreadable mtext.
    # Validating the whole buffer in one go, after the size check, got both
    # backwards.
    #
    # This is the one leg here that qemu-aarch64 cannot referee: its own msgsnd
    # copies the guest's buffer in with a single lock_user() over
    # sizeof(long) + msgsz, so it reproduces the exact ordering under test.
    # Measured -- a type-0 message whose mtext runs off the mapping is EINVAL on
    # the real kernel and EFAULT under qemu-user. Hence a host build for the
    # reference side, as M18 does for ptrace.
    MO_ORACLE=$IG/msg_order
    mo_ready=1
    if ! guest_cc "$IG/msg_order" tests/arm64emu/chroot/native/guests/msg_order.c; then
        mo_ready=0
        skip "m22 msgsnd refusal order: the guest program does not build here"
    elif [ -n "$ARM64EMU_NATIVE_EMU" ]; then
        mo_ready=0
        for _c in ${HOSTCC:-} cc gcc clang; do
            have "$_c" || continue
            if "$_c" -O2 -o "$IG/mo_host" tests/arm64emu/chroot/native/guests/msg_order.c 2>/dev/null
            then
                MO_ORACLE=$IG/mo_host
                mo_ready=1
                break
            fi
        done
        [ "$mo_ready" = 1 ] ||
            skip "m22 msgsnd refusal order: no host compiler for the differential oracle"
    fi
    if [ "$mo_ready" = 1 ]; then
        if [ -n "$TIMEOUT" ]; then
            mo_k=$("$TIMEOUT" 60 "$MO_ORACLE" 2>/dev/null)
        else
            mo_k=$("$MO_ORACLE" 2>/dev/null)
        fi
        mo_e=$(run_t 90 "$IG" /msg_order 2>/dev/null)
        if [ "$mo_k" = "$mo_e" ]; then
            pass=$((pass + 1))
            printf '  ok   m22 msgsnd refuses in the kernel'\''s order\n'
        else
            fail=$((fail + 1))
            printf '  FAIL m22 msgsnd refuses in a different order than the kernel\n'
            printf '    kernel: %s\n' "$(echo "$mo_k" | tr '\n' '|')"
            printf '    cng   : %s\n' "$(echo "$mo_e" | tr '\n' '|')"
        fi
    fi

    # --- namespace scope (nothing to diff: the host has one namespace) -----
    # By default the namespace is per invocation, so a keyed set one launch
    # creates must be invisible to the next.
    K=51bb0001
    run_t 60 "$IG" /sem_key $K create 4 >/dev/null 2>&1
    out=$(run_t 60 "$IG" /sem_key $K find 2>/dev/null)
    check_contains "m22 a keyed set does not leak between invocations" \
        "found=0" "$out"

    # --shared-proc widens it to the rootfs, the same way it widens the process
    # view -- and then the second launch reads what the first left behind.
    K=51bb0002
    run_t 60 --shared-proc "$IG" /sem_key $K create 4 >/dev/null 2>&1
    out=$(run_t 60 --shared-proc "$IG" /sem_key $K find 2>/dev/null)
    check_contains "m22 --shared-proc shares sets and queues between invocations" \
        "found=1 val=4 msg=4" "$out"
    run_t 60 --shared-proc "$IG" /sem_key $K rmid >/dev/null 2>&1

    # --- and the containment that is the point of all of it ----------------
    # The guest's objects must never appear in the HOST's namespace. `ipcs` is
    # the check a user would make; where it is absent the leg sits out rather
    # than passing for the wrong reason.
    if ! have ipcs; then
        skip "m22 host-namespace check: no ipcs(1) here"
    else
        K=51bb0003
        run_t 60 "$IG" /sem_key $K create 4 >/dev/null 2>&1
        hostview=$(ipcs -s 2>/dev/null; ipcs -q 2>/dev/null)
        case "$hostview" in
        *51bb0003*) fail=$((fail + 1))
            echo "  FAIL m22 the guest's keyed objects reached the HOST namespace" ;;
        *) pass=$((pass + 1))
            echo "  ok   m22 the host sem/msg namespace is untouched" ;;
        esac
    fi
fi
rm -rf "$IG"

# --- guest-shell smoke ------------------------------------------------------
# `ipcs -s`/`-q` is what a user reaches for, and busybox's applet drives
# SEM_INFO/SEM_STAT and MSG_INFO/MSG_STAT. This only has to show the tool runs
# against the emulated
# namespace and reports it empty, the way it would against a kernel with none.
M22_ALPINE="${M22_ALPINE:-$ARM64EMU_NATIVE_ALPINE}"
if [ -n "$M22_ALPINE" ] && [ -x "$M22_ALPINE/bin/busybox" ]; then
    SR=$(mktemp -d); cp -a "$M22_ALPINE/." "$SR"
    out=$(run_t 60 "$SR" /bin/sh -c 'ipcs -s; ipcs -q; echo rc=$?' 2>/dev/null)
    check_contains "m22 ipcs -s and -q run in a guest shell" "rc=0" "$out"
    rm -rf "$SR"
else
    skip "m22 ipcs guest-shell scenario: no alpine rootfs"
fi
