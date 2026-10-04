# M27 terminal ioctls an Android app is refused (sourced by tests/run.sh).
#
# Android's SELinux policy whitelists the ioctls an app may issue on its pty,
# and the ones a libc builds its verdict about a terminal on are not all on the
# list: the host answers EACCES, and the guest -- which makes its ioctls itself
# -- took its terminal for none. The filter traps those requests (tty.c) and
# the dispatcher answers them from the commands the policy allows. Each guest
# here prints what a real kernel prints (the expected text was taken from a
# native build on one), so a host that serves the ioctl itself, a host that
# refuses it, and the knob that forces the refusal anywhere must all agree.
echo "== M27: terminal ioctls =="

TTY_DIR=$(mktemp -d)

# tty_check DESC GUEST KNOB WANT — run /GUEST on every tier this host has, once
# as the host serves it and once with KNOB=1, which refuses the ioctl before
# the host is asked (the tier Android's policy serves).
tty_check() {
    _desc=$1
    _guest=$2
    _knob=$3
    _want=$4
    for _tier in -R plain; do
        if [ "$_tier" = plain ]; then
            if [ "$CNG_SECCOMP_LIVE" != 1 ]; then
                skip "$_desc, seccomp tier: the filter is inert here"
                continue
            fi
            _opt=""
        else
            _opt="-R"
        fi
        for _deny in 0 1; do
            _label="$_desc ($_tier)"
            [ "$_deny" = 1 ] && _label="$_desc, the host's refusal forced ($_tier)"
            # shellcheck disable=SC2086  # $_opt and $GUEST_BINDS are split on purpose
            _got=$(
                [ "$_deny" = 1 ] && export "$_knob=1"
                run_t 60 $_opt $GUEST_BINDS "$TTY_DIR" "/$_guest" 2>/dev/null
            )
            case "$_got" in
            SKIP:*)
                skip "$_label: ${_got#SKIP: }"
                continue
                ;;
            esac
            if [ "$_got" = "$_want" ]; then
                pass=$((pass + 1))
                echo "  ok   $_label"
            else
                fail=$((fail + 1))
                echo "  FAIL $_label"
                printf '%s\n' "$_got" | sed 's/^/    /'
            fi
        done
    done
}

# TCGETS2/TCSETS2/TCSETSW2/TCSETSF2 over a real pty: the 44-byte struct comes
# back whole and no further, agrees with TCGETS on the first 36 bytes, each
# setter carries all of it in (c_ospeed too -- a copy that stopped at 36 reads
# back rate 0), a rate set through the master is read through the slave, an
# unmapped buffer is EFAULT both ways and a pipe is ENOTTY. A glibc since 2.42
# builds isatty on them: on a device the original build answered EACCES here,
# and bash in an Ubuntu 26.04 rootfs printed no prompt.
if guest_xlate_ready "termios2 ioctls" &&
    guest_cc_report "$TTY_DIR/termios2" tests/guests/termios2.c; then
    tty_check "termios2 ioctls agree with a kernel's" termios2 CNG_TERMIOS2_DENY \
"get2=0 errno=0 whole=1 tail=1 prefix=1
set2=0 errno=0 readback=1
setsw2=0 errno=0 readback=1
setsf2=0 errno=0 readback=1
master_set2=0 errno=0 readback=1
master_get2=0 speed=1
get2_fault=-1 errno=14
set2_fault=-1 errno=14
pipe=-1 errno=25
done"
fi

rm -rf "$TTY_DIR"
