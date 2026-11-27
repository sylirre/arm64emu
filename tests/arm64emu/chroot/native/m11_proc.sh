# M11: /proc emulation (sourced by tests/arm64emu/chroot/native/run.sh).
#
# Guest-shell scenarios with a real Alpine rootfs, and compiled guests: they are
# the ones that would catch a file describing arm64emu instead of the guest,
# since a shell reads them the way a real tool does. Unlike M10 this is not differential against arm64emu — the sibling
# emulator synthesizes a plausibly different mount table (its own pseudo-mount
# rows and mount IDs), so the assertions are structural instead.
echo "== M11: /proc emulation =="

M11_ALPINE="${M11_ALPINE:-$ARM64EMU_NATIVE_ALPINE}"


# --- maps lines longer than the line reader ---------------------------------
# put_maps re-parses every chunk the line reader hands it, and a line longer
# than that reader's buffer arrives in pieces. Only the first piece carries the
# five fixed columns; the rest is bare path characters, which the five-field
# scan runs straight past, so the "no pathname column" branch printed the raw
# host tail — the one thing put_maps exists to prevent. Two legs, because the
# fix has two halves: a buffer that fits any path the kernel can print, and a
# refusal to re-parse a piece of a line when it does not.
#
# The length has to come from the guest side of a short rootfs: a bind's host
# path is capped at 512 bytes, so it cannot supply one. The rootfs prefix is
# then the only part of the host spelling the guest cannot also say, which makes
# the mktemp directory's own name the marker to count.
ML_DIR=$(mktemp -d)
ML_MARK=$(basename "$ML_DIR")
ML_ROOT="$ML_DIR/rootfs"
mkdir -p "$ML_ROOT"
if guest_xlate_ready "maps over-long-line legs" &&
    guest_cc_report "$ML_ROOT/mapslong" tests/arm64emu/chroot/native/guests/mapslong.c; then
    # (a) A host path long enough to have overrun the old 2 KiB line buffer,
    # which is an ordinary deep tree: a nested node_modules, a Java class
    # hierarchy. The mapping must still appear, under its guest name.
    ml_deep=""
    ml_i=0
    while [ "$ml_i" -lt 50 ]; do
        ml_deep="$ml_deep/cngdeepcngdeepcngdeepcngdeepcngdeepcngdeepcng"
        ml_i=$((ml_i + 1))
    done
    if mkdir -p "$ML_ROOT$ml_deep" 2>/dev/null &&
        printf 'x' > "$ML_ROOT$ml_deep/blob" 2>/dev/null; then
        # shellcheck disable=SC2086  # $ARM64EMU_NATIVE_GUEST_BINDS is a deliberately split list
        out=$(run_t 60 $ARM64EMU_NATIVE_GUEST_BINDS "$ML_ROOT" /mapslong "$ml_deep/blob" \
            "$ML_MARK" 2>&1)
        check_contains "maps a long host path survives whole" \
            "lines=1 hostleak=0 mapped=1 malformed=0" "$out"
    else
        skip "maps long-path leg: the host filesystem refused the path"
    fi

    # (b) Past any buffer, so the size alone cannot be the whole fix. The kernel
    # escapes '\n' in the pathname column as the four bytes \012, so a path made
    # of newlines prints four times its own length: measured, fourteen 251-byte
    # components print as a 14155-byte line, and PATH_MAX bounds only the path.
    # A guest can make such a directory inside its own rootfs. Nothing faithful
    # can be emitted for that line — but half of it is not the answer either,
    # and re-parsing the second half is what printed the raw host tail.
    ml_nl=$(printf 'a'; printf '%0249d' 0 | tr '0' '\n'; printf 'b')
    ml_wide=""
    ml_i=0
    while [ "$ml_i" -lt 14 ]; do
        ml_wide="$ml_wide/$ml_nl"
        ml_i=$((ml_i + 1))
    done
    if mkdir -p "$ML_ROOT$ml_wide" 2>/dev/null &&
        printf 'x' > "$ML_ROOT$ml_wide/blob" 2>/dev/null; then
        # shellcheck disable=SC2086  # $ARM64EMU_NATIVE_GUEST_BINDS is a deliberately split list
        out=$(run_t 60 $ARM64EMU_NATIVE_GUEST_BINDS "$ML_ROOT" /mapslong "$ml_wide/blob" \
            "$ML_MARK" 2>&1)
        # mapped=0 by construction: the guest names the path in raw bytes and
        # maps prints it escaped, so the row cannot be found by that spelling.
        check_contains "maps an unprintable-length path emits no half-line" \
            "hostleak=0 mapped=0 malformed=0" "$out"
    else
        skip "maps escaped-path leg: the host filesystem refused the path"
    fi
