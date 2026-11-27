# The native tier's test harness (`arm64emu chroot --native`) — platform layer
# (sourced first by tests/arm64emu/chroot/native/run.sh).
#
# The suite has to run on three hosts, and every difference between them is
# resolved here rather than inside the milestone scripts:
#
#   Linux x86_64      the AArch64 cross build (build/arm64emu-a64) and every
#                     AArch64 guest run under this host's arm64emu, in its
#                     chroot mode with the host's own tree as the rootfs. That
#                     mode emulates a guest's seccomp filter, RET_TRAP and
#                     SIGSYS included, so the SIGSYS tier is LIVE there as on
#                     real hardware.
#   Linux aarch64     native: build/arm64emu itself, no emulator, and the
#                     seccomp/SIGSYS tier is LIVE.
#   Termux (Android)  native as well, but the guest toolchain is bionic clang:
#                     -static-pie may not link, so compiled guests can be
#                     dynamic and then need the platform linker's directories
#                     bound into a synthetic rootfs.
#
# What the milestone scripts get from here:
#
#   run / run_t          run `$ARM64EMU_NATIVE_BIN chroot --native`, optionally
#                        under a timeout
#   emu / emu_t          run any other AArch64 program the same way
#   guest_cc OUT SRC     build a tests/arm64emu/chroot/native/guests/*.c program; rc 2 = no toolchain
#   ARM64EMU_NATIVE_GUEST_BINDS          -b args a dynamic guest needs inside a fresh rootfs
#   GUEST_LD_DESC        human name of the link mode in use
#   ARM64EMU_NATIVE_SECCOMP_LIVE     1 when the in-process monitor really traps syscalls
#                        (the suite stops where it does not)
#   ARM64EMU_NATIVE_EXECMEM          1 when anon RW->RX memory is permitted
#   ARM64EMU_NATIVE_GUEST_XLATE      1 when a compiled guest's own path syscalls get
#                        translated here (see guest_xlate_ready)
#   ARM64EMU_NATIVE_ALPINE/_DEBIAN/_ORACLE  rootfs + differential-oracle locations
#   cng_rootfs NAME fetch  provision a pinned rootfs image on demand
#   skip MSG             uniform, counted "  skip <msg>" reporting
#   elf_type / elf_machine / elf_has_interp   ELF facts without file(1)
#
# Overridable from the environment: ARM64EMU_NATIVE_BIN, ARM64EMU_NATIVE_EMU (set empty to force direct
# execution), ARM64EMU_NATIVE_GUESTCC, ARM64EMU_NATIVE_GUESTLD, ARM64EMU_NATIVE_GUEST_BINDS, ARM64EMU_NATIVE_ROOTFS_DIR, ARM64EMU_NATIVE_ROOTFS_FETCH,
# ARM64EMU_NATIVE_ALPINE, ARM64EMU_NATIVE_DEBIAN, ARM64EMU_NATIVE_ORACLE, ARM64EMU_NATIVE_SYSROOT. See docs/arm64emu/chroot/native-testing.md.

pass=${pass:-0}
fail=${fail:-0}
skipped=${skipped:-0}

# The arm64emu that runs --native: this host's own on AArch64, the AArch64
# cross build elsewhere (`make test-arm64emu-chroot-native` sets it). Any
# other build of it will do (ARM64EMU_NATIVE_BIN=build/arm64emu-embed).
case "$(uname -m 2>/dev/null)" in
aarch64 | arm64) ARM64EMU_NATIVE_BIN="${ARM64EMU_NATIVE_BIN:-$PWD/build/arm64emu}" ;;
*) ARM64EMU_NATIVE_BIN="${ARM64EMU_NATIVE_BIN:-$PWD/build/arm64emu-a64}" ;;
esac

# A run of the tier that cannot be one -- a probe that finds the filter not
# applied, say -- is an error here, not the emulator's run in its place: no
# row of this suite may pass on the emulator's answers. The rows that test the
# fallback itself clear it.
ARM64EMU_NATIVE_NO_FALLBACK=1
export ARM64EMU_NATIVE_NO_FALLBACK

have() { command -v "$1" >/dev/null 2>&1; }

# The host's sh by a name the host kernel can follow from a #! line. Not
# /bin/sh: Android had no /bin before 10, and termux-exec's rewrite of the name
# reaches only the first level of a script whose interpreter is a script.
HOST_SH=$(command -v sh)

# skip DESC — a leg that cannot be exercised on this host. Counted so the
# summary never reads like full coverage when half the suite sat out.
skip() {
    skipped=$((skipped + 1))
    printf '  skip %s\n' "$1"
}

