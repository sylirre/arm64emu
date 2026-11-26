# M5b monitor tests (sourced by tests/chroot-ng/run.sh): the syscall monitor as a
# guest meets it -- its arguments, what it refuses outright, and the path
# questions answered byte for byte against the kernel.
echo "== M5b: syscall monitor =="

ROOT=$(mktemp -d)
mkdir -p "$ROOT/etc"
printf 'HELLO-FROM-ROOTFS' > "$ROOT/etc/greeting"

# --- the rootfs argument itself ---------------------------------------------
# It is a prefix, and every guest path is joined to it, so the two ways of
# getting one that is not what the user typed both end somewhere they did not
# ask for. An empty one is the identity — `chroot-ng "$ROOTFS" rm -rf /` with the
# variable unset ran against the host root, silently. And one that did not fit
# the 512-byte field was truncated, which lands mid-component: the guest was
# rooted at a shorter path, or at one that is not a directory at all. (The
# truncation also read one byte past the buffer while trimming trailing slashes,
# since cng_strlcpy reports the length of the source.)
run "" /bin/true >/dev/null 2>&1
check "an empty rootfs argument is refused" 2 $?
check_contains "the empty rootfs is diagnosed, with the deliberate spelling" \
    "chroot-ng: <rootfs> is empty (use '/' for the host root)" \
    "$(run "" /bin/true 2>&1)"