fi
rm -rf "$ML_DIR"

# A refreshable file's tracking entry used to publish the descriptor before
# its kind and identity, so a sibling thread reading the new number in that
# interval judged it against the entry's previous identity, retired it, and
# the file never refreshed again. Reader threads pread the whole reserved range
# from offset 0 while the main thread opens batches of /proc/uptime into it and
# checks that each rewind reads a clock that has moved. The window is a few
# instructions wide, so the previous build loses a refresh in only some runs;
# a lost one is never right.
PR_ROOT=$(mktemp -d)
if guest_xlate_ready "refresh-tracking race leg" &&
    guest_cc "$PR_ROOT/procrace" tests/arm64emu/chroot/native/guests/procrace.c -pthread; then
    # shellcheck disable=SC2086  # $ARM64EMU_NATIVE_GUEST_BINDS is a deliberately split list
    out=$(run_t 120 $ARM64EMU_NATIVE_GUEST_BINDS "$PR_ROOT" /procrace 2>&1)
    check_contains "a rewind refreshes however readers race the open" \
        "procrace: samples>0=1 lost=0" "$out"
else
    skip "refresh-tracking race leg: could not build tests/arm64emu/chroot/native/guests/procrace.c with -pthread"
fi
rm -rf "$PR_ROOT"

# The mount tables are emitted from one static snapshot of the view, held by
# thread id. From guest context it runs on the guest's signal mask, so a
# handler that interrupted the emission and opened a mount table itself waited
# for the snapshot its own thread held: the previous build hangs here in every
# run. An interval timer interrupts a loop of mountinfo reads, and the handler
# reads it too; every read must be the same table.
MS_ROOT=$(mktemp -d)
if guest_xlate_ready "mount table from a signal handler leg" &&
    guest_cc_report "$MS_ROOT/mntsig" tests/arm64emu/chroot/native/guests/mntsig.c; then
    # shellcheck disable=SC2086  # $ARM64EMU_NATIVE_GUEST_BINDS is a deliberately split list
    out=$(run_t 60 $ARM64EMU_NATIVE_GUEST_BINDS "$MS_ROOT" /mntsig 2>&1)
    check_contains "a mount table opened from a handler that interrupted another" \
        "mntsig: handled>0=1 bad=0" "$out"
fi
rm -rf "$MS_ROOT"

# A /proc number spelled with a leading zero is no name at all to procfs
# (name_to_int), so "/proc/self/fd/05" and "/proc/0<pid>/exe" are ENOENT
# whatever descriptor 5 and process <pid> are. Where the monitor answers such a
# name itself — an exec through an fd link, exe/cwd, a synthesized cmdline — it
# read the digits as the number, and the previous build ran the program and
# read the files. Differential against the same guest with nothing in the way.
PN_ROOT=$(mktemp -d)
if guest_xlate_ready "leading-zero /proc name leg" &&
    guest_cc_report "$PN_ROOT/procnum" tests/arm64emu/chroot/native/guests/procnum.c; then
    pn_base=$(emu_t 60 "$PN_ROOT/procnum" 2>/dev/null)
    # shellcheck disable=SC2086  # $ARM64EMU_NATIVE_GUEST_BINDS is a deliberately split list
    pn_got=$(run_t 60 $ARM64EMU_NATIVE_GUEST_BINDS "$PN_ROOT" /procnum 2>/dev/null)
    case "$pn_base" in
    *"fd plain open: ok"*"exec pid-zero-exe: errno=2"*)
        if [ "$pn_got" = "$pn_base" ]; then
            pass=$((pass + 1))
            echo "  ok   a /proc number with a leading zero names what the kernel's does"
        else
            fail=$((fail + 1))
            echo "  FAIL a /proc number with a leading zero names what the kernel's does"
            printf '%s\n' "$pn_base" >"$ARM64EMU_NATIVE_TMP/pn.want"
            printf '%s\n' "$pn_got" >"$ARM64EMU_NATIVE_TMP/pn.got"
            diff "$ARM64EMU_NATIVE_TMP/pn.want" "$ARM64EMU_NATIVE_TMP/pn.got" | sed 's/^/    /'
        fi
        ;;
    *)
        skip "leading-zero /proc name leg: the unemulated guest did not run cleanly"
        ;;
    esac