# ---- running AArch64 programs ---------------------------------------------
# On an AArch64 host $ARM64EMU_NATIVE_EMU is empty and everything runs directly;
# elsewhere it is an arm64emu, and the program runs in its chroot mode with the
# host's tree as the rootfs, the way a binfmt handler would run it:
#
#   - the JIT, which keeps a suite of a few thousand runs affordable;
#   - --keep-fds, since a row may hand the program descriptors above 2 (a
#     socket to import, a pipe to report on), and chroot mode closes them;
#   - --shared-proc, so every run sees the others' processes as one host's
#     processes see each other: chroot mode otherwise hides whatever is not
#     of its own session, and rows start helpers (signal holders, a second
#     invocation sharing a registry) as runs of their own;
#   - the shell's working directory, which chroot mode would otherwise
#     replace with /;
#   - the whole environment, which chroot mode builds fresh. The native tier's
#     knobs and the variables a row sets for a guest arrive through it, so
#     every exported variable is forwarded with -E, the TERM and PATH chroot
#     mode would choose for itself included; and it leaves the emulator's own
#     environment, whose knobs are the same ARM64EMU_* names: a row's
#     ARM64EMU_TERMIOS2_FORCE_DENY is the tier's, not the emulator's under it.
#
# Kept as functions rather than a variable so an empty $ARM64EMU_NATIVE_EMU never
# becomes an empty argv[0].

# emu_t SECS CMD... — run an AArch64 program, bounded by SECS seconds when SECS
# is not empty: a test that blocks (a netlink dump that never terminates, a
# socket that never accepts) should fail one check rather than wedge the suite.
emu_t() {
    _emu_t=$1
    shift
    if [ -n "$ARM64EMU_NATIVE_EMU" ]; then
        # Forwarded in the order the names come, after the command, then the
        # command rotated back behind them: positional parameters are the one
        # array a POSIX shell has. A name the shell cannot spell is not one a
        # row set, and is left behind.
        _emu_n=$#
        for _emu_v in $(awk 'BEGIN { for (k in ENVIRON) if (k ~ /^[A-Za-z_][A-Za-z0-9_]*$/) print k }'); do
            eval "set -- \"\$@\" -E \"$_emu_v=\${$_emu_v-}\""
        done
        set -- "$@" /
        while [ "$_emu_n" -gt 0 ]; do
            set -- "$@" "$1"
            shift
            _emu_n=$((_emu_n - 1))
        done
        set -- "$ARM64EMU_NATIVE_EMU" chroot --jit --keep-fds --shared-proc -w "$PWD" "$@"
        for _emu_v in $(awk 'BEGIN { for (k in ENVIRON) if (k ~ /^ARM64EMU_[A-Za-z0-9_]*$/) print k }'); do
            set -- -u "$_emu_v" "$@"
        done
        set -- env "$@"
    fi
    # KILL, not TERM: at the deadline it goes to the whole process group. A
    # guest that has every signal blocked (a ptrace row's tracee) outlives a
    # TERM that ended its parent, and holds the row's output pipe open: the
    # substitution reading it never ends, nor does the suite.
    if [ -n "$_emu_t" ] && [ -n "$TIMEOUT" ]; then
        "$TIMEOUT" -s KILL "$_emu_t" "$@"
    else
        "$@"
    fi
}

emu() { emu_t "" "$@"; }

# kref CMD... — run a host-built reference, the stand-in for "what the kernel
# answers" in a differential. Termux preloads libtermux-exec into every process,
# and it wraps execve: the exec probes of a reference run answered EACCES for a
# path the kernel calls ENOTDIR or ENOENT, which is the shim's verdict and not
# the kernel's. A guest under the native tier never carries the preload (its
# environment is the one it was given), so the reference is run without it.
kref() { env -u LD_PRELOAD "$@"; }

run() { emu "$ARM64EMU_NATIVE_BIN" chroot --native "$@"; }
run_t() {
    _t=$1
    shift
    emu_t "$_t" "$ARM64EMU_NATIVE_BIN" chroot --native "$@"
}

# ---- ELF facts, without file(1) (absent on a bare Termux) ------------------
# e_type at offset 16, e_machine at 18, both 2-byte little-endian; od's native
# byte order matches on every host we target (x86_64 / AArch64 LE).

elf_u16() { od -An -tu2 -j"$2" -N2 "$1" 2>/dev/null | tr -d ' \n'; }

# elf_type FILE -> EXEC | DYN | other-numeric | empty
elf_type() {
    case "$(elf_u16 "$1" 16)" in
    2) echo EXEC ;;
    3) echo DYN ;;
    "") echo "" ;;
    *) elf_u16 "$1" 16 ;;
    esac
}

# elf_machine FILE -> e_machine (183 == EM_AARCH64)
elf_machine() { elf_u16 "$1" 18; }

elf_is_aarch64() { [ "$(elf_machine "$1")" = 183 ]; }

