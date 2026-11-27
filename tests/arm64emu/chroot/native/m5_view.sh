# M5: the guest's view of the file tree (sourced by tests/arm64emu/chroot/native/run.sh).
# Which file a name reaches — through the rootfs, the binds, symlinks, a chroot,
# the /dev zone and the fd links — and what a read-only bind refuses. Every row
# is asked by a guest making the call, and what it reached is told by content
# planted on the host side.
echo "== M5: the guest's view: paths and binds =="

# Outside every bind: a host-only file must be one no guest path reaches, and
# Termux's $TMPDIR is inside $PREFIX, which is bound.
V=$(cng_outside_binds "$(mktemp -d)")
mkdir -p "$V/root/bin2" "$V/root/etc" "$V/root/a/b" "$V/root/a/etc" "$V/root/a/dat" \
    "$V/root/dev" "$V/root/sub" "$V/root/home/u/foo" "$V/root/datextra" "$V/root/dat" \
    "$V/root/usr/bin" "$V/root/usr/lib" "$V/root/usr/inner" "$V/root/lib" \
    "$V/b1" "$V/b2" "$V/real" "$V/devb"
echo ROOT-PASSWD >"$V/root/etc/passwd"
echo INNER >"$V/root/a/etc/inner"
echo AB-X >"$V/root/a/b/x"
echo REL >"$V/root/home/u/foo/bar"
echo EXTRA >"$V/root/datextra/x"
echo HOST-SECRET >"$V/secret"
echo B1 >"$V/b1/x"
echo B1-HOSTS >"$V/b1/hosts"
echo B2 >"$V/b2/3"
echo BOUND >"$V/real/f"
echo NOTNULL >"$V/devb/null"
echo USR-LIB >"$V/root/usr/lib/m"
echo LIB >"$V/root/lib/m"
echo USR-M >"$V/root/usr/m"
ln -s real "$V/link"
ln -s /usr/bin "$V/root/bin"          # absolute target, re-rooted into the rootfs
ln -s usr/lib "$V/root/l"             # relative target
ln -s /etc/passwd "$V/root/flink"
ln -s passwd "$V/root/etc/rel"
ln -s "$V/secret" "$V/root/esc"       # names a host file no guest path reaches
ln -s /etc/passwd "$V/root/sub/lnk"
ln -s /nowhere "$V/root/dangling"