M5B_DEEP="$ROOT"
while [ ${#M5B_DEEP} -lt 512 ]; do M5B_DEEP="$M5B_DEEP/dddddddddddddddddddd"; done
mkdir -p "$M5B_DEEP"
run "$M5B_DEEP" /bin/true >/dev/null 2>&1
check "a rootfs path too long to hold is refused, not truncated" 2 $?
check_contains "the over-long rootfs is diagnosed with the limit" \
    "path too long (max 511)" "$(run "$M5B_DEEP" /bin/true 2>&1)"
check_contains "a bind destination too long to hold is refused too" \
    "--bind '$ROOT:/$(printf 'b%.0s' $(seq 1 300))': path too long" \
    "$(run -b "$ROOT:/$(printf 'b%.0s' $(seq 1 300))" "$ROOT" /bin/true 2>&1)"

# An EMPTY CHROOTNG_DEBUG is off, as it is for every other CNG_* switch (the
# two beside it in the same loop, and cng_broker_env, all require a non-empty
# value). `CHROOTNG_DEBUG= chroot-ng ...` is how a shell clears a variable for one
# command; it used to turn verbose logging on instead, onto the guest's own
# stderr -- a stream package managers capture.
# The knob is read by cng_run, so a real (here: failing) guest launch is what
# drives it; the banner it stamps is the cheapest thing to look for.
check_contains "CHROOTNG_DEBUG=1 does log" "[cng] chroot-ng" \
    "$(CHROOTNG_DEBUG=1 run "$ROOT" /nosuchprogram 2>&1)"
check_absent "an empty CHROOTNG_DEBUG does not" "[cng] chroot-ng" \
    "$(CHROOTNG_DEBUG= run "$ROOT" /nosuchprogram 2>&1)"
check_absent "and CHROOTNG_DEBUG=0 does not" "[cng] chroot-ng" \
    "$(CHROOTNG_DEBUG=0 run "$ROOT" /nosuchprogram 2>&1)"

# AT_EMPTY_PATH names the descriptor, not a name to resolve. The empty name was
# walked like any other relative one -- the probe deciding that ends in a
# readlinkat, which for an empty name reports on the dirfd, so a symlink fd said
# "this is a link, walk it" -- and the walk then resolved the fd's own guest path
# dereferencing its final component. fstatat described the TARGET where the
# kernel describes the link, and a dangling link answered ENOENT where the kernel
# answers fine. That is the race-free lstat-by-fd idiom systemd and util-linux
# use everywhere. Byte-for-byte against the host build, which is the kernel.
EPD=$(mktemp -d)
printf hi > "$EPD/f"; ln -s f "$EPD/l"; ln -s nowhere "$EPD/dang"
if ! guest_xlate_ready "AT_EMPTY_PATH legs"; then
    :
elif guest_cc_report "$EPD/emptypath" tests/chroot-ng/guests/emptypath.c; then
    ep_k=""
    if [ -n "$CHROOTNG_EMU" ]; then
        # A foreign host's guests talk to the runner's kernel, the 6.1 arm64emu
        # advertises, not to the host's: linkat's AT_EMPTY_PATH is a 6.10
        # change, so the host build answers for another kernel than the one
        # the monitor re-issues to. The same build without the monitor does.
        ep_k=$(emu "$EPD/emptypath" "$EPD" 2>/dev/null)
    elif have cc && cc -O1 -o "$EPD/ep_host" tests/chroot-ng/guests/emptypath.c 2>/dev/null; then
        ep_k=$(kref "$EPD/ep_host" "$EPD" 2>/dev/null)
    fi
    # shellcheck disable=SC2086  # $CHROOTNG_GUEST_BINDS is a deliberately split arg list
    ep_g=$(run_t 60 $CHROOTNG_GUEST_BINDS "$EPD" /emptypath 2>/dev/null)
    # A kernel before 3.19 has no execveat and answers every exec_ line ENOSYS:
    # no reference for that half, which the emulation answers as the 6.1 it
    # advertises. The rest is still the kernel's to judge.
    case "$ep_k" in
    *"exec_cwd=38"*)
        skip "AT_EMPTY_PATH execveat differential: the host kernel predates execveat (3.19), so it is no reference"
        ep_k=$(printf '%s\n' "$ep_k" | grep -v '^exec_')
        ep_g=$(printf '%s\n' "$ep_g" | grep -v '^exec_') ;;
    esac
    if [ -z "$ep_k" ]; then
        skip "AT_EMPTY_PATH differential: no host compiler for the oracle"
    elif [ "$ep_k" = "$ep_g" ]; then
        pass=$((pass + 1))
        echo "  ok   AT_EMPTY_PATH matches the kernel byte-for-byte"
    else
        fail=$((fail + 1))
        echo "  FAIL AT_EMPTY_PATH diverges from the kernel"
        printf '    kernel: %s\n' "$(echo "$ep_k" | tr '\n' '|')"
        printf '    cng   : %s\n' "$(echo "$ep_g" | tr '\n' '|')"
    fi
    # linkat's half of the flag is a capability question, and the kernel's
    # answer is what the differential above pins: before 6.10 an unprivileged
    # caller is refused with ENOENT ahead of everything else, and it used to
    # get the link anyway, made through the descriptor's /proc link. Under
    # fake root the capability is faked the way chroot's CAP_SYS_CHROOT is, so
    # the descriptor links on every kernel — and the number still has to be
    # one: AT_FDCWD is the working directory (EPERM: no link to a directory),
    # a negative or closed number is EBADF, as root would have had them.
    # shellcheck disable=SC2086  # $CHROOTNG_GUEST_BINDS is a deliberately split arg list
    ep_r=$(run_t 60 -u 0:0 $CHROOTNG_GUEST_BINDS "$EPD" /emptypath 2>/dev/null)
    # ...and the link it makes is the host's to make: made where the filesystem
    # allows a hardlink (link_plain=0), and answered with the host's own errno
    # where it does not (Android's SELinux policy: EACCES). Either way it is a
    # link by name's verdict, which is not what the capability gate answers.
    ep_lp=$(printf '%s\n' "$ep_r" | sed -n 's/^link_plain=//p')
    case "$ep_lp" in '' | *[!0-9]*) ep_lp=missing ;; esac
    check_contains "fake root links by descriptor on every kernel" \
        "link_byfd=$ep_lp" "$ep_r"
    check_contains "...and AT_FDCWD is still the working directory there" \
        "link_cwd=1" "$ep_r"
    check_contains "...a number that is no descriptor is still EBADF" \
        "link_negfd=9" "$ep_r"
    check_contains "...a closed one too" "link_closedfd=9" "$ep_r"
    check_contains "...and an unknown flag bit is still EINVAL" \
        "link_badflag=22" "$ep_r"
    check_contains "...and the flag beside a real name links it" \
        "link_named_flag=$ep_lp" "$ep_r"
fi
rm -rf "$EPD"

# name_to_handle_at's flag runs the other way from every other *at() call: it
# does NOT follow a final symlink unless AT_SYMLINK_FOLLOW is given. Translation
# treated it as a follower, so it encoded the target instead of the link, and a
# dangling link — which the kernel encodes happily, never looking at the target —
# came back ENOENT. Byte-for-byte against the host build.
NHD=$(mktemp -d)
printf hi > "$NHD/f"; ln -s f "$NHD/l"; ln -s nowhere "$NHD/dang"
if ! guest_xlate_ready "name_to_handle_at legs"; then
    :