# elf_has_interp FILE — true for a dynamically linked executable. readelf when
# there is one; otherwise the .interp section name, which only appears in the
# section-header string table of an object that has the segment.
elf_has_interp() {
    for _re in readelf llvm-readelf aarch64-linux-gnu-readelf; do
        if have "$_re"; then
            "$_re" -l "$1" 2>/dev/null | grep -q INTERP
            return $?
        fi
    done
    LC_ALL=C grep -aq '\.interp' "$1"
}

# ---- platform detection ----------------------------------------------------

cng_detect_host() {
    HOST_ARCH=$(uname -m 2>/dev/null || echo unknown)
    case "$HOST_ARCH" in
    aarch64 | arm64) ARM64EMU_NATIVE_A64HOST=1 ;;
    *) ARM64EMU_NATIVE_A64HOST=0 ;;
    esac

    ARM64EMU_NATIVE_TERMUX=0
    if [ -n "${TERMUX_VERSION:-}" ] || [ -d /data/data/com.termux/files/usr ]; then
        ARM64EMU_NATIVE_TERMUX=1
    fi
    case "${PREFIX:-}" in
    */com.termux/*) ARM64EMU_NATIVE_TERMUX=1 ;;
    esac

    # ARM64EMU_NATIVE_EMU: an explicit setting always wins, INCLUDING an empty one —
    # that is how to force direct execution on a cross host that runs AArch64
    # binaries some other way (a binfmt_misc handler). Otherwise a foreign host
    # takes the arm64emu this repository builds (`make
    # test-arm64emu-chroot-native` builds it), by an absolute name, since rows
    # change directory.
    ARM64EMU_NATIVE_EMU_MISSING=0
    if [ "${ARM64EMU_NATIVE_EMU+x}" != x ]; then
        ARM64EMU_NATIVE_EMU=
        if [ "$ARM64EMU_NATIVE_A64HOST" = 0 ]; then
            if [ -x "$PWD/build/arm64emu" ]; then
                ARM64EMU_NATIVE_EMU=$PWD/build/arm64emu
            else
                ARM64EMU_NATIVE_EMU_MISSING=1
            fi
        fi
    fi

    TIMEOUT=
    have timeout && TIMEOUT=timeout
}

# ---- guest toolchain ------------------------------------------------------
# The milestone scripts build small AArch64 programs (tests/arm64emu/chroot/native/guests/*.c) to drive
# the native tier as a real guest would. Which compiler that is, and whether it can
# link statically, is entirely host-dependent — so probe rather than assume, and
# prove the result by running it.

cng_detect_guestcc() {
    # Every one of these has to be defined before the first early return: the
    # milestone scripts run under `set -u` and read them whether or not a
    # toolchain turned up. Note the explicitness of ARM64EMU_NATIVE_GUESTLD is captured before it
    # is defaulted, since "set to empty" means "dynamic, no fallback".
    if [ "${ARM64EMU_NATIVE_GUESTLD+x}" = x ]; then _ld_explicit=1; else _ld_explicit=0; fi
    ARM64EMU_NATIVE_GUESTLD=${ARM64EMU_NATIVE_GUESTLD-}
    GUEST_STATIC=0
    ARM64EMU_NATIVE_GUEST_BINDS=${ARM64EMU_NATIVE_GUEST_BINDS:-}
    GUEST_LD_DESC=none
    GUEST_CC_LOG=$ARM64EMU_NATIVE_TMP/guest_cc.log

    if [ -n "${ARM64EMU_NATIVE_GUESTCC:-}" ]; then
        have "$ARM64EMU_NATIVE_GUESTCC" || {
            ARM64EMU_NATIVE_GUESTCC=
            return 1
        }
    else
        # A native compiler is only a candidate on an AArch64 host; on x86_64 an
        # unprefixed gcc would happily build an x86 binary and every guest test
        # would then fail for the wrong reason.
        if [ "$ARM64EMU_NATIVE_A64HOST" = 1 ]; then
            set -- aarch64-linux-gnu-gcc-13 aarch64-linux-gnu-gcc cc gcc clang
        else
            set -- aarch64-linux-gnu-gcc-13 aarch64-linux-gnu-gcc-12 \
                aarch64-linux-gnu-gcc aarch64-linux-musl-gcc
        fi
        ARM64EMU_NATIVE_GUESTCC=
        for _c in "$@"; do
            if have "$_c"; then
                ARM64EMU_NATIVE_GUESTCC=$_c
                break
            fi
        done
        [ -n "$ARM64EMU_NATIVE_GUESTCC" ] || return 1
    fi

    printf 'int main(void){return 7;}\n' >"$ARM64EMU_NATIVE_TMP/ldprobe.c"
    # Most of the suite needs a self-contained guest it can drop into a synthetic
    # rootfs, so prefer static-PIE, then plain static, and only fall back to
    # dynamic (Termux/bionic, where there is no libc.a to link against). An
    # explicit ARM64EMU_NATIVE_GUESTLD is honoured exactly, with no fallback.
    if [ "$_ld_explicit" = 1 ]; then
        cng_try_guest_ld "$ARM64EMU_NATIVE_GUESTLD" && return 0
        ARM64EMU_NATIVE_GUESTCC=
        ARM64EMU_NATIVE_GUESTLD=
        return 1
    fi
    for _m in -static-pie -static ""; do
        cng_try_guest_ld "$_m" && return 0
    done
    ARM64EMU_NATIVE_GUESTCC=
    ARM64EMU_NATIVE_GUESTLD=
    return 1
}

# cng_try_guest_ld MODE — can $ARM64EMU_NATIVE_GUESTCC build an AArch64 binary this way, and does
# the result actually run here? Sets ARM64EMU_NATIVE_GUESTLD/GUEST_STATIC/GUEST_LD_DESC on yes.
cng_try_guest_ld() {
    rm -f "$ARM64EMU_NATIVE_TMP/ldprobe"
    # shellcheck disable=SC2086  # $1 is a single flag or deliberately empty
    $ARM64EMU_NATIVE_GUESTCC $1 -O0 -o "$ARM64EMU_NATIVE_TMP/ldprobe" "$ARM64EMU_NATIVE_TMP/ldprobe.c" \
        2>"$GUEST_CC_LOG" || return 1
    elf_is_aarch64 "$ARM64EMU_NATIVE_TMP/ldprobe" || return 1
    emu_t 30 "$ARM64EMU_NATIVE_TMP/ldprobe" >/dev/null 2>&1
    [ $? = 7 ] || return 1
    ARM64EMU_NATIVE_GUESTLD=$1
    case "$1" in
    "")
        GUEST_STATIC=0
        GUEST_LD_DESC=dynamic
        ;;
    -static-pie)
        GUEST_STATIC=1
        GUEST_LD_DESC=static-pie
        ;;
    *)
        GUEST_STATIC=1
        GUEST_LD_DESC=$1
        ;;
    esac
    [ "$GUEST_STATIC" = 1 ] || cng_guest_binds
    return 0
}

# A dynamically linked guest resolves its ELF interpreter and libraries through
# the guest view, so a synthetic rootfs holding nothing but the binary cannot
# start it — the native tier reports "cannot load interpreter <rootfs>/lib/ld-...".
# Expose the host directories the loader needs at the same guest paths. (Only for
# rootfs the suite builds itself — never for a real Alpine/Debian tree, which
# ships its own.) Preset ARM64EMU_NATIVE_GUEST_BINDS in the environment for an unusual layout.
#
# Bionic's linker reads nothing under /lib, and binding it anyway is not
# harmless: an Android ramdisk may carry a /lib of its own (/lib/modules on
# Android 8), and the bind then hides the test rootfs's /lib — M19's
# lib/apk/exec among it. So an Android host binds Android's directories only.
cng_guest_binds() {
    [ -z "${ARM64EMU_NATIVE_GUEST_BINDS:-}" ] || return 0
    ARM64EMU_NATIVE_GUEST_BINDS=
    if [ -e /system/bin/linker64 ]; then
        set -- /system /apex /linkerconfig /vendor
    else
        set -- /lib /lib64 /usr/lib /usr/lib64
    fi
    for _d in "$@" ${PREFIX:+"$PREFIX"}; do
        [ -d "$_d" ] || continue
        ARM64EMU_NATIVE_GUEST_BINDS="$ARM64EMU_NATIVE_GUEST_BINDS -b $_d:$_d"
    done
}

# cng_outside_binds DIR — where a scratch tree may be put in front of the
# arm64emu oracle. The oracle spells a guest path with the longest HOST prefix
# it finds, and a rootfs that sits inside a directory $ARM64EMU_NATIVE_GUEST_BINDS exposes is then
# spelled as the bind: Termux's $TMPDIR is inside $PREFIX, which is bound for the
# linker, so the oracle's getcwd answered "/data/data/.../tmp.X/a/b" for a guest
# whose cwd is /a/b — the native tier answers /a/b there, as the kernel's chroot would,
# so the reference is the one at fault. Prints DIR itself when no bind contains it;
# otherwise DIR is moved under $HOME, where none does, and the new name is
# printed. Callers re-point their variable and clean up as before.
cng_outside_binds() {
    _real=$(cd "$1" 2>/dev/null && pwd -P) || {
        printf '%s\n' "$1"
        return 0
    }
    _in=0
    for _w in $ARM64EMU_NATIVE_GUEST_BINDS; do
        [ "$_w" = -b ] && continue
        case "$_real/" in "${_w#*:}"/*) _in=1 ;; esac
    done
    if [ "$_in" = 1 ] && _to=$(mktemp -d "${HOME:-/}/cng-unbound.XXXXXX" 2>/dev/null) &&
        rmdir "$_to" && mv "$1" "$_to"; then
        printf '%s\n' "$_to"
        return 0
    fi
    printf '%s\n' "$1"
}

# cng_dyn_binds — the same directories, for a guest that is dynamic even though
# this host's probed link mode is static (M23 builds one deliberately, since the
# noexec .so path only exists for a guest whose OWN ld.so maps libraries). On a
# cross host the AArch64 interpreter and libc are under the toolchain sysroot
# rather than at the host's own /lib, so they are exposed at the guest paths the
# binary names instead of at their host ones -- the interpreter the loader
# resolves through the guest view among them. Sets GUEST_DYN_BINDS.
cng_dyn_binds() {
    GUEST_DYN_BINDS=
    if [ "$ARM64EMU_NATIVE_A64HOST" = 1 ]; then _sr=${ARM64EMU_NATIVE_SYSROOT-}; else
        _sr=${ARM64EMU_NATIVE_SYSROOT-/usr/aarch64-linux-gnu}
    fi
    if [ -n "$_sr" ]; then
        for _d in lib lib64 usr/lib usr/lib64; do
            [ -d "$_sr/$_d" ] || continue
            GUEST_DYN_BINDS="$GUEST_DYN_BINDS -b $_sr/$_d:/$_d"
        done
    else
        for _d in /system /apex /linkerconfig /vendor /lib /lib64 /usr/lib \
            /usr/lib64 ${PREFIX:+"$PREFIX"}; do
            [ -d "$_d" ] || continue
            GUEST_DYN_BINDS="$GUEST_DYN_BINDS -b $_d:$_d"
        done
    fi
}

# guest_cc OUT SRC [flags...] — build an AArch64 guest program.
#   rc 0   built
#   rc 1   build failed (diagnostics in $GUEST_CC_LOG) — a real failure
#   rc 2   no usable guest toolchain on this host — the caller should skip
guest_cc() {
    [ -n "$ARM64EMU_NATIVE_GUESTCC" ] || return 2
    _out=$1
    _src=$2
    shift 2
    # shellcheck disable=SC2086  # $ARM64EMU_NATIVE_GUESTLD is a flag or deliberately empty
    $ARM64EMU_NATIVE_GUESTCC $ARM64EMU_NATIVE_GUESTLD -O2 -o "$_out" "$_src" "$@" 2>"$GUEST_CC_LOG"
}

# guest_xlate_ready WHAT — can this host observe a compiled guest's OWN path
# syscalls being translated at all? Emits the reason and returns 1 if not, so a
# milestone that depends on it says which of the two prerequisites is missing.
guest_xlate_ready() {
    if [ -z "$ARM64EMU_NATIVE_GUESTCC" ]; then
        skip "$1: no AArch64 guest toolchain"
        return 1
    fi
    if [ "$ARM64EMU_NATIVE_GUEST_XLATE" = 0 ]; then
        skip "$1: the seccomp tier is inert here"
        return 1
    fi
    return 0
}

# guest_cc_report OUT SRC — build, and on failure emit the right diagnosis:
# "skip" where no toolchain exists, a counted FAIL where one does and broke.
guest_cc_report() {
    guest_cc "$@"
    _rc=$?
    case $_rc in
    0) return 0 ;;
    2) skip "no AArch64 guest toolchain: cannot build $2" ;;
    *)
        fail=$((fail + 1))
        printf '  FAIL could not build %s\n' "$2"
        sed 's/^/    /' "$GUEST_CC_LOG" 2>/dev/null | head -8
        ;;
    esac
    return 1
}

# ---- the native tier's capabilities on THIS host -----------------------
# The seccomp filter is the native tier's whole interception: a host that does not
# apply a guest's filter (qemu-user) cannot run it at all, so whether the tier
# is live is checked, not assumed -- a compiled guest's open of a file that is
# only in a scratch rootfs has to find it there. ARM64EMU_NATIVE_SECCOMP_LIVE is that
# answer (the suite stops on a 0: run.sh), unknown and taken as live where no
# guest can be built to ask. ARM64EMU_NATIVE_EXECMEM is whether the host lets anonymous
# memory become executable, asked of the host with no arm64emu in the way
# (guests/execmem.c): the loader's anonymous strategy needs it.

cng_probe_caps() {
    ARM64EMU_NATIVE_SECCOMP_LIVE=1
    ARM64EMU_NATIVE_EXECMEM=0
    _pr=$ARM64EMU_NATIVE_TMP/probe
    mkdir -p "$_pr/root/etc" "$_pr/root/bin"
    if guest_cc "$_pr/root/bin/readfile" tests/arm64emu/chroot/native/guests/readfile.c; then
        printf 'cng-live\n' >"$_pr/root/etc/greeting"
        # shellcheck disable=SC2086  # $ARM64EMU_NATIVE_GUEST_BINDS is a deliberately split arg list
        case "$(run_t 60 $ARM64EMU_NATIVE_GUEST_BINDS "$_pr/root" /bin/readfile 2>/dev/null)" in
        cng-live) ;;
        *) ARM64EMU_NATIVE_SECCOMP_LIVE=0 ;;
        esac
    fi
    if guest_cc "$_pr/execmem" tests/arm64emu/chroot/native/guests/execmem.c; then
        emu_t 30 "$_pr/execmem" >/dev/null 2>&1
        [ $? = 7 ] && ARM64EMU_NATIVE_EXECMEM=1
    fi
    rm -rf "$_pr"
}

# ---- guest rootfs images + the differential oracle ------------------------
# Several milestones need a real AArch64 userland (Alpine busybox, Debian glibc)
# and M10 needs the arm64emu oracle. The userlands are pinned images the
# suite provisions itself under $ARM64EMU_NATIVE_ROOTFS_DIR (default build/cache/arm64emu/chroot/native/rootfs):
# Alpine at startup, since seven milestones use it, and Debian only when M10
# reaches its one leg, which needs the oracle as well. The oracle is another
# project and cannot be fetched, so it is searched for. A milestone left
# without an image or the oracle skips cleanly.

# cng_rootfs_pin NAME — the image NAME is provisioned from: _rf_file (the
# tarball's name under downloads/), _rf_url, _rf_sha (its sha256) and
# _rf_marker (the program that makes a tree usable). Bumping an image is an
# edit here and nothing else: a provisioned tree is stamped with the hash it
# came from, and one stamped with another is replaced on the next run.
cng_rootfs_pin() {
    case "$1" in
    alpine)
        # The stock minirootfs: the suite only ever calls bin/busybox in it.
        _rf_file=alpine-minirootfs-3.20.3-aarch64.tar.gz
        _rf_url=https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/aarch64/$_rf_file
        _rf_sha=041fa34a81788242df9e78fa69b97ab45b8ec47ddbf88864755610414a7bf3de
        _rf_marker=bin/busybox
        ;;
    debian)
        # Debian publishes no minirootfs. The official debian:trixie-20261020-slim
        # image is a single layer per platform, and Docker Hub serves a layer by
        # its own sha256: the linux/arm64/v8 one's digest is pin and checksum.
        _rf_file=debian-trixie-20261020-slim-arm64.tar.gz
        _rf_sha=ddc0c1e36a6793411b6c8d48e1ddad2987405d377da92bcf532ed3181c2e4d62
        _rf_url=https://registry-1.docker.io/v2/library/debian/blobs/sha256:$_rf_sha
        _rf_marker=bin/ls
        ;;
    *) return 1 ;;
    esac
}

# cng_get URL OUT [HEADER] — download URL to OUT ("-" is stdout).
cng_get() {
    if have curl; then
        curl -fsSL --retry 3 --connect-timeout 30 ${3:+-H "$3"} -o "$2" "$1"
    elif have wget; then
        wget -q -T 30 ${3:+--header="$3"} -O "$2" "$1"
    else
        return 127
    fi
}

# cng_rootfs_fetch NAME OUT — download NAME's pinned tarball to OUT.
cng_rootfs_fetch() {
    case "$1" in
    debian)
        # Even an anonymous pull from Docker Hub needs a bearer token.
        _tok=$(cng_get "https://auth.docker.io/token?service=registry.docker.io&scope=repository:library/debian:pull" - |
            sed -n 's/.*"token" *: *"\([^"]*\)".*/\1/p')
        [ -n "$_tok" ] && cng_get "$_rf_url" "$2" "Authorization: Bearer $_tok"
        ;;
    *) cng_get "$_rf_url" "$2" ;;
    esac
}