if guest_xlate_ready "the guest's view" &&
    guest_cc_report "$V/root/viewpath" tests/arm64emu/chroot/native/guests/viewpath.c; then
    # shellcheck disable=SC2086  # $ARM64EMU_NATIVE_GUEST_BINDS is a deliberately split arg list
    vp() { run_t 60 $ARM64EMU_NATIVE_GUEST_BINDS "$@" 2>/dev/null; }
    # vw WANT DESC ARGS...: one operation, the options ahead of "--".
    vw() {
        _w=$1 _d=$2
        shift 2
        _o=""
        while [ "$1" != -- ]; do
            _o="$_o $1"
            shift
        done
        shift
        # shellcheck disable=SC2086  # a deliberately split option list
        check_contains "$_d" "$_w" "$(vp $_o "$V/root" /viewpath "$@")"
    }

    vw "read /etc/passwd: ROOT-PASSWD" "a guest name is the rootfs's" -- read:/etc/passwd
    vw ": ROOT-PASSWD" "'..' collapses inside the rootfs" -- read:/a/b/../../../etc/passwd
    vw ": errno 2" "'..' cannot climb out of it" -- read:/../secret
    vw ": errno 2" "...nor can a create" -- "mk:/../esc_$$/x"
    vw ": ROOT-PASSWD" "dots and double slashes collapse" -- read:/etc/.//passwd
    vw ": REL" "a relative name uses the working directory" -w /home/u -- read:foo/bar
    vw ": REL" "...as it does after a chdir" -- chdir:/home/u read:foo/bar
    check_contains "the rootfs / is the host's own tree" ": HOST-SECRET" \
        "$(run_t 60 -l / "$V/root/viewpath" "read:$V/secret" 2>/dev/null)"

    # Binds: matched by whole components, the longest first.
    vw ": B1" "a name under a bind is the bind's" -b "$V/b1:/dat" -- read:/dat/x
    vw ": directory" "...and so is the mount point" -b "$V/b1:/dat" -- stat:/dat
    vw ": B2" "the longest bind wins" -b "$V/b1:/dat" -b "$V/b2:/dat/pts" -- read:/dat/pts/3
    vw ": EXTRA" "a bind's prefix is whole components" -b "$V/b1:/dat" -- read:/datextra/x
    vw ": B1" "a :ro suffix is not part of the mount point" -b "$V/b1:/dat:ro" -- read:/dat/x
    vw ": NOTNULL" "a bind outranks the /dev zone" -b "$V/devb:/dev" -- read:/dev/null

    # A bind source may be a single file, `-b /etc/resolv.conf:/etc/resolv.conf`
    # being the commonest bind there is. It is stored as the kernel spells it,
    # symlink-free, since the same prefix is matched against host paths the kernel
    # produces: kept in the caller's spelling, an fd link of it read back as the
    # host path. It has no children, as a file has none.
    vw ": BOUND" "a file bind reads" -b "$V/link/f:/etc/x" -- read:/etc/x
    vw "fdlink /etc/x: /etc/x" "...and is named by its guest path" -b "$V/link/f:/etc/x" \
        -- fdlink:/etc/x
    vw ": errno 20" "...and a name under it is ENOTDIR" -b "$V/link/f:/etc/x" -- read:/etc/x/y
    vw "fdlink /dat/f: /dat/f" "a directory bind's files are too" -b "$V/link:/dat" \
        -- fdlink:/dat/f

    # chroot(2) moves the root and unmounts nothing: binds under the new root are
    # rebased onto it, one outside is gone. It needs CAP_SYS_CHROOT.
    vw "read /etc/inner: INNER" "chroot moves the root" -u -- chroot:/a read:/etc/inner
    vw ": B1" "a chroot to / keeps the binds" -u -b "$V/b1:/dat" -- chroot:/ read:/dat/x
    vw ": B1" "a bind under the new root is rebased" -u -b "$V/b1:/a/dat" -- chroot:/a read:/dat/x
    vw "read /etc/hosts: errno 2" "one outside it is gone" -u -b "$V/b1:/etc" \
        -- chroot:/a read:/etc/hosts
    vw "read x: AB-X" "a chroot keeps the working directory" -u -w /a/b -- chroot:/a read:x
    vw ": errno 2" "'..' cannot climb out of the new root" -u -- chroot:/a chdir:/ \
        read:../etc/passwd
    vw "chroot /sub: errno 1" "chroot without CAP_SYS_CHROOT is EPERM" -- chroot:/sub

    # Resolution is the kernel's, not the shell's: ".." backs out of where a
    # symlink led, so it is applied while walking and not collapsed up front.
    vw ": USR-LIB" "'..' backs out of where a symlink led" -- read:/bin/../lib/m
    vw ": USR-M" "...which is the target's parent" -- read:/bin/../m
    vw ": directory" "a symlinked directory is its target" -- stat:/bin
    vw ": USR-LIB" "a relative target resolves beside its link" -- read:/l/m
    vw ": ROOT-PASSWD" "an absolute target is re-rooted" -- read:/flink
    vw ": ROOT-PASSWD" "a relative one is followed" -- read:/etc/rel
    vw ": symlink" "a final symlink is kept by lstat" -- lstat:/flink
    vw ": errno 40" "...and refused by O_NOFOLLOW" -- nofollow:/flink
    vw ": ROOT-PASSWD" "...the ones before it are still followed" -- nofollow:/bin/../../etc/passwd
    vw ": errno 2" "a target outside the rootfs is not reached" -- read:/esc
    vw ": errno 2" "'..' relative to a descriptor cannot climb out" -- readat:/sub:../../secret
    vw ": ROOT-PASSWD" "...and still works inside" -- readat:/sub:../etc/passwd
    vw ": ROOT-PASSWD" "an absolute target is re-rooted there too" -- readat:/sub:lnk
    vw ": ROOT-PASSWD" "a name through /proc/self/cwd" -- read:/proc/self/cwd/etc/passwd
    vw ": ROOT-PASSWD" "...and through /proc/self/root" -- read:/proc/self/root/etc/passwd
    vw ": ROOT-PASSWD" "...and through an O_PATH descriptor's link" -- fdread:/etc/passwd
    vw ": errno 2" "/proc/self/root cannot climb out" -- read:/proc/self/root/../secret

    # The /dev zone is a whitelist: a node the host has and the list does not is
    # not reachable, and its name is the rootfs's.
    for _n in mem kmsg loop-control fuse port hwrng vda sda; do
        [ -e "/dev/$_n" ] || continue
        vw ": errno 2" "a host node off the /dev list is not reachable ($_n)" -- "stat:/dev/$_n"
        echo FAKE >"$V/root/dev/$_n"
        vw ": FAKE" "...its name is the rootfs's" -- "read:/dev/$_n"
        rm -f "$V/root/dev/$_n"
        break
    done

    # What a path call reaches when it is not an open.
    vw ": ok" "faccessat has no flags word" -- access:/etc/passwd
    vw "access /dangling: errno 2" "access follows a dangling link" -- access:/dangling
    case "$(vp "$V/root" /viewpath accessnf:/dangling)" in
    *"errno 38"*) skip "faccessat2: this host has none" ;;
    *) vw "accessnf /dangling: ok" "faccessat2 AT_SYMLINK_NOFOLLOW answers for the link" \
        -- accessnf:/dangling ;;
    esac
    vw ": errno 22" "an invalid access mode is EINVAL" -- badmode:/etc/passwd
    vw ": errno 22" "...under fake root too" -u -- badmode:/etc/passwd
    vw ": errno 2" "an xattr call on a host-only path is contained" -- "getxa:$V/secret"
    case "$(vp "$V/root" /viewpath getxa:/etc/passwd)" in
    *"errno 61"* | *"errno 95"*)
        pass=$((pass + 1)); echo "  ok   ...and reaches the rootfs's file" ;;
    *)
        fail=$((fail + 1)); echo "  FAIL ...and reaches the rootfs's file"
        vp "$V/root" /viewpath getxa:/etc/passwd | sed 's/^/    /' ;;
    esac
    vw ": errno 2" "inotify on a host-only path is contained" -- "inotify:$V/secret"
    vw ": ok" "...and reaches the rootfs's file" -- inotify:/etc/passwd

    # A name the rootfs prefix pushes past PATH_MAX is refused, not cut to the
    # shorter name that fits: 4090 bytes of "/d" name nothing here.
    _long=""
    while [ ${#_long} -lt 4080 ]; do _long="$_long/dddddddddddddddddddddddddddddd"; done
    vw ": errno 36" "a path with no room for the rootfs prefix is ENAMETOOLONG" -- "read:$_long"
fi

# --- :ro binds --------------------------------------------------------------
# A ":ro" bind answers EROFS for every mutating call while still serving reads,
# however the file is named: by path, relative to a descriptor (rm -rf, find
# -delete, tar and git all work so), through an fd link, by descriptor alone. The
# rw run is the negative control: the same calls must NOT answer EROFS there, so a
# blanket refusal cannot pass both. The guest's expectations are a kernel's, whose
# own read-only bind mount passes every leg.
if guest_xlate_ready ":ro binds" &&
    guest_cc_report "$V/root/robind" tests/arm64emu/chroot/native/guests/robind.c; then
    ro_legs() { # tag, output
        check_contains "$1 ran to its end" ": 0 failures" "$2"
        check_absent "$1: no leg failed" "-> FAIL" "$2"
        printf '%s\n' "$2" | grep -e '-> FAIL' | sed 's/^/    /'
    }
    for _u in "" -u; do
        for _m in ro rw; do
            rm -rf "$V/rb"; mkdir "$V/rb"
            printf 'RO-DATA' >"$V/rb/f"
            printf 'RO-DATA' >"$V/root/src"
            _s=""
            [ "$_m" = ro ] && _s=":ro"
            # shellcheck disable=SC2086  # $_u is an option or nothing
            out=$(vp $_u -b "$V/rb:/rb$_s" "$V/root" /robind $_m /rb /rb/f /src)
            ro_legs "a $_m bind, every mutating call${_u:+ ($_u)}" "$out"
            [ "$_m" = ro ] && check "...and the file is as it was${_u:+ ($_u)}" RO-DATA \
                "$(cat "$V/rb/f")"
        done
    done

    # A link2symlink group in the bind. Its data file lives in the store under
    # the rootfs, which no bind covers, so a mutator that followed the name
    # arrived at a writable file; to the guest the name is a regular file, and the
    # mount over the NAME is what governs it. Linked while the bind was writable,
    # bound read-only afterwards; ARM64EMU_L2S_FORCE makes the group the
    # emulation's on a host that would have made a real hardlink.
    for _m in ro rw; do
        rm -rf "$V/rb"; mkdir "$V/rb"
        printf 'L2S-DATA' >"$V/rb/f"
        ARM64EMU_L2S_FORCE=1 vp -l -b "$V/rb:/rb" "$V/root" /robind mklink /rb /rb/f
        check "a link2symlink group is made in the bind ($_m)" symlink \
            "$([ -L "$V/rb/f" ] && echo symlink)"
        case "$(readlink "$V/rb/f")" in
        "$V/rb"/* | [!/]*) check "...with its data outside it" outside inside ;;
        esac
        _s=""
        [ "$_m" = ro ] && _s=":ro"
        out=$(ARM64EMU_L2S_FORCE=1 vp -l -b "$V/rb:/rb$_s" "$V/root" /robind "l2s-$_m" /rb /rb/f)
        ro_legs "a link2symlink name in a $_m bind" "$out"
    done

    # A link out of a :ro bind is EXDEV, a bind being a mount, after the new
    # name's own verdict. Made anyway, the new name was a writable way into the
    # :ro file, and under -l the fallback moved the file into the store.
    for _o in "" -l; do
        for _m in ro rw; do
            rm -rf "$V/rb" "$V"/root/rolink.*; mkdir "$V/rb"
            printf 'RO-DATA' >"$V/rb/f"
            _s=""
            [ "$_m" = ro ] && _s=":ro"
            # shellcheck disable=SC2086  # $_o is an option or nothing
            out=$(ARM64EMU_L2S_FORCE=${_o:+1} vp $_o -b "$V/rb:/rb$_s" "$V/root" \
                /robind "link-$_m" /rb /rb/f)
            case "$_m:$_o:$out" in
            rw::*"name: rc=-13"*)
                skip "a link out of a rw bind: this host refuses every hardlink (EACCES)" ;;
            *)
                ro_legs "a link out of a $_m bind${_o:+ ($_o)}" "$out" ;;
            esac
        done
    done
fi
rm -rf "$V"