elif guest_cc_report "$NHD/handleat" tests/chroot-ng/guests/handleat.c; then
    nh_k=""
    nh_built=0
    if [ -n "$CHROOTNG_EMU" ]; then
        # The runner's kernel, as for AT_EMPTY_PATH above: arm64emu exports no
        # file handles, and the monitor re-issues to it.
        nh_built=1
        nh_k=$(emu "$NHD/handleat" "$NHD" 2>/dev/null)
    elif have cc && cc -O1 -o "$NHD/nh_host" tests/chroot-ng/guests/handleat.c 2>/dev/null; then
        nh_built=1
        # Android's ambient filter kills a process that issues name_to_handle_at
        # (SIGSYS), and the shell reports that on ITS stderr: the group takes it.
        nh_k=$({ kref "$NHD/nh_host" "$NHD" || true; } 2>/dev/null)
    fi
    # shellcheck disable=SC2086  # $CHROOTNG_GUEST_BINDS is a deliberately split arg list
    nh_g=$(run_t 60 $CHROOTNG_GUEST_BINDS "$NHD" /handleat 2>/dev/null)
    if [ "$nh_built" = 0 ]; then
        skip "name_to_handle_at differential: no host compiler for the oracle"
    elif [ -z "$nh_k" ]; then
        skip "name_to_handle_at differential: the host build cannot issue it (the ambient filter refuses it)"
    elif [ "$nh_k" = "$nh_g" ]; then
        pass=$((pass + 1))
        echo "  ok   name_to_handle_at matches the kernel byte-for-byte"
    else
        fail=$((fail + 1))
        echo "  FAIL name_to_handle_at diverges from the kernel"
        printf '    kernel: %s\n' "$(echo "$nh_k" | tr '\n' '|')"
        printf '    cng   : %s\n' "$(echo "$nh_g" | tr '\n' '|')"
    fi
fi
rm -rf "$NHD"

# The designed-ENOSYS set. io_uring submits path operations through a ring,
# never an svc, so a created ring reaches the host filesystem with no trap and
# no translation; clone3's flags sit behind a pointer, so BPF cannot tell a
# thread from a vfork, and refusing it puts glibc's posix_spawn/pthread_create
# back on __NR_clone, where the CLONE_VFORK->fork conversion lives; an mq name
# is not a filesystem path but an entry in the per-IPC-namespace mqueue mount,
# so no path trap can translate it and a guest mq_open() created its queue in
# the HOST's namespace; acct(2) names a file there is one of per machine; a
# guest-installed filter would also govern the syscalls the SIGSYS handler
# re-issues through the gate, which that filter knows nothing about. The mount
# family took paths and was neither trapped nor refused, so the guest's own
# untranslated spelling went to the host kernel to be judged; a mount cannot be
# carried out here in any case, so ENOSYS is the honest answer, where EPERM
# implied it might have worked with privilege.
DR=$(mktemp -d)
if guest_xlate_ready "designed-ENOSYS legs" &&
    guest_cc_report "$DR/denied" tests/chroot-ng/guests/denied.c; then
    # shellcheck disable=SC2086  # $CHROOTNG_GUEST_BINDS is a deliberately split arg list
    out=$(run $CHROOTNG_GUEST_BINDS "$DR" /denied 2>&1)
    check_absent "no designed-ENOSYS call is answered otherwise" "-> FAIL" "$out"
    for _leg in io_uring_setup io_uring_enter io_uring_register clone3 \
        statmount open_tree mq_open mq_timedsend mount umount2 pivot_root \
        quotactl swapon acct seccomp; do
        check_contains "$_leg is refused ENOSYS" "denied $_leg: rc=-38 -> OK" "$out"
    done
    check_contains "an ordinary syscall still runs" "denied control getpid: rc=" "$out"
fi
rm -rf "$DR"

rm -rf "$ROOT"

# End to end, the shape the JVM hit: a page that shares a VMA with a thread's
# stack, made executable by that thread. Where SELinux is enforcing the kernel
# refuses it for the stack pointer's sake (EACCES), so what is asserted is that
# the guest gets the same answer for that page as for one of its own. Elsewhere
# the call never fails and the legs pass without proving much; the structural
# legs above are what hold there. The gate is the harness's own probe of
# anonymous executable memory, not a case of the guest's: a baseline taken
# through the monitor would be refused by the very defect being tested, and the
# leg would skip instead of fail.
EXD=$(mktemp -d)
if guest_xlate_ready "executable mprotect beside a thread's stack" &&
    guest_cc_report "$EXD/execstack" tests/chroot-ng/guests/execstack.c -lpthread; then
    if [ "$CHROOTNG_EXECMEM" != 1 ]; then
        skip "executable mprotect beside a thread's stack: the host denies anonymous executable memory"
    else
        # shellcheck disable=SC2086  # $CHROOTNG_GUEST_BINDS is a deliberately split arg list
        out=$(run_t 60 $CHROOTNG_GUEST_BINDS "$EXD" /execstack 2>/dev/null)
        check_contains "a page of its own can be made RWX" "rwx_main=0" "$out"
        check_contains "a page beside a thread's stack can be made RWX" \
            "rwx_beside_stack=0" "$out"
        check_contains "...or R+X" "rx_beside_stack=0" "$out"
        check_contains "...and the guest is none the worse for it" \
            "rwx_after=0" "$out"
    fi
