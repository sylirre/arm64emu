#!/usr/bin/env sh
# the native tier's test harness: runs `arm64emu chroot --native` -- natively
# on an AArch64 host, the AArch64 cross build under arm64emu's chroot mode on
# another -- and checks behaviour, one milestone script at a time (the
# milestones are chroot-ng's, the program the tier was before it was one).
# Everything host-dependent (emulator, guest toolchain, which translation tier
# is live, rootfs images) is resolved in tests/arm64emu/chroot/native/lib.sh;
# see the header there for the environment knobs.
set -u

cd "$(dirname "$0")/../../../.." || exit 1
. tests/arm64emu/chroot/native/lib.sh

if [ ! -x "$ARM64EMU_NATIVE_BIN" ]; then
    echo "build first: make test-arm64emu-chroot-native builds $ARM64EMU_NATIVE_BIN" >&2
    exit 1
fi

check() { # desc, expected_rc, actual_rc
    if [ "$2" = "$3" ]; then
        pass=$((pass + 1))
        printf '  ok   %s\n' "$1"
    else
        fail=$((fail + 1))
        printf '  FAIL %s (rc want %s got %s)\n' "$1" "$2" "$3"
    fi
}

check_contains() { # desc, needle, haystack
    case "$3" in
    *"$2"*)
        pass=$((pass + 1))
        printf '  ok   %s\n' "$1"
        ;;
    *)
        fail=$((fail + 1))
        printf '  FAIL %s (missing: %s)\n' "$1" "$2"
        ;;
    esac
}

check_absent() { # desc, needle, haystack
    case "$3" in
    *"$2"*)
        fail=$((fail + 1))
        printf '  FAIL %s (present: %s)\n' "$1" "$2"
        ;;
    *)
        pass=$((pass + 1))
        printf '  ok   %s\n' "$1"
        ;;
    esac
}

cng_platform_init
# A signal ends the run: trapped without an exit, it removed the scratch
# directory and let the suite go on without it.
trap 'rm -rf "$ARM64EMU_NATIVE_TMP"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
cng_platform_banner
if [ "$ARM64EMU_NATIVE_EMU_MISSING" = 1 ]; then
    echo "a $HOST_ARCH host runs the suite's AArch64 programs under arm64emu, and" \
        "build/arm64emu is not built: make arm64emu, or set ARM64EMU_NATIVE_EMU= if AArch64 binaries run here anyway" >&2
    exit 1
fi
# The seccomp filter is the whole of the tier's interception, and nothing a
# row asserts holds where it is not applied: qemu-user, or a kernel before 3.5.
if [ "$ARM64EMU_NATIVE_SECCOMP_LIVE" != 1 ]; then
    echo "a guest's path syscalls are not translated here: this host does not" \
        "apply a guest's seccomp filter, and the native tier cannot run on it" >&2
    exit 1
fi
echo

echo "== M1: CLI =="
out=$(run --help 2>&1); check "help rc" 0 $?
check_contains "help usage line" "arm64emu chroot [options] <rootfs> <program>" "$out"
check_contains "help names --native" "--native" "$out"
check_contains "help OPTIONS section" "OPTIONS" "$out"
out=$(run -h 2>&1); check "short -h rc" 0 $?
# The help is reflowed to the terminal, and where no terminal answers
# TIOCGWINSZ -- every redirected run, this one included -- to $COLUMNS. That
# value arrives from the environment, so the parse has to survive anything sent
# through it: an accumulator that overflowed was undefined behaviour, which the
# compiler is entitled to act on rather than wrap. The width is clamped to
# HELP_MIN_COLS..HELP_MAX_COLS, and the help is long enough that some line fills
# whatever width it is given. The help asks stdin as well as stdout and stderr,
# and a suite started from a terminal has one there, so stdin is detached too:
# without it a terminal answers and $COLUMNS is never consulted.
# A word is never split, and the help has some no width holds -- the knobs'
# names, the default PATH -- each on a line of its own past the width: the
# width is the longest line of more than one word.
help_width() { run --help </dev/null 2>&1 | awk 'NF > 1 { if (length > m) m = length } END { print m+0 }'; }
export COLUMNS
COLUMNS=40
check "COLUMNS renders the help at that width" 40 "$(help_width)"
COLUMNS=1
check "a COLUMNS under the floor clamps up" 32 "$(help_width)"
COLUMNS=4294967296
check "a COLUMNS past an int's range clamps to the ceiling" 92 "$(help_width)"
COLUMNS=99999999999999999999
check "...and so does one past every range" 92 "$(help_width)"
COLUMNS=abc
check "a COLUMNS that is not a number is ignored" 80 "$(help_width)"
unset COLUMNS
run >/dev/null 2>&1; check "no-args rc" 2 $?
run --frobnicate >/dev/null 2>&1; check "unknown-option rc" 2 $?
run / >/dev/null 2>&1; check "rootfs but no program rc" 2 $?