fi
rm -rf "$PN_ROOT"

# stat, statx, access and an O_PATH fstat on the synthesized global files
# (version, loadavg, uptime, stat, sys/kernel/overflow{u,g}id). Android's SELinux
# refuses an app every one of them, and the open was served from memory regardless, so `cat` worked where
# `stat` and `test -r` of the same name failed. The guest checks the shape every
# form must agree on, which holds wherever the host answers or refuses the call
# itself: on a device the refusal is real, and elsewhere ARM64EMU_PROCSYNTH_FORCE_STAT_DENY=1
# makes the second run take the same path (/proc/stat and the overflow{u,g}id
# sysctls, readable here, join it with ARM64EMU_PROCSTAT_FORCE_SYNTH and
# ARM64EMU_OVERFLOWID_FORCE_SYNTH). Where the host has no statx at all the guest leaves
# that form out and says so on a last line, which is taken off and noted.
PS_ROOT=$(mktemp -d)
ps_nostatx() {
    case "$ps_got" in
    *"
statx: none on this host")
        ps_got=${ps_got%"
statx: none on this host"}
        [ -n "${ps_noted:-}" ] ||
            skip "statx of a synthesized /proc file: this host has no statx to issue"
        ps_noted=1 ;;
    esac
}
if guest_xlate_ready "synthesized /proc stat leg" &&
    guest_cc_report "$PS_ROOT/procstat" tests/arm64emu/chroot/native/guests/procstat.c; then
    ps_want="/proc/version: ok
/proc/loadavg: ok
/proc/uptime: ok
/proc/stat: ok
/proc/sys/kernel/overflowuid: ok
/proc/sys/kernel/overflowgid: ok"
    # shellcheck disable=SC2086  # $ARM64EMU_NATIVE_GUEST_BINDS is a deliberately split list
    ps_got=$(run_t 60 $ARM64EMU_NATIVE_GUEST_BINDS "$PS_ROOT" /procstat 2>/dev/null)
    ps_nostatx
    if [ "$ps_got" = "$ps_want" ]; then
        pass=$((pass + 1))
        echo "  ok   stat and access of a synthesized /proc file agree with the file it is served as"
    else
        fail=$((fail + 1))
        echo "  FAIL stat and access of a synthesized /proc file agree with the file it is served as"
        printf '%s\n' "$ps_got" | sed 's/^/    /'
    fi
    # shellcheck disable=SC2086
    ps_got=$(ARM64EMU_PROCSYNTH_FORCE_STAT_DENY=1 ARM64EMU_PROCSTAT_FORCE_SYNTH=1 ARM64EMU_OVERFLOWID_FORCE_SYNTH=1 \
        run_t 60 $ARM64EMU_NATIVE_GUEST_BINDS "$PS_ROOT" /procstat 2>/dev/null)
    ps_nostatx
    if [ "$ps_got" = "$ps_want" ]; then
        pass=$((pass + 1))
        echo "  ok   ...and with the host's own refusal of each call forced"
    else
        fail=$((fail + 1))
        echo "  FAIL ...and with the host's own refusal of each call forced"
        printf '%s\n' "$ps_got" | sed 's/^/    /'
    fi
fi
rm -rf "$PS_ROOT"

# cmdline and environ are the process's memory, read when the file is: a title
# written over argv is what `ps` prints from then on (node's process.title,
# setproctitle, nginx's and postgres's status lines). They were the registry's
# exec-time copies, so every one of those read back as the command line the
# process started with. Now the kernel's own record is pointed at the guest's
# strings where it takes PR_SET_MM_MAP (a desktop kernel: "record: kernel"),
# and where it does not (Android, or ARM64EMU_MM_MAP_FORCE_DENY=1) the ranges are read
# out of the guest's memory under the kernel's rules — ours directly, a child's
# with process_vm_readv. The guest checks its own files and a forked child's
# against those rules, which a native host first confirms against the kernel
# itself. arm64emu reaches the memory of no running process but the caller's
# (process_vm_readv answers EPERM), so on the cross host a
# child's files are still the copies and only the guest's own are asserted.
PT_ROOT=$(mktemp -d)
pt_check() { # desc, want, got
    if [ "$2" = "$3" ]; then
        pass=$((pass + 1)); echo "  ok   $1"
    else
        fail=$((fail + 1)); echo "  FAIL $1"
        printf '%s\n' "$2" >"$ARM64EMU_NATIVE_TMP/pt.want"
        printf '%s\n' "$3" >"$ARM64EMU_NATIVE_TMP/pt.got"
        diff "$ARM64EMU_NATIVE_TMP/pt.want" "$ARM64EMU_NATIVE_TMP/pt.got" | sed 's/^/    /'
    fi
}
if guest_xlate_ready "live cmdline/environ leg" &&
    guest_cc_report "$PT_ROOT/proctitle" tests/arm64emu/chroot/native/guests/proctitle.c; then
    pt_legs="start: ok
start environ: ok
environ: ok
kid cmdline: ok
kid environ: ok
title: ok
overflow: ok
overflow environ: ok"
    pt_record=ours
    if [ -n "$ARM64EMU_NATIVE_EMU" ]; then
        pt_legs=$(printf '%s\n' "$pt_legs" | grep -v '^kid ')
        pt_cut() { grep -v -e '^kid ' -e '^mm_map: '; }
        skip "a child's live cmdline/environ: arm64emu reaches no running process's memory but the caller's"
    else
        pt_cut() { grep -v '^mm_map: '; }
        pt_got=$(kref "$PT_ROOT/proctitle" 2>&1)
        case "$pt_got" in
        "mm_map: yes"*) pt_record=kernel ;;
        esac
        # A kernel before 4.18 ended a title run into the environment at its
        # NUL and left the NUL out (Android 8's 3.18, measured): no reference
        # for that leg, which the emulation answers as the 6.1 it advertises.
        # The rest is still the kernel's to judge.
        pt_kwant=$pt_legs
        if printf '%s\n' "$pt_got" |
            grep -q '^overflow: FAIL got [0-9]* \[\([^]]*\)\] want [0-9]* \[\1|\]$'; then
            skip "the kernel's cmdline of an overflowing title: the host kernel predates its NUL (4.18), so it is no reference"
            pt_kwant=$(printf '%s\n' "$pt_legs" | grep -v '^overflow: ')
            pt_got=$(printf '%s\n' "$pt_got" | grep -v '^overflow: ')
        fi
        pt_check "the kernel's own cmdline and environ follow the rules the guest checks" \
            "record: kernel