fi
rm -rf "$EXD"

# Two ways a guest path says "this had better be a directory", both of which
# canonicalization throws away -- because what canonicalization makes is a name,
# and neither of these is about the name.
#
# A trailing slash: "f/" says f is a directory, and Linux answers ENOTDIR when
# it is not, loudly enough to override the caller -- a final symlink is followed
# under O_NOFOLLOW and AT_SYMLINK_NOFOLLOW alike, and an O_CREAT becomes EISDIR.
#
# And "..", which is walked THROUGH the directory it leaves: "f/.." is ENOTDIR
# and "missing/.." is ENOENT, where collapsing the pair -- the thing that keeps
# a guest inside its rootfs, since "/../../etc" has to come out as "/etc" --
# never looks at what is being left.
#
# Neither statement reached the kernel, so every one of these answered about a
# file the kernel would have refused to reach. Mostly that was a wrong answer;
# for the calls that WRITE it was worse, since `unlink("f/")` and
# `unlink("f/../f")` both removed a file the kernel will not touch. The last
# legs ask it of a #! interpreter path, which the kernel opens by name like any
# other and which the emulation resolves down its own chain.
# Byte-for-byte against the host build.
TSD=$(mktemp -d)
mkdir -p "$TSD/d" "$TSD/d2"; printf hi > "$TSD/f"; : > "$TSD/d/mark"
printf x > "$TSD/a..b"   # dots in a name are not a ".." component
ln -s f "$TSD/l2f"; ln -s d "$TSD/l2d"; ln -s nowhere "$TSD/dang"
ln -s /d "$TSD/labs"   # absolute target: only the walk can re-root it
if ! guest_xlate_ready "trailing-slash legs"; then
    :
elif guest_cc_report "$TSD/trailslash" tests/chroot-ng/guests/trailslash.c; then
    ts_k=""
    if have cc && cc -O1 -o "$TSD/ts_host" tests/chroot-ng/guests/trailslash.c \
        2>/dev/null; then
        ts_k=$(kref "$TSD/ts_host" "$TSD" 2>/dev/null)
    fi
    # shellcheck disable=SC2086  # $CHROOTNG_GUEST_BINDS is a deliberately split arg list
    ts_g=$(run_t 60 $CHROOTNG_GUEST_BINDS "$TSD" /trailslash 2>/dev/null)
    if [ -z "$ts_k" ]; then
        skip "trailing-slash differential: no host compiler for the oracle"
    elif [ "$ts_k" = "$ts_g" ]; then
        pass=$((pass + 1))
        echo "  ok   a trailing slash and \"..\" match the kernel byte-for-byte"
    else
        fail=$((fail + 1))
        echo "  FAIL a trailing slash or \"..\" diverges from the kernel"
        printf '    kernel: %s\n' "$(echo "$ts_k" | tr '\n' '|')"
        printf '    cng   : %s\n' "$(echo "$ts_g" | tr '\n' '|')"
    fi
    # ...and the tree is still standing, which is the half of it that a diff
    # against a mangled oracle could not have caught on its own.
    check_contains "the path-spelling probes left the tree as they found it" \
        "intact=1" "$ts_g"
    # The one question the oracle cannot be asked: the same tree on the host has
    # no rootfs to be inside of. A trailing slash makes the kernel follow the
    # final symlink whatever O_NOFOLLOW said, so leaving that link for the
    # kernel would have it follow an ABSOLUTE target from the host root -- out
    # of the rootfs. The walk follows it first, where the target is re-rooted.
    # shellcheck disable=SC2086  # $CHROOTNG_GUEST_BINDS is a deliberately split arg list
    check_contains "a trailing slash on an absolute symlink stays in the rootfs" \
        "contain=rootfs" \
        "$(run_t 60 $CHROOTNG_GUEST_BINDS "$TSD" /trailslash "" contain 2>/dev/null)"
fi
rm -rf "$TSD"