# M2+ tests are appended as milestones land.
if [ -f tests/arm64emu/chroot/native/m3_loader.sh ]; then . tests/arm64emu/chroot/native/m3_loader.sh; fi
if [ -f tests/arm64emu/chroot/native/m4_dynamic.sh ]; then . tests/arm64emu/chroot/native/m4_dynamic.sh; fi
if [ -f tests/arm64emu/chroot/native/m5_view.sh ]; then . tests/arm64emu/chroot/native/m5_view.sh; fi
if [ -f tests/arm64emu/chroot/native/m5b_monitor.sh ]; then . tests/arm64emu/chroot/native/m5b_monitor.sh; fi
if [ -f tests/arm64emu/chroot/native/m6_execve.sh ]; then . tests/arm64emu/chroot/native/m6_execve.sh; fi
if [ -f tests/arm64emu/chroot/native/m7_fidelity.sh ]; then . tests/arm64emu/chroot/native/m7_fidelity.sh; fi
if [ -f tests/arm64emu/chroot/native/m8_context.sh ]; then . tests/arm64emu/chroot/native/m8_context.sh; fi
if [ -f tests/arm64emu/chroot/native/m9_androidnet.sh ]; then . tests/arm64emu/chroot/native/m9_androidnet.sh; fi
if [ -f tests/arm64emu/chroot/native/m10_l2s_shell.sh ]; then . tests/arm64emu/chroot/native/m10_l2s_shell.sh; fi
if [ -f tests/arm64emu/chroot/native/m11_proc.sh ]; then . tests/arm64emu/chroot/native/m11_proc.sh; fi
if [ -f tests/arm64emu/chroot/native/m12_shm.sh ]; then . tests/arm64emu/chroot/native/m12_shm.sh; fi
if [ -f tests/arm64emu/chroot/native/m14_dev.sh ]; then . tests/arm64emu/chroot/native/m14_dev.sh; fi
if [ -f tests/arm64emu/chroot/native/m15_unixsock.sh ]; then . tests/arm64emu/chroot/native/m15_unixsock.sh; fi
if [ -f tests/arm64emu/chroot/native/m16_netlink.sh ]; then . tests/arm64emu/chroot/native/m16_netlink.sh; fi
if [ -f tests/arm64emu/chroot/native/m17_fault.sh ]; then . tests/arm64emu/chroot/native/m17_fault.sh; fi
if [ -f tests/arm64emu/chroot/native/m17_seccomp.sh ]; then . tests/arm64emu/chroot/native/m17_seccomp.sh; fi
if [ -f tests/arm64emu/chroot/native/m17_uname.sh ]; then . tests/arm64emu/chroot/native/m17_uname.sh; fi
if [ -f tests/arm64emu/chroot/native/m17_env.sh ]; then . tests/arm64emu/chroot/native/m17_env.sh; fi
if [ -f tests/arm64emu/chroot/native/m17_workdir.sh ]; then . tests/arm64emu/chroot/native/m17_workdir.sh; fi
if [ -f tests/arm64emu/chroot/native/m18_ptrace.sh ]; then . tests/arm64emu/chroot/native/m18_ptrace.sh; fi
if [ -f tests/arm64emu/chroot/native/m19_scripts.sh ]; then . tests/arm64emu/chroot/native/m19_scripts.sh; fi
if [ -f tests/arm64emu/chroot/native/m20_sysvipc.sh ]; then . tests/arm64emu/chroot/native/m20_sysvipc.sh; fi
if [ -f tests/arm64emu/chroot/native/m23_execmap.sh ]; then . tests/arm64emu/chroot/native/m23_execmap.sh; fi
if [ -f tests/arm64emu/chroot/native/m24_openat2.sh ]; then . tests/arm64emu/chroot/native/m24_openat2.sh; fi
if [ -f tests/arm64emu/chroot/native/m25_fdview.sh ]; then . tests/arm64emu/chroot/native/m25_fdview.sh; fi
if [ -f tests/arm64emu/chroot/native/m26_pinned.sh ]; then . tests/arm64emu/chroot/native/m26_pinned.sh; fi
if [ -f tests/arm64emu/chroot/native/m27_tty.sh ]; then . tests/arm64emu/chroot/native/m27_tty.sh; fi
if [ -f tests/arm64emu/chroot/native/m28_dacroot.sh ]; then . tests/arm64emu/chroot/native/m28_dacroot.sh; fi
if [ -f tests/arm64emu/chroot/native/m29_tier.sh ]; then . tests/arm64emu/chroot/native/m29_tier.sh; fi

echo
echo "== summary: $pass passed, $fail failed, $skipped skipped =="
[ "$fail" -eq 0 ]