$pt_kwant" "$(printf '%s\n' "$pt_got" | pt_cut)"
    fi
    # shellcheck disable=SC2086  # $ARM64EMU_NATIVE_GUEST_BINDS is a deliberately split list
    pt_got=$(run_t 60 $ARM64EMU_NATIVE_GUEST_BINDS "$PT_ROOT" /proctitle 2>/dev/null)
    pt_check "cmdline and environ are the guest's live strings" \
        "record: $pt_record
$pt_legs" "$(printf '%s\n' "$pt_got" | pt_cut)"
    # shellcheck disable=SC2086
    pt_got=$(ARM64EMU_MM_MAP_FORCE_DENY=1 run_t 60 $ARM64EMU_NATIVE_GUEST_BINDS "$PT_ROOT" /proctitle \
        2>/dev/null)
    pt_check "...and read out of guest memory where the kernel's record stays ours" \
        "record: ours
$pt_legs" "$(printf '%s\n' "$pt_got" | pt_cut)"
fi
rm -rf "$PT_ROOT"

# --- guest-shell scenarios -------------------------------------------------
m11_ready=0
if [ -n "$M11_ALPINE" ] && [ -x "$M11_ALPINE/bin/busybox" ]; then
    m11_ready=1
else
    skip "guest-shell /proc scenarios: no alpine rootfs"
fi
# Bound at the guest's /tmp so the mount table has a third row to find. The host
# side is a directory of ours: /tmp is not a path Android has.
M11TMP=$(mktemp -d)

# m11_sh <desc> <expected> <script>: run the script in an Alpine guest and
# compare its (whitespace-trimmed) stdout. $M11BIND is the -b spec, so a leg
# that needs a different mount point can set it around itself.
M11BIND="$M11TMP:/tmp"
m11_sh() {
    got=$(run -b "$M11BIND" "$M11_ALPINE" /bin/busybox sh -c "$3" 2>/dev/null)
    if [ "$got" = "$2" ]; then
        pass=$((pass + 1)); echo "  ok   m11 $1"
    else
        fail=$((fail + 1)); echo "  FAIL m11 $1"
        echo "    want: $2"
        echo "    got:  $got"
    fi
}

