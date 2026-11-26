# M24: openat2(2)'s open_how.resolve (sourced by tests/chroot-ng/run.sh).
#
# openat2 was translated like openat and re-issued with the guest's own
# open_how — which carries `resolve`, a description of the resolution WE are the
# one performing. Every bit of it was then judged against the wrong namespace:
# RESOLVE_BENEATH saw the absolute host path translation had just produced and
# answered EXDEV for every relative name a guest could pass; RESOLVE_IN_ROOT
# re-rooted that host path at the dirfd and named a file nobody asked for; and
# RESOLVE_NO_SYMLINKS was simply violated, since the resolver had already
# followed the links the guest asked it not to.
#
# A differential: the same guest program runs twice, once with no emulation,
# and the kernel's own answers are demanded byte for byte. Where openat2 is
# unavailable (a pre-5.6 kernel, Android's filter) chroot-ng emulates it, and
# the answers the kernel gives (recorded below) are what the emulation is held
# to instead.
echo "== M24: openat2 resolve semantics =="


# --- differential: the same program, once with no emulation at all -----------
#
# Where the host cannot issue openat2 — a pre-5.6 kernel, qemu-user, Android's
# filter — chroot-ng emulates it, and there is no kernel to diff against. The
# answers below stand in: the kernel's own, measured natively (6.8 on AArch64)
# and stable from 5.6 on. Where the host CAN issue it, the emulation is diffed
# against the live kernel instead, with CHROOTNG_OPENAT2_DENY taking it as refused.
O2_KERNEL='plain=ok
symlink=ok
nosym_link=-40
nosym_plain=ok
nosym_magic=-40
nomagic_magic=-40
nomagic_link=ok
beneath_abs=-18
beneath_dotdot=-18
beneath_abslink=-18
beneath_ok=ok
beneath_link=ok
inroot_abs=ok
inroot_abslink=ok
inroot_dotdot=ok
inroot_missing=-2
badbit=-22
both_scopes=-22
size_small=-22
size_big_zero=ok
size_big_tail=-7
mode_nocreat=-22
mode_toobig=-22
path_extra=-22
flag_high=-22
tmpfile_rdonly=-22
tmpfile_creat=-22
scope_badfd=-9
scope_badfd_abs=-18
scope_badfd_inroot=-9
scope_notdir=-20
tar_parent=ok
tar_parent_link=-18
tar_parent_up=-18
tar_create=ok
tar_create_again=-17
tar_create_up=-18
beneath_inside_dotdot=ok
beneath_nofollow=-40
beneath_pathlink=ok
beneath_missing=-2
beneath_notdir=-20'

# o2_cases WANT GOT — each case named on its own as well, so a failure says
# which rule broke rather than printing two blocks of text.
o2_cases() {
    for _c in nosym_link nosym_plain nosym_magic nomagic_magic \
        nomagic_link beneath_abs beneath_dotdot beneath_abslink \
        beneath_ok inroot_abs inroot_abslink inroot_dotdot badbit \
        size_small size_big_tail mode_nocreat mode_toobig path_extra \
        flag_high tmpfile_rdonly tmpfile_creat scope_badfd scope_badfd_abs \
        scope_badfd_inroot scope_notdir tar_parent tar_parent_link \
        tar_parent_up tar_create tar_create_again tar_create_up \
        beneath_inside_dotdot beneath_nofollow beneath_pathlink \
        beneath_missing beneath_notdir; do
        _w=$(printf '%s\n' "$1" | grep "^$_c=")
        check_contains "$3$_c" "$_w" "$2"
    done
}

O2D=$(mktemp -d)
mkdir -p "$O2D/d"
O2N=$(mktemp -d)
if ! guest_xlate_ready "openat2 differential"; then
    :
elif ! guest_cc_report "$O2D/openat2" tests/chroot-ng/guests/openat2.c; then
    :
else
    want=$(emu "$O2D/openat2" "$O2N" 2>/dev/null)
    case "$want" in
    "")
        skip "openat2 differential: the reference run produced nothing"
        ;;
    *openat2=unsupported*)
        # shellcheck disable=SC2086  # $CHROOTNG_GUEST_BINDS is a deliberately split list
        got=$(run $CHROOTNG_GUEST_BINDS "$O2D" /openat2 /d 2>/dev/null)
        check "openat2 is emulated where it cannot be issued, with the kernel's answers" \
            "$O2_KERNEL" "$got"
        o2_cases "$O2_KERNEL" "$got" "emulated: "
        ;;
    *)
        # shellcheck disable=SC2086  # $CHROOTNG_GUEST_BINDS is a deliberately split list
        got=$(run $CHROOTNG_GUEST_BINDS "$O2D" /openat2 /d 2>/dev/null)
        check "every openat2 resolve case matches the host kernel" "$want" "$got"
        o2_cases "$want" "$got" ""
        # ...and the emulation against this same kernel.
        _label="openat2 emulated (CHROOTNG_OPENAT2_DENY) matches the host kernel"
        # shellcheck disable=SC2086  # $CHROOTNG_GUEST_BINDS is a deliberately split arg list
        got=$(CHROOTNG_OPENAT2_DENY=1 run $CHROOTNG_GUEST_BINDS "$O2D" /openat2 /d \
            2>/dev/null)
        check "$_label" "$want" "$got"
        o2_cases "$want" "$got" "emulated: "
        ;;
    esac
    # A number the host refuses is emulated without a word on the guest's
    # stderr, which is the program's output to whoever reads it: the one-shot
    # "not permitted here" note is CHROOTNG_DEBUG's. It used to be printed on every
    # run, into the middle of whatever the guest wrote, on Android.
    if [ -n "$want" ]; then
        # shellcheck disable=SC2086  # $CHROOTNG_GUEST_BINDS is a deliberately split list
        err=$(CHROOTNG_OPENAT2_DENY=1 run $CHROOTNG_GUEST_BINDS "$O2D" /openat2 /d \
            2>&1 >/dev/null)
        check_absent "an emulated syscall is not announced on the guest's stderr" \
            "not permitted here" "$err"
        # shellcheck disable=SC2086  # $CHROOTNG_GUEST_BINDS is a deliberately split list
        err=$(CHROOTNG_DEBUG=1 CHROOTNG_OPENAT2_DENY=1 run $CHROOTNG_GUEST_BINDS "$O2D" \
            /openat2 /d 2>&1 >/dev/null)
        check_contains "...and CHROOTNG_DEBUG=1 does announce it" \
            "[cng] syscall 437 not permitted here -> emulated" "$err"
    fi
fi
rm -rf "$O2D" "$O2N"
