# M29 the tier itself (sourced by tests/arm64emu/chroot/native/run.sh):
# `--native` is chroot mode's option, and where it cannot be what runs the
# guest a warning says why and the emulator runs it instead, with --jit, or
# with the interpreter where a step diagnostic is what needs the emulator.
# Every other row of this suite runs under ARM64EMU_NATIVE_NO_FALLBACK=1, the
# test aid that makes that an error instead, so that none of them passes on
# the emulator's answers; these rows clear it to see the fallback, and keep it
# to see the error.
echo "== M29: --native and its fallback =="

GDIR=build/tests/arm64emu/chroot/native
mkdir -p "$GDIR"

# --- an option or a knob that needs the emulator ----------------------------
# No guest is needed: the warning comes before the program is looked for, and
# a program that is not there is then the emulator's own "cannot execute" --
# which says that the emulator is what took over.
for _o in --strace --strace-full --max-insn=1000000000000; do
    out=$(ARM64EMU_NATIVE_NO_FALLBACK= run_t 120 $_o / /no-such-program 2>&1)
    check_contains "$_o: the warning says why the emulator runs instead" \
        "arm64emu: --native: ${_o%%=*} needs the emulator; continuing with --jit" "$out"
    check_contains "$_o: ...and the emulator runs the guest" \
        "arm64emu: cannot execute /no-such-program" "$out"
done
for _o in --debug --reg-trace; do
    out=$(ARM64EMU_NATIVE_NO_FALLBACK= run_t 120 $_o / /no-such-program 2>&1)
    check_contains "$_o: a step diagnostic takes the interpreter" \
        "arm64emu: --native: $_o needs the emulator; continuing with the interpreter" "$out"
    check_contains "$_o: ...which runs the guest" \
        "arm64emu: cannot execute /no-such-program" "$out"
done
out=$(ARM64EMU_NATIVE_NO_FALLBACK= ARM64EMU_PROF=1 run_t 120 / /no-such-program 2>&1)
check_contains "ARM64EMU_PROF: a step diagnostic's knob takes the interpreter too" \
    "arm64emu: --native: ARM64EMU_PROF needs the emulator; continuing with the interpreter" "$out"

# The test aid: the same command line is an error, and runs nothing.
out=$(run_t 60 --strace / /no-such-program 2>&1)
check "under ARM64EMU_NATIVE_NO_FALLBACK the fallback is an error" 126 $?
check_contains "...which says why" \
    "arm64emu: --native: --strace needs the emulator" "$out"
check_absent "...and does not go on" "continuing" "$out"
check_absent "...nor run the emulator" "cannot execute" "$out"

# --- what the tier asks the host ---------------------------------------------
# A host that refuses what the preflight asks for, made so by a filter
# installed before arm64emu starts (guests/denyfilter.c), as an ambient one is
# on a device.
if guest_cc_report "$GDIR/denyfilter" tests/arm64emu/chroot/native/guests/denyfilter.c &&
    guest_cc_report "$GDIR/m29hello" tests/arm64emu/chroot/native/guests/hello.c; then
    out=$(ARM64EMU_NATIVE_NO_FALLBACK= emu_t 120 "$GDIR/denyfilter" seccomp \
        "$ARM64EMU_NATIVE_BIN" chroot --native / "$PWD/$GDIR/m29hello" m29 2>&1)
    case "$out" in
    SKIP*) skip "a filter that refuses the tier's: none can be installed here" ;;
    *)
        check_contains "a kernel that refuses the seccomp filter: the warning names the errno" \
            "arm64emu: --native: the kernel refused the seccomp filter (EACCES); continuing with --jit" \
            "$out"
        check_contains "...and the emulator runs the guest" "guest: argv1=m29" "$out"
        ;;
    esac
    # Code that cannot be made executable leaves the emulator nothing to run
    # with either (its JIT is refused too), so these are seen under the test
    # aid, as errors. Only for a tier binary that is static: a dynamic one's
    # libraries are mapped executable by its own loader, under the same filter.
    if elf_has_interp "$ARM64EMU_NATIVE_BIN"; then
        skip "executable memory refused: $ARM64EMU_NATIVE_BIN is dynamic, its loader would be refused first"
    else
        out=$(emu_t 120 "$GDIR/denyfilter" exec "$ARM64EMU_NATIVE_BIN" chroot --native / \
            "$PWD/$GDIR/m29hello" 2>&1)
        rc=$?
        case "$out" in
        SKIP*) skip "executable memory refused: no filter can be installed here" ;;
        *)
            check "executable memory refused: the tier cannot run, and says so" 126 $rc
            check_contains "...the anonymous strategy's errno and the file's" \
                "arm64emu: --native: guest code cannot be made executable: anonymous memory may not be (EACCES), nor can" \
                "$out"
            check_contains "...naming the program" "$GDIR/m29hello be mapped so (EACCES)" "$out"
            out=$(emu_t 120 "$GDIR/denyfilter" exec "$ARM64EMU_NATIVE_BIN" chroot --native -F / \
                "$PWD/$GDIR/m29hello" 2>&1)
            check_contains "with -F only the file is asked" \
                "arm64emu: --native: with -F, $PWD/$GDIR/m29hello cannot be mapped executable (EACCES)" \
                "$out"
            ;;
        esac
    fi
fi

# --- the first program, a #! script -----------------------------------------
# As an execve's: the emulator routes its first program through its execve,
# and the tier follows the chain the same way its execve does (run.c,
# first_chain) -- npm, the first program of `arm64emu chroot --native rootfs
# /usr/bin/npm`, is a script. Each level's interpreter and argument go in
# ahead of the program as it was named, the deepest first, argv[0] dropped.
M29A="${ARM64EMU_NATIVE_ALPINE:-}"
if [ -z "$M29A" ] || [ ! -x "$M29A/bin/busybox" ]; then
    skip "a #! script as the first program: no alpine rootfs"
else
    M29S=$(mktemp -d)
    printf '#!/bin/sh -e\necho "script: $0 $*"\n' >"$M29S/s1"
    printf '#!/m29/s1 lvl2\n' >"$M29S/s2"
    chmod 0755 "$M29S/s1" "$M29S/s2"
    out=$(run -b "$M29S:/m29" "$M29A" /m29/s1 a b 2>&1)
    check_contains "a #! script is the first program" "script: /m29/s1 a b" "$out"
    out=$(run -b "$M29S:/m29" "$M29A" /m29/s2 a b 2>&1)
    check_contains "...and a chain of two, with the argv the kernel gives" \
        "script: /m29/s1 lvl2 /m29/s2 a b" "$out"
    chmod 0644 "$M29S/s1"
    out=$(run -b "$M29S:/m29" "$M29A" /m29/s1 2>&1)
    check "a #! script with no execute bit is not run" 126 $?
    check_contains "...and says why, as an exec would" \
        "arm64emu: cannot execute /m29/s1 (errno 13)" "$out"
    rm -rf "$M29S"
fi

# --- the command line both tiers share ---------------------------------------
# A -b whose operands look swapped -- the source missing, the destination a
# host path that is there (the harness's own scratch directory: not every
# host has a /tmp) -- is the order chroot-ng took before 0.1.0, and the
# error says which way round would have worked.
out=$(run -b "/no-such-m29-src:$ARM64EMU_NATIVE_TMP" / /no-such-program 2>&1)
check "a -b source that is not there is refused" 126 $?
check_contains "...and named" "arm64emu: --bind source path '/no-such-m29-src': not found" "$out"
check_contains "...with the swap it looks like" \
    "did you mean '$ARM64EMU_NATIVE_TMP:/no-such-m29-src'?" "$out"
