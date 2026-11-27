# M17-14 kernel-identity tests (sourced by tests/arm64emu/chroot/native/run.sh).
#
# uname(2) used to hand the guest the host's own release. Inside a rootfs that
# describes nothing the guest can act on, and on the target platform it is a
# device fingerprint (`-android14-11-...`, `-perf`). The two places a guest
# reads it — uname and /proc/version — must agree, because anything that
# cross-checks them (distro install scripts do) otherwise sees a contradiction.
echo "== M17-14: kernel identity =="

UR=$(mktemp -d)
if guest_xlate_ready "kernel identity" &&
    guest_cc_report "$UR/unameview" tests/arm64emu/chroot/native/guests/unameview.c; then
    # shellcheck disable=SC2086  # $ARM64EMU_NATIVE_GUEST_BINDS is a deliberately split arg list
    out=$(run $ARM64EMU_NATIVE_GUEST_BINDS "$UR" /unameview 2>&1)
    check "uname rc" 0 $?
    check_contains "release is the fixed identity, not the host's" \
        "rel=6.1.0-arm64emu" "$out"
    check_contains "version string is fixed too" "ver=#1 SMP arm64emu" "$out"
    check_contains "sysname and machine describe the guest ABI" \
        "sys=Linux" "$out"
    check_contains "machine is aarch64" "mach=aarch64" "$out"
    # nodename names the machine the guest is really on: `hostname` reports
    # it either way, and faking it would only confuse a user. The kernel
    # the guest's own uname reaches with no monitor says which that is.
    _node=$(emu "$UR/unameview" 2>/dev/null | sed -n 's/.* node=\([^ ]*\) .*/\1/p')
    check_contains "nodename stays the host's" "node=$_node " "$out"
    # The host release must not appear anywhere in what the guest was told.
    # A host whose release is the tier's own -- the emulator under the cross
    # host, which reports the same 6.1.0-arm64emu -- cannot tell the two
    # apart, and the leak rows say so rather than pass on nothing.
    _hrel=$(emu "$UR/unameview" 2>/dev/null | sed -n 's/.* rel=\([^ ]*\) .*/\1/p')
    if [ "$_hrel" = 6.1.0-arm64emu ]; then
        skip "the host release does not leak through uname: it is the tier's own here"
    else
        check_absent "the host release does not leak through uname" \
            "rel=${_hrel:-IMPOSSIBLE} " "$out"
    fi
    # ...including past the terminator. Each field is 65 bytes and the kernel
    # NUL-pads its own, so writing a shorter identity over a longer one leaves
    # the host's tail sitting there, readable by anything that walks the whole
    # array rather than stopping at the NUL. That tail is the device
    # fingerprint.
    check_contains "no host bytes survive past a faked field's terminator" \
        "tail_dirty=0" "$out"
    check_contains "/proc/version repeats the same release" \
        "version: Linux version 6.1.0-arm64emu" "$out"
    check_contains "...and the same version string" "#1 SMP arm64emu" "$out"
    # ...and named against a /proc dirfd, which is the other way a program
    # spells it. The dirfd-relative pre-filter lists the names worth resolving
    # and omitted this one, so the open fell through to the host file: uname
    # said 6.1.0-arm64emu while openat(dirfd, "version") returned the host's
    # real kernel, its build host and its compiler — the exact contradiction
    # faking both of them exists to avoid, plus a device fingerprint.
    check_contains "a dirfd-relative /proc/version is synthesized too" \
        "version_at: Linux version 6.1.0-arm64emu" "$out"
    if [ "$_hrel" = 6.1.0-arm64emu ]; then
        skip "...and leaks no host release either: it is the tier's own here"
    else
        check_absent "...and leaks no host release either" \
            "version_at: Linux version ${_hrel:-IMPOSSIBLE} " "$out"
    fi
fi
rm -rf "$UR"
