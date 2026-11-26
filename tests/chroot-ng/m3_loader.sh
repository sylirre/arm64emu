# M3 loader tests (sourced by tests/chroot-ng/run.sh). Builds a guest with whatever AArch64
# toolchain this host has (tests/chroot-ng/lib.sh picked it, and the link mode it proved
# works: static-PIE where possible, plain static, else dynamic on bionic) and
# runs it through `chroot-ng`. The rootfs is "/" throughout, so a dynamically
# linked guest still finds its interpreter at a real host path.
echo "== M3: ul_exec loader ($GUEST_LD_DESC guest) =="

GDIR=build/tests/chroot-ng
mkdir -p "$GDIR"

if guest_cc_report "$GDIR/hello" tests/chroot-ng/guests/hello.c; then
    # The link mode fixes the ELF type: -static-pie and a dynamic link give
    # ET_DYN (loaded at a base we choose), plain -static gives ET_EXEC at a fixed
    # vaddr. Both paths matter and the loader takes a different branch for each.
    case "$CHROOTNG_GUESTLD" in
    -static) m3_want=EXEC ;;
    *) m3_want=DYN ;;
    esac
    check "guest ELF type matches the link mode ($m3_want)" \
        "$m3_want" "$(elf_type "$GDIR/hello")"

    # CHROOTNG_TEST is set in BOTH environments and the guest must see the -E value:
    # the host environment is not inherited at all (M17-16 covers the scrubbing
    # itself; here it is the -E entry's trip through the stack builder).
    out=$(CHROOTNG_TEST=host run -E CHROOTNG_TEST=hello / "$GDIR/hello" AA BB 2>&1); rc=$?
    check "loader exit code propagates (42)" 42 $rc
    check_contains "guest ran (argc)" "guest: argc=3" "$out"
    check_contains "argv0 forwarded" "argv0=$GDIR/hello" "$out"
    check_contains "argv1 forwarded" "guest: argv1=AA" "$out"
    check_contains "argv2 forwarded" "guest: argv2=BB" "$out"
    check_contains "-E env forwarded, host value not" "guest: CHROOTNG_TEST=hello" "$out"
    check_contains "guest made a syscall (pid)" "guest: pid=" "$out"

    # file-backed mapping path (-F): the fallback used on Android when the
    # SELinux policy revokes anonymous executable memory. Must produce a working
    # guest. (It is the only tier available at all when execmem is denied, which
    # is why it is not gated on CHROOTNG_EXECMEM.)
    out=$(run -F / "$GDIR/hello" FB 2>&1); rc=$?
    check "file-backed (-F) exit code (42)" 42 $rc
    check_contains "file-backed guest ran" "guest: argc=2" "$out"
    check_contains "file-backed argv forwarded" "argv1=FB" "$out"
fi

# The initial stack's strings are one ascending run, argv then envp then the
# AT_EXECFN path, the way the kernel leaves them: libuv sizes the room for a
# process title from that shape, and the builder's old reversed argv made it
# negative (`npm -v` died in the memset). The guest checks every adjacency
# itself, then re-execs itself, so the stack an emulated execve builds is held
# to the same shape. The guest is run without chroot-ng first, so its idea of
# the layout is the host loader's.
if guest_cc_report "$GDIR/argvlayout" tests/chroot-ng/guests/argvlayout.c; then
    al="$PWD/$GDIR/argvlayout"
    if [ -n "$CHROOTNG_EMU" ]; then out=$(emu "$al" a bb ccc 2>&1)
    else out=$(kref "$al" a bb ccc 2>&1)
    fi
    check_contains "argv layout guest agrees with the host's own loader" \
        "layout: ok argc=5 envc=2" "$out"
    out=$(run / "$al" a bb ccc 2>&1); rc=$?
    check "argv layout guest exit 0" 0 $rc
    check_contains "initial stack: argv, envp and execfn strings ascending" \
        "layout: ok argc=4" "$out"
    check_contains "...and the same from an emulated execve" \
        "layout: ok argc=5 envc=2" "$out"
fi

# Programs whose headers do not make sense: each is the guest's own file with a
# field changed, exec'd by it in a child. A loader that sizes a mapping, a read
# or a copy from such a field has to judge it first, and the caller must live to
# read an errno or be killed as a kernel kills it -- never anything else, least
# of all a write outside what the object reserves, which here is the monitor's
# memory. Where the kernel answers before its point of no return the errno is
# its own; where it kills the caller, refusing earlier is no less right.
EBD=$(mktemp -d)
if guest_xlate_ready "malformed headers" &&
    guest_cc_report "$EBD/elfbad" tests/chroot-ng/guests/elfbad.c; then
    # shellcheck disable=SC2086  # $CHROOTNG_GUEST_BINDS is a deliberately split arg list
    out=$(run_t 120 $CHROOTNG_GUEST_BINDS "$EBD" /elfbad 2>/dev/null)
    check_contains "a malformed program leaves its exec'er alive" "parent alive" "$out"
    check_contains "...and an unchanged copy of it runs" "unchanged: ran" "$out"
    for _l in "no headers: errno 8" "odd header size: errno 8" \
        "headers past the end: errno 8" "wrong machine: errno 8" \
        "a relocatable: errno 8" "cut in the header: errno 8" \
        "an interpreter past the end: errno 5" "an interpreter too long: errno 8" \
        "an interpreter too short: errno 8" "an interpreter with no end: errno 8"; do
        check_contains "${_l%%:*}: refused with the kernel's errno" "$_l" "$out"
    done
    for _c in "no segment" "file part past the memory part" "file part past the end" \
        "a segment that wraps" "a span over all addresses" "an end that rounds up to 0"; do
        case "$(printf '%s\n' "$out" | grep "^$_c: ")" in
        "$_c: errno "* | "$_c: signal 11" | "$_c: signal 9")
            pass=$((pass + 1)); echo "  ok   $_c: refused, or kills only its exec'er" ;;
        *)
            fail=$((fail + 1)); echo "  FAIL $_c: refused, or kills only its exec'er"
            printf '%s\n' "$out" | grep "^$_c: " | sed 's/^/    /' ;;
        esac
    done