cng_sha256() { sha256sum "$1" 2>/dev/null | cut -d' ' -f1; }

# cng_rootfs_lock NAME — runs sharing $ARM64EMU_NATIVE_ROOTFS_DIR take turns provisioning.
# mkdir is the one atomic test-and-set every host has; a lock whose owner has
# died is taken over. Released by removing the directory.
cng_rootfs_lock() {
    _lk=$ARM64EMU_NATIVE_ROOTFS_DIR/.$1.lock
    _n=0
    until mkdir "$_lk" 2>/dev/null; do
        _p=$(cat "$_lk/pid" 2>/dev/null)
        if [ -n "$_p" ] && ! kill -0 "$_p" 2>/dev/null; then
            rm -rf "$_lk"
            continue
        fi
        [ "$_n" = 0 ] && echo "  waiting for another run to provision $1" >&2
        _n=$((_n + 1))
        [ "$_n" -le 900 ] || return 1
        sleep 1
    done
    echo $$ >"$_lk/pid"
}

# cng_rootfs NAME [fetch] — ARM64EMU_NATIVE_ROOTFS is set to $ARM64EMU_NATIVE_ROOTFS_DIR/NAME when that
# tree is usable, otherwise emptied with the reason in ARM64EMU_NATIVE_ROOTFS_WHY. A tree is
# usable once it has its marker and is stamped with the pinned hash, or carries
# no stamp at all: one staged by hand is used as it is and never touched. With
# "fetch", a missing or stale tree is provisioned.
cng_rootfs() {
    ARM64EMU_NATIVE_ROOTFS=
    ARM64EMU_NATIVE_ROOTFS_WHY=
    cng_rootfs_pin "$1" || return 1
    _rf_dir=$ARM64EMU_NATIVE_ROOTFS_DIR/$1
    _rf_stamp=$ARM64EMU_NATIVE_ROOTFS_DIR/.$1.sha256
    cng_rootfs_usable && return 0
    if [ "${2:-}" != fetch ]; then
        ARM64EMU_NATIVE_ROOTFS_WHY="not provisioned yet"
        return 1
    fi
    if ! mkdir -p "$ARM64EMU_NATIVE_ROOTFS_DIR/downloads" || ! cng_rootfs_lock "$1"; then
        ARM64EMU_NATIVE_ROOTFS_WHY="cannot lock $ARM64EMU_NATIVE_ROOTFS_DIR for provisioning"
        return 1
    fi
    cng_rootfs_provision "$1"
    _rc=$?
    rm -rf "$ARM64EMU_NATIVE_ROOTFS_DIR/.$1.lock"
    return $_rc
}

