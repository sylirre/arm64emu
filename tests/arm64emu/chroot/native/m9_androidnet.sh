# M9 Android seccomp-compat net (sourced by tests/arm64emu/chroot/native/run.sh): the calls
# an ambient filter refuses, answered as the kernel would.
echo "== M9: Android seccomp net =="
# The uid and gid setters without -u, in a guest. Android's app filter refuses
# all of them but setresuid, and each was answered ENOSYS, from a trap of that
# filter; the kernel tells such a process 0 for a call that changes nothing,
# EPERM for one that would, EINVAL for an id of -1 (cred.c real_set). Run once
# as the host serves the setters and once with ARM64EMU_SETID_FORCE_DENY=1, which
# takes them as Android does. The text is what a kernel prints, libc's setxid
# broadcast to a second thread included.
SETID_DIR=$(mktemp -d)
SETID_WANT="setuid_self=0
setgid_self=0
setuid_other=-1 errno=1
setgid_other=-1 errno=1
setuid_invalid=-1 errno=22
setgid_invalid=-1 errno=22
setreuid_keep=0
setreuid_self=0
setregid_other=-1 errno=1
setresuid_self=0
setresuid_other=-1 errno=1
setresgid_keep=0
setresgid_other=-1 errno=1
setfsuid_self=1
setfsuid_other=1
setfsgid_other=1
setgroups_none=-1 errno=1
ids_kept=1
threads_setgid=0
threads_setuid=0
done"
if guest_xlate_ready "uid/gid setters in a guest" &&
    guest_cc_report "$SETID_DIR/setid" tests/arm64emu/chroot/native/guests/setid.c -pthread; then
    for _deny in 0 1; do
        _label="uid/gid setters in a guest answer as a kernel does"
        [ "$_deny" = 1 ] &&
            _label="uid/gid setters in a guest, refused as Android refuses them"
        # shellcheck disable=SC2086  # $ARM64EMU_NATIVE_GUEST_BINDS is a deliberately split arg list
        _got=$(
            [ "$_deny" = 1 ] && export ARM64EMU_SETID_FORCE_DENY=1
            run_t 60 $ARM64EMU_NATIVE_GUEST_BINDS "$SETID_DIR" /setid 2>/dev/null
        )
        case "$_got" in
        SKIP:*)
            skip "$_label: ${_got#SKIP: }"
            continue
            ;;
        esac
        if [ "$_got" = "$SETID_WANT" ]; then
            pass=$((pass + 1))
            echo "  ok   $_label"
        else
            fail=$((fail + 1))
            echo "  FAIL $_label"
            printf '%s\n' "$_got" | sed 's/^/    /' | head -24
        fi
    done
    # ...and under a filter of the host's that refuses the eight as Android's
    # does, installed before arm64emu starts (tests/arm64emu/chroot/native/guests/ambtrap.c): what
    # the startup probe has to find, rather than what the knob says.
    if guest_cc_report "$SETID_DIR/ambtrap" tests/arm64emu/chroot/native/guests/ambtrap.c; then
        _label="uid/gid setters in a guest, under a filter that refuses them"
        # shellcheck disable=SC2086  # $ARM64EMU_NATIVE_GUEST_BINDS is a deliberately split arg list
        _got=$(emu_t 60 "$SETID_DIR/ambtrap" "$ARM64EMU_NATIVE_BIN" chroot --native $ARM64EMU_NATIVE_GUEST_BINDS \
            "$SETID_DIR" /setid 2>/dev/null)
        case "$_got" in
        SKIP:*)
            skip "$_label: ${_got#SKIP: }"
            ;;
        *)
            if [ "$_got" = "$SETID_WANT" ]; then
                pass=$((pass + 1))
                echo "  ok   $_label"
            else
                fail=$((fail + 1))
                echo "  FAIL $_label"
                printf '%s\n' "$_got" | sed 's/^/    /' | head -24
            fi
            ;;
        esac
    fi
fi
rm -rf "$SETID_DIR"