fi
rm -rf "$EBD"

# Fixed-address non-PIE guest (ET_EXEC @ 0x400000, e.g. gcc's cc1). chroot-ng is
# a PIE, placed where no such guest's link-time address is, so MAP_FIXED-loading
# one at 0x400000 does not overwrite our own monitor; verify it loads and runs
# (both the anon and file-backed paths, since the collision is in the fixed
# mapping).
if [ -n "$CHROOTNG_GUESTCC" ] &&
    $CHROOTNG_GUESTCC -static -no-pie -O2 -o "$GDIR/hello_exec" tests/chroot-ng/guests/hello.c \
        2>"$GUEST_CC_LOG"; then
    check "guest is fixed-address ET_EXEC" EXEC "$(elf_type "$GDIR/hello_exec")"
    out=$(run / "$GDIR/hello_exec" NP 2>&1); rc=$?
    check "ET_EXEC@0x400000 guest exit (42) — no collision with monitor" 42 $rc
    check_contains "ET_EXEC guest ran" "guest: argc=2" "$out"
    out=$(run -F / "$GDIR/hello_exec" NPF 2>&1); rc=$?
    check "ET_EXEC@0x400000 file-backed exit (42)" 42 $rc
else
    skip "ET_EXEC leg: no -static -no-pie AArch64 toolchain"
fi

# Two PT_LOADs sharing one host page. An object linked for a 4 KiB max page
# size on a 16 KiB kernel — Android's 16 KiB migration — can put its text and
# its data into the same host page, and the anonymous strategy is the only one
# that runs it at all. The final protections are applied per host page as the
# union of the segments touching it; applied per segment, the last one to touch
# the page won, and text lost execute or data lost write. The guest is a
# freestanding page of text beside a page of data. Under qemu-user the host
# page size is chosen with QEMU_PAGESIZE (the -p option), which is how a 16 KiB
# host is had here; a native host ignores it and runs at its own page size,
# which on a 16 KiB device is exactly the case under test.
if [ -n "$CHROOTNG_GUESTCC" ] &&
    $CHROOTNG_GUESTCC -nostdlib -static -Wl,-z,max-page-size=4096 \
        -Wl,-T,tests/chroot-ng/guests/pageshare.ld -o "$GDIR/pageshare" \
        tests/chroot-ng/guests/pageshare.S 2>"$GUEST_CC_LOG"; then
    out=$(run / "$GDIR/pageshare" 2>&1); rc=$?
    check "shared-page guest runs at the host page size" 0 $rc
    check_contains "shared-page guest wrote its data" "shared!" "$out"
    out=$(QEMU_PAGESIZE=16384 run / "$GDIR/pageshare" 2>&1); rc=$?
    check "shared-page guest runs on a 16 KiB page host" 0 $rc
    check_contains "text kept execute and data kept write on the shared page" \
        "shared!" "$out"
else
    skip "shared-page leg: no AArch64 toolchain for a freestanding guest"
fi

# The guest stack has a guard under it. A real main stack faults when it grows
# into stack_guard_gap; the one cng_build_stack mapped was RW to its last byte,
# with whatever the kernel had placed below it -- a library, a scratch stack of
# ours -- next in line for a recursion that ran off the end. The guest recurses
# 48 MiB deep and returns (the guard took nothing from the stack), then without
# limit and dies of SIGSEGV where it used to write into the neighbour.
if guest_cc_report "$GDIR/recurse" tests/chroot-ng/guests/recurse.c; then
    out=$(run_t 120 / "$GDIR/recurse" deep 2>&1); rc=$?
    check "a guest recursing 48 MiB deep returns (the guard costs no stack)" 0 $rc
    check_contains "...and did the work" "recurse: deep ok" "$out"
    out=$(run_t 120 / "$GDIR/recurse" overflow 2>&1); rc=$?
    check "a guest overflowing its stack dies of SIGSEGV (139)" 139 $rc
    check_absent "...and never ran past the guard" "survived the overflow" "$out"
else
    skip "stack guard guest legs: no AArch64 guest toolchain"
fi
