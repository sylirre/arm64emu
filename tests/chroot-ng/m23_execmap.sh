# M23: mmap(PROT_EXEC) of a file the host refuses (sourced by tests/chroot-ng/run.sh).
#
# chroot-ng's loader defeats a noexec mount by reading a guest image with pread
# and mapping it anonymously — but a dynamic guest's libraries are mapped by the
# guest's OWN ld.so, which asks for PROT_EXEC straight from the file. On a true
# MNT_NOEXEC mount the kernel refuses that and every dynamically linked guest
# dies in its interpreter. The dispatcher answers the refusal with the same
# anonymous copy the loader would have made (execmap.c).
#
# No host here has a true noexec mount to offer, so the path is forced with
# CHROOTNG_MMAP_FORCE_ANON=1: take the anonymous route without asking the kernel
# first. Same testing convention as CHROOTNG_SHM_FORCE_FILE / CHROOTNG_PROCREG_NONE.
echo "== M23: mmap(PROT_EXEC) served from anonymous memory =="

# What lies past the end of the file. A file mapping longer than its file
# answers a fault on any page wholly past EOF with SIGBUS, and a truncated
# object dies there; the anonymous copy pread what the file had and left the
# rest zero, so the same object ran on -- into pages of `udf #0` for code, and
# into zeroes with no word said for data. The tail is put back under the file
# now (a mapping without PROT_EXEC is one no mount refuses). A differential
# against the kernel's own mapping of the same file, page by page, from a child
# that touches each (guests/pasteof.c), the copy forced with
# CHROOTNG_MMAP_FORCE_ANON=1.
PED=$(mktemp -d)
if guest_xlate_ready "past-EOF legs" &&
    guest_cc_report "$PED/pasteof" tests/chroot-ng/guests/pasteof.c; then
    printf 'short file\n' >"$PED/short"
    pe_k=$(emu_t 60 "$PED/pasteof" "$PED/short" 2>/dev/null)
    # shellcheck disable=SC2086  # $CHROOTNG_GUEST_BINDS is a deliberately split arg list
    pe_g=$(CHROOTNG_MMAP_FORCE_ANON=1 run_t 60 $CHROOTNG_GUEST_BINDS "$PED" /pasteof /short \
        2>/dev/null)
    check_contains "the kernel's answer past EOF really is SIGBUS" \
        "page2-past-eof: sig7" "$pe_k"
    check "a copy past EOF faults page by page as the kernel's mapping does" \
        "$pe_k" "$pe_g"
fi
rm -rf "$PED"

XMR=$(mktemp -d)
mkdir -p "$XMR/bin" "$XMR/etc"
printf 'GREETING-VIA-EXECMAP' > "$XMR/etc/greeting"
cng_dyn_binds

# A DYNAMIC guest, deliberately: the harness's own link mode is static-PIE on
# most hosts, and a static guest never maps a library at all.
if [ -z "$CHROOTNG_GUESTCC" ]; then
    skip "execmap legs: no AArch64 guest toolchain"
elif ! $CHROOTNG_GUESTCC -O2 -o "$XMR/bin/readfile" tests/chroot-ng/guests/readfile.c \
    2>"$GUEST_CC_LOG"; then
    fail=$((fail + 1)); printf '  FAIL could not build a dynamic guest\n'
    sed 's/^/    /' "$GUEST_CC_LOG"
elif ! elf_has_interp "$XMR/bin/readfile"; then
    skip "execmap legs: this toolchain links even -O2 guests statically"
else
    # shellcheck disable=SC2086  # both are deliberately split arg lists
    xmrun() { run $GUEST_DYN_BINDS "$@"; }

    out=$(CHROOTNG_MMAP_FORCE_ANON=1 xmrun "$XMR" /bin/readfile 2>/dev/null); rc=$?
    check "a dynamic guest starts with every library mapped from a copy" 0 $rc
    # The guest's open() is libc's, in a library chroot-ng never loaded itself.
    # Reaching the rootfs means the copy carried the code, and the filter
    # traps the calls it makes from there.
    check_contains "...and its libc openat is translated into the rootfs" \
        "GREETING-VIA-EXECMAP" "$out"

    # The control. Without the knob the mapping is granted by the kernel and
    # stays file-backed, and the SIGSYS tier translates the open as it always
    # did.
    out=$(xmrun "$XMR" /bin/readfile 2>/dev/null); rc=$?
    check "unforced, the seccomp tier translates the same open" 0 $rc
    check_contains "seccomp-translated openat reached the rootfs" \
        "GREETING-VIA-EXECMAP" "$out"

    # The conversion is a copy of a MAP_PRIVATE file mapping, so a second run of
    # the same guest must be indistinguishable from the first — nothing about it
    # is stateful, and a half-protected page would show up as a difference
    # here.
    a=$(CHROOTNG_MMAP_FORCE_ANON=1 xmrun "$XMR" /bin/readfile 2>/dev/null)
    b=$(CHROOTNG_MMAP_FORCE_ANON=1 xmrun "$XMR" /bin/readfile 2>/dev/null)
    check "the same guest run twice answers the same" "$a" "$b"
fi
rm -rf "$XMR"

# mremap(MREMAP_DONTUNMAP) out of the loader's copy of an image. The move
# leaves the old range mapped, reading back what backs it: the file's bytes
# for the file mapping a kernel exec makes, zero-fill for the anonymous copy
# the loader makes instead. V8 moves its builtins out of the text of the
# binary that carries them and CHECKs the first word at both addresses, and
# every Chrome renderer died of it. The oracle is the kernel's own move of a
# private mapping of the same program's file, whichever answer this kernel
# gives (EINVAL before 5.13); anonymous memory must still come back zeroed.
# -F loads the image from the file, where the kernel answers by itself. The
# loader's copy is put back through /proc/self/mem; a host that refuses it
# (arm64emu does, its memory not being the guest's) leaves the copy no way to
# move, and then the answer is EINVAL, as it is before 5.13.
DUM=$(mktemp -d)
dum_rows() { # tier-desc, extra flags
    # shellcheck disable=SC2086  # $CHROOTNG_GUEST_BINDS is a deliberately split arg list
    out=$(run_t 60 $2 $CHROOTNG_GUEST_BINDS "$DUM/rootfs" /bin/dontunmap 2>/dev/null)
    case "$out" in
    *"anon r=0 "*)
        check_contains "DONTUNMAP ($1): anonymous memory moves and leaves zero-fill" \
            "anon r=0 kept=0 zeroed=1" "$out"
        _want=$(printf '%s\n' "$out" | sed -n 's/^file //p')
        case "$2:$out" in
        :*"mem r=0"*) ;;
        :*) _want="r=22" ;;   # EINVAL
        esac
        check_contains "DONTUNMAP ($1): ...a file mapping keeps its bytes, or says EINVAL" \
            "file r=" "$out"
        check_contains "DONTUNMAP ($1): the image's own pages move as that file mapping does" \
            "image ${_want:-?}" "$out"
        ;;
    *"anon r="*) skip "DONTUNMAP ($1): this kernel has no MREMAP_DONTUNMAP" ;;
    *) check_contains "DONTUNMAP ($1): the guest ran" "anon r=" "$out" ;;
    esac
}
if ! guest_xlate_ready "DONTUNMAP legs"; then
    :
elif guest_cc_report "$DUM/dontunmap" tests/chroot-ng/guests/dontunmap.c; then
    mkdir -p "$DUM/rootfs/bin"; cp "$DUM/dontunmap" "$DUM/rootfs/bin/"
    dum_rows "seccomp" ""
    dum_rows "-F" -F
fi
rm -rf "$DUM"
