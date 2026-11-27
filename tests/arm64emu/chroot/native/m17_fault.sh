# M17-7 guest-pointer validation tests (sourced by tests/arm64emu/chroot/native/run.sh).
#
# The syscalls the monitor emulates rather than re-issues dereference the
# guest's pointers itself, so the kernel never validates them for us — and the
# SIGSYS handler runs with SIGSEGV masked, which makes a fault there unblockable
# and fatal. A bad pointer must therefore come back -EFAULT.
#
# A guest hands NULL to every path-bearing call (guests/nullpath.c), compared
# with the kernel's answers byte for byte.
echo "== M17-7: EFAULT instead of death on a bad guest pointer =="

# A NULL pathname. getname() faults on it before the dirfd, the flags or any permission
# check are looked at, so the answer is EFAULT and never ENOENT -- and the
# emulation gets that by handing the NULL to the kernel rather than deciding for
# itself, which is also what keeps utimensat's and statx's legitimate NULL
# spellings answering as the host answers. linkat resolved both ends itself,
# re-issued neither, and gave a NULL name the ENOENT it gives an unresolvable
# one. Differential over ~35 calls against the same program under the kernel.
NULD=$(mktemp -d)
if guest_xlate_ready "NULL-pathname leg" &&
    guest_cc_report "$NULD/nullpath" tests/arm64emu/chroot/native/guests/nullpath.c; then
    nul_k=$(emu "$NULD/nullpath" "$NULD" 2>/dev/null)
    # Where the reference has no openat2 (qemu-user, a pre-5.6 kernel) the
    # emulation still answers it (M97), and a kernel that has the call faults
    # a NULL name before anything else: EFAULT is the answer to hold it to.
    nul_k=$(printf '%s\n' "$nul_k" | sed 's/^openat2=unavailable$/openat2=14/')
    # shellcheck disable=SC2086  # $ARM64EMU_NATIVE_GUEST_BINDS is a deliberately split arg list
    nul_g=$(run_t 60 $ARM64EMU_NATIVE_GUEST_BINDS "$NULD" /nullpath 2>/dev/null)
    if [ -z "$nul_k" ]; then
        skip "NULL-pathname differential: the reference run produced nothing"
    elif [ "$nul_k" = "$nul_g" ]; then
        pass=$((pass + 1))
        echo "  ok   a NULL pathname answers as the kernel does, call for call"
    else
        fail=$((fail + 1))
        echo "  FAIL a NULL pathname diverges from the kernel"
        printf '    kernel: %s\n' "$(echo "$nul_k" | tr '\n' '|')"
        printf '    cng   : %s\n' "$(echo "$nul_g" | tr '\n' '|')"
    fi
    check_contains "linkat's source name is no exception" "linkat_src=14" "$nul_g"
    check_contains "...nor is its destination" "linkat_dst=14" "$nul_g"
    # The flagged NULL is whatever this kernel says: EFAULT from 6.10 on, and
    # before that the ENOENT an unprivileged caller gets for the flag itself,
    # ahead of the name — both are the kernel's to order, so the oracle's own
    # line is the needle.
    _w=$(printf '%s\n' "$nul_k" | grep "^linkat_src_empty_flag=")
    check_contains "...and AT_EMPTY_PATH does not make a NULL an empty name" \
        "$_w" "$nul_g"
    check_contains "a dirfd still names itself through a NULL utimensat" \
        "utimensat_dirfd=0" "$nul_g"
fi
rm -rf "$NULD"