cng_rootfs_usable() {
    [ -x "$_rf_dir/$_rf_marker" ] || return 1
    [ ! -e "$_rf_stamp" ] || [ "$(cat "$_rf_stamp")" = "$_rf_sha" ] || return 1
    ARM64EMU_NATIVE_ROOTFS=$_rf_dir
}

# cng_rootfs_provision NAME — under the lock. The tarball in downloads/ is
# downloaded first when absent (never with ARM64EMU_NATIVE_ROOTFS_FETCH=0) and checked
# against the pin either way. It is unpacked beside the tree and renamed into
# place, so a run that dies midway leaves nothing half-written under the name.
cng_rootfs_provision() {
    cng_rootfs_usable && return 0 # another run got there first
    if [ -e "$_rf_dir" ] && [ ! -e "$_rf_stamp" ]; then
        ARM64EMU_NATIVE_ROOTFS_WHY="$_rf_dir has no $_rf_marker and was not provisioned here"
        return 1
    fi
    if ! have sha256sum; then
        ARM64EMU_NATIVE_ROOTFS_WHY="no sha256sum to check $_rf_file with"
        return 1
    fi
    _tb=$ARM64EMU_NATIVE_ROOTFS_DIR/downloads/$_rf_file
    if [ "$(cng_sha256 "$_tb")" != "$_rf_sha" ]; then
        if [ "${ARM64EMU_NATIVE_ROOTFS_FETCH:-1}" = 0 ]; then
            ARM64EMU_NATIVE_ROOTFS_WHY="no $_rf_file matching its pin in downloads/, and ARM64EMU_NATIVE_ROOTFS_FETCH=0"
            return 1
        fi
        echo "  fetching $_rf_file" >&2
        if ! cng_rootfs_fetch "$1" "$_tb.part"; then
            rm -f "$_tb.part"
            ARM64EMU_NATIVE_ROOTFS_WHY="cannot download $_rf_url"
            return 1
        fi
        if [ "$(cng_sha256 "$_tb.part")" != "$_rf_sha" ]; then
            rm -f "$_tb.part"
            ARM64EMU_NATIVE_ROOTFS_WHY="$_rf_file does not match its pinned sha256"
            return 1
        fi
        if ! mv -f "$_tb.part" "$_tb"; then
            ARM64EMU_NATIVE_ROOTFS_WHY="cannot store $_tb"
            return 1
        fi
    fi
    _new=$ARM64EMU_NATIVE_ROOTFS_DIR/.$1.new
    rm -rf "$_new"
    if ! mkdir "$_new" || ! (cd "$_new" && gzip -dc "$_tb" | tar -xof -) ||
        [ ! -x "$_new/$_rf_marker" ]; then
        rm -rf "$_new"
        ARM64EMU_NATIVE_ROOTFS_WHY="cannot unpack $_rf_file"
        return 1
    fi
    # Only a tree stamped as provisioned here gets this far to be replaced.
    rm -rf "$_rf_dir"
    if [ -e "$_rf_dir" ]; then
        rm -rf "$_new"
        ARM64EMU_NATIVE_ROOTFS_WHY="cannot remove the stale $_rf_dir"
        return 1
    fi
    if ! echo "$_rf_sha" >"$_rf_stamp" || ! mv "$_new" "$_rf_dir"; then
        rm -rf "$_new"
        ARM64EMU_NATIVE_ROOTFS_WHY="cannot move the unpacked tree to $_rf_dir"
        return 1
    fi
    ARM64EMU_NATIVE_ROOTFS=$_rf_dir
}