if [ "$m11_ready" -eq 1 ]; then
    # The guest's own command line — the host file holds the arm64emu argv.
    m11_sh "cmdline is the guest's own argv" "cat|/proc/self/cmdline|" \
        'cat /proc/self/cmdline | tr "\0" "|"'

    # The mount table is the guest's: rootfs, /proc, and the -b bind.
    m11_sh "mounts shows rootfs + proc + bind" "3" \
        'grep -c -e "^/dev/root / " -e "^proc /proc proc " -e " /tmp " /proc/mounts'

    # ...and it names a device as the source of every row, as a real one does.
    # The directory a bind came from is a host path: `df`, `mount` and findmnt
    # print that field, and it used to spell out where the rootfs lives on the
    # device — in the one namespace whose whole point is that the guest cannot
    # name a host path (the maps leg above is the same assertion one file over).
    m11_sh "no mount table leaks the bind's source path" "0" \
        "cat /proc/mounts /proc/self/mountinfo /proc/self/mountstats |
         grep -c '$M11TMP'"
    m11_sh "...and the bind is still a row of the guest's own device" "1" \
        'grep -c "^/dev/root /tmp " /proc/mounts'

    # A guest mount point with whitespace in it. Every one of these tables is
    # whitespace-separated and everything that reads one splits on that —
    # libmount, busybox df and mount, findmnt — so a real kernel escapes space,
    # tab, newline and backslash as octal in every path and device name it
    # prints (fs/proc_namespace.c). Printed raw, the row named "/tmp/my" and
    # handed the field after it, the filesystem type, to "dir".
    M11BIND="$M11TMP:/tmp/my dir"
    m11_sh "a mount point with a space is escaped in all three tables" "3" \
        "cat /proc/mounts /proc/self/mountinfo /proc/self/mountstats |
         grep -cF '/tmp/my\040dir'"
    m11_sh "...and appears raw in none of them" "0" \
        "cat /proc/mounts /proc/self/mountinfo /proc/self/mountstats |
         grep -cF '/tmp/my dir'"
    # ...and it is still the mount point the guest can reach by that name.
    m11_sh "...and the guest still finds it under that name" "ok" \
        '[ -d "/tmp/my dir" ] && echo ok'
    M11BIND="$M11TMP:/tmp"

    # No host path may appear in the guest's view of its own mappings.
    m11_sh "maps leaks no rootfs host path" "0" \
        "grep -c '$M11_ALPINE' /proc/self/maps"

    # Host processes are invisible; the guest's own are not.
    # A thread's directory spells the same files: its address space, its mounts and
    # its links are the process's, and the host's describe arm64emu. The shell
    # reads its own, with no other process in between.
    got=$(run_t 60 -b "$M11BIND" "$M11_ALPINE" /bin/busybox sh -c '
        cd /etc; t=/proc/$$/task/$$
        for f in maps mounts mountinfo; do
            while read -r l; do echo "$f $l"; done <$t/$f
        done
        for f in exe cwd root; do echo "$f=$(readlink $t/$f)"; done
        [ -d $t/cwd ] && [ -f $t/exe ] && echo followed
        cd $t/cwd && pwd' 2>/dev/null)
    check_absent "m11 a thread's directory leaks no host path" "$M11_ALPINE" "$got"
    check_contains "m11 ...its exe link is the guest's" "exe=/bin/busybox" "$got"
    check_contains "m11 ...and its cwd link" "cwd=/etc" "$got"
    check_contains "m11 ...both followed where they lead" "followed" "$got"
    check_contains "m11 ...its maps is the guest's" "maps " "$got"
    check_contains "m11 ...its mount table too" "mountinfo 1 1" "$got"

    m11_sh "a host pid is hidden" "hidden" \
        '[ -e /proc/1/stat ] || echo hidden'
    m11_sh "our own pid is visible" "visible" \
        '[ -e /proc/$$/stat ] && echo visible'
    m11_sh "ps sees only the guest's processes" "few" \
        'n=$(ps | wc -l); [ "$n" -lt 10 ] && echo few'

    # The magic links stay in guest terms.
    m11_sh "exe link is the guest path" "/bin/busybox" 'readlink /proc/self/exe'
    m11_sh "root link is the guest root" "/" 'readlink /proc/self/root'

    # loadavg / uptime shape (Android denies the real files to apps).
    m11_sh "loadavg has five fields" "5" 'wc -w < /proc/loadavg'
    m11_sh "uptime has two fields" "2" 'wc -w < /proc/uptime'

    # bubblewrap reads these two before it does anything else and dies where
    # Android denies them. Forced here, the guest gets the kernel's default;
    # unforced, a readable host file is the answer, and the debug trace shows
    # which one served the open.
    got=$(ARM64EMU_OVERFLOWID_FORCE_SYNTH=1 ARM64EMU_NATIVE_DEBUG=1 run "$M11_ALPINE" /bin/busybox sh -c \
        'echo $(cat /proc/sys/kernel/overflowuid) $(cat /proc/sys/kernel/overflowgid)' 2>&1)
    case $got in
    *"procfs /proc/sys/kernel/overflowuid -> fd"*"procfs /proc/sys/kernel/overflowgid -> fd"*"65534 65534")
        pass=$((pass + 1)); echo "  ok   m11 overflowuid and overflowgid are served from the fallback, 65534" ;;
    *)
        fail=$((fail + 1)); echo "  FAIL m11 overflowuid and overflowgid are served from the fallback, 65534"
        printf '%s\n' "$got" | sed 's/^/    /' ;;
    esac
    if [ -r /proc/sys/kernel/overflowuid ] && [ -r /proc/sys/kernel/overflowgid ]; then
        want="$(cat /proc/sys/kernel/overflowuid) $(cat /proc/sys/kernel/overflowgid)"
        got=$(ARM64EMU_NATIVE_DEBUG=1 run "$M11_ALPINE" /bin/busybox sh -c \
            'echo $(cat /proc/sys/kernel/overflowuid) $(cat /proc/sys/kernel/overflowgid)' 2>&1)
        case $got in
        *"procfs /proc/sys/kernel/overflow"*)
            fail=$((fail + 1)); echo "  FAIL m11 a readable host overflowuid/overflowgid is not shadowed"
            printf '%s\n' "$got" | sed 's/^/    /' ;;
        *"$want")
            pass=$((pass + 1)); echo "  ok   m11 a readable host overflowuid/overflowgid is not shadowed" ;;
        *)
            fail=$((fail + 1)); echo "  FAIL m11 a readable host overflowuid/overflowgid is not shadowed"
            echo "    want: $want"
            printf '%s\n' "$got" | sed 's/^/    /' ;;
        esac
    else
        skip "overflowuid/overflowgid passthrough: the host cannot read them"
    fi

    # comm is the guest program, not arm64emu (PR_SET_NAME, not synthesis).
    m11_sh "comm names the guest program" "busybox" 'cat /proc/self/comm'

    # What a synthesized fd says about itself. The shell's redirection is the
    # common way one reaches a program: the shell opens the file, dup2()s it
    # onto 0 and closes the original, so the program's fstat(0) is of a dup'd
    # fd. That used to describe the memfd behind it — 0777, no links, the
    # content's size — where stat of the path says the real file; and the fd
    # link read "/memfd:cng-proc (deleted)". busybox stat -L on the link is a
    # stat through it, which lands on the same memfd. Every answer here is what
    # the kernel gives for the real file, compared against the path form.
    m11_sh "a redirected synthesized fd stats as the real file (mode links size)" \
        "same" \
        'a=$(stat -c "%f %h %s %d %i" /proc/$$/mounts);
         b=$(stat -L -c "%f %h %s %d %i" /dev/stdin < /proc/$$/mounts);
         [ "$a" = "$b" ] && echo same || echo "$a | $b"'
    # The global files Android hides (loadavg, uptime, stat, version) answer
    # their stat as the file they are served as: the host refuses it there, and
    # the guest used to read /proc/version with cat and be told "Permission
    # denied" by stat on the same name. The descriptor and the path agree.
    m11_sh "...and for a root-owned global file" "same" \
        'a=$(stat -c "%f %h %s %d %i %u" /proc/loadavg);
         b=$(stat -L -c "%f %h %s %d %i %u" /dev/stdin < /proc/loadavg);
         [ "$a" = "$b" ] && echo same || echo "$a | $b"'
    m11_sh "stat of a global /proc file is a /proc regular file, as access says" \
        "8124 1 0 0 readable" \
        'a=$(stat -c "%f %h %s %u" /proc/version) || exit 1;
         [ -r /proc/version ] && echo "$a readable" || echo "$a unreadable"'
    m11_sh "the fd link of a synthesized file names the file" \
        "/proc/loadavg" 'readlink /proc/self/fd/0 < /proc/loadavg'
    m11_sh "...and a process entry by its number" "ok" \
        'exec 3< /proc/self/status; [ "$(readlink /proc/self/fd/3)" = "/proc/$$/status" ] && echo ok || readlink /proc/self/fd/3'
    m11_sh "the filesystem behind a synthesized fd is proc" "same" \
        'a=$(stat -f -c "%t %T" /proc/$$/mounts);
         b=$(stat -L -f -c "%t %T" /dev/stdin < /proc/$$/mounts);
         [ "$a" = "$b" ] && echo same || echo "$a | $b"'

    # Binding the host /proc at /proc is a common habit from proot; it must not
    # hand the guest the host's process list back. The hidden view keys on where
    # a path lands, not on which map rule put it there.
    got=$(run -b /proc:/proc "$M11_ALPINE" /bin/busybox sh -c \
        'ls /proc | grep -c "^[0-9]*$"' 2>/dev/null)
    own=$(run "$M11_ALPINE" /bin/busybox sh -c \
        'ls /proc | grep -c "^[0-9]*$"' 2>/dev/null)
    if [ -n "$got" ] && [ "$got" = "$own" ]; then
        pass=$((pass + 1)); echo "  ok   m11 -b /proc:/proc hides host pids like the default"
    else
        fail=$((fail + 1))
        echo "  FAIL m11 -b /proc:/proc hides host pids like the default"
        echo "    bound: $got  default: $own"
    fi
    got=$(run -b /proc:/proc "$M11_ALPINE" /bin/busybox sh -c \
        '[ -e /proc/1/stat ] || echo hidden' 2>/dev/null)
    check_contains "a host pid stays unreachable under a /proc bind" \
        "hidden" "$got"

    # An explicit -b DIR:/proc pointing somewhere else is the user overriding
    # the host view, and must outrank both the passthrough and the synthesis.
    BP=$(mktemp -d); printf 'bound\n' > "$BP/loadavg"
    got=$(run -b "$BP":/proc "$M11_ALPINE" /bin/busybox cat /proc/loadavg \
        2>/dev/null)
    check_contains "an explicit -b DIR:/proc outranks the synthesis" \
        "bound" "$got"
    rm -rf "$BP"

    # --no-proc turns the whole thing off: the rootfs has no /proc of its own.
    got=$(run --no-proc "$M11_ALPINE" /bin/busybox sh -c \
        'cat /proc/loadavg 2>/dev/null || echo none' 2>/dev/null)
    check_contains "--no-proc leaves the guest without /proc" "none" "$got"

    # --shared-proc: two INDEPENDENT invocations over one rootfs share the
    # process view through the per-rootfs broker — ps in the second lists the
    # first's guest process, with the cmdline the sleeper published. Without
    # the flag each invocation has its own registry and hides the other's
    # processes (the pre-flag behavior, still the default).
    run --shared-proc "$M11_ALPINE" /bin/busybox sleep 87.65 2>/dev/null &
    m11_bg=$!
    sleep 2
    got=$(run --shared-proc "$M11_ALPINE" /bin/busybox ps 2>/dev/null)
    case "$got" in
    *"sleep 87.65"*)
        pass=$((pass + 1))
        echo "  ok   m11 --shared-proc: ps sees the other invocation's guest" ;;
    *)
        fail=$((fail + 1))
        echo "  FAIL m11 --shared-proc: ps sees the other invocation's guest"
        echo "    got: $got" ;;
    esac
    solo=$(run "$M11_ALPINE" /bin/busybox ps 2>/dev/null)
    case "$solo" in
    *"sleep 87.65"*)
        fail=$((fail + 1))
        echo "  FAIL m11 without --shared-proc the other invocation stays hidden" ;;
    *)
        pass=$((pass + 1))
        echo "  ok   m11 without --shared-proc the other invocation stays hidden" ;;
    esac
    kill "$m11_bg" 2>/dev/null
    wait "$m11_bg" 2>/dev/null
fi
rm -rf "$M11TMP"