cng_detect_images() {
    # Absolute, since the provisioning and the milestones change directory.
    ARM64EMU_NATIVE_ROOTFS_DIR="${ARM64EMU_NATIVE_ROOTFS_DIR:-build/cache/arm64emu/chroot/native/rootfs}"
    case "$ARM64EMU_NATIVE_ROOTFS_DIR" in
    /*) : ;;
    *) ARM64EMU_NATIVE_ROOTFS_DIR=$PWD/$ARM64EMU_NATIVE_ROOTFS_DIR ;;
    esac
    ARM64EMU_NATIVE_ALPINE="${ARM64EMU_NATIVE_ALPINE:-}"
    ARM64EMU_NATIVE_ALPINE_WHY=
    if [ -z "$ARM64EMU_NATIVE_ALPINE" ]; then
        cng_rootfs alpine fetch
        ARM64EMU_NATIVE_ALPINE=$ARM64EMU_NATIVE_ROOTFS
        ARM64EMU_NATIVE_ALPINE_WHY=$ARM64EMU_NATIVE_ROOTFS_WHY
    fi
    # Only looked for here: M10 provisions it when it reaches the Debian leg.
    ARM64EMU_NATIVE_DEBIAN="${ARM64EMU_NATIVE_DEBIAN:-}"
    ARM64EMU_NATIVE_DEBIAN_WHY=
    if [ -z "$ARM64EMU_NATIVE_DEBIAN" ]; then
        cng_rootfs debian
        ARM64EMU_NATIVE_DEBIAN=$ARM64EMU_NATIVE_ROOTFS
        ARM64EMU_NATIVE_DEBIAN_WHY=$ARM64EMU_NATIVE_ROOTFS_WHY
    fi

    # The oracle is a host-native emulator, not an AArch64 program: usable only
    # where its own machine matches this host. It is the arm64emu this
    # repository builds (`make arm64emu`; `make test-arm64emu-chroot-native` builds it), run
    # in its chroot mode: `$ARM64EMU_NATIVE_ORACLE chroot [options] <rootfs> ...`.
    ARM64EMU_NATIVE_ORACLE="${ARM64EMU_NATIVE_ORACLE:-}"
    if [ -z "$ARM64EMU_NATIVE_ORACLE" ] && [ -x "$PWD/build/arm64emu" ]; then
        ARM64EMU_NATIVE_ORACLE=$PWD/build/arm64emu
    fi
    if [ -n "$ARM64EMU_NATIVE_ORACLE" ]; then
        _m=$(elf_machine "$ARM64EMU_NATIVE_ORACLE")
        case "$HOST_ARCH:$_m" in
        x86_64:62 | aarch64:183 | arm64:183) : ;;
        *) ARM64EMU_NATIVE_ORACLE= ;;
        esac
    fi
}

# ---- init ------------------------------------------------------------------

cng_platform_init() {
    ARM64EMU_NATIVE_TMP=$(mktemp -d)
    cng_detect_host
    cng_detect_guestcc || :
    cng_probe_caps
    cng_detect_images

    # A compiled guest's own path syscalls are translated wherever the tier
    # is live, which the suite requires (run.sh).
    ARM64EMU_NATIVE_GUEST_XLATE=$ARM64EMU_NATIVE_SECCOMP_LIVE
}

cng_platform_banner() {
    printf '== platform ==\n'
    printf '  host          %s%s\n' "$HOST_ARCH" \
        "$([ "$ARM64EMU_NATIVE_TERMUX" = 1 ] && echo ' (Termux/Android)')"
    printf '  native tier   %s chroot --native\n' "$ARM64EMU_NATIVE_BIN"
    printf '  exec          %s\n' \
        "$([ -n "$ARM64EMU_NATIVE_EMU" ] && echo "$ARM64EMU_NATIVE_EMU chroot" || echo 'native (no emulator)')"
    printf '  seccomp tier  %s\n' \
        "$([ "$ARM64EMU_NATIVE_SECCOMP_LIVE" = 1 ] && echo 'live' || echo 'INERT')"
    printf '  execmem       %s\n' \
        "$([ "$ARM64EMU_NATIVE_EXECMEM" = 1 ] && echo 'permitted' || echo 'DENIED')"
    printf '  guest cc      %s\n' \
        "$([ -n "$ARM64EMU_NATIVE_GUESTCC" ] && echo "$ARM64EMU_NATIVE_GUESTCC ($GUEST_LD_DESC)" ||
            echo 'none — guest-binary legs will skip')"
    printf '  alpine rootfs %s\n' "${ARM64EMU_NATIVE_ALPINE:-none ($ARM64EMU_NATIVE_ALPINE_WHY)}"
    printf '  debian rootfs %s\n' "${ARM64EMU_NATIVE_DEBIAN:-none ($ARM64EMU_NATIVE_DEBIAN_WHY)}"
    printf '  l2s oracle    %s\n' "${ARM64EMU_NATIVE_ORACLE:-none}"
}
