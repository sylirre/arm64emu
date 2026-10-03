# M9 Android seccomp-compat net (sourced by tests/run.sh). Validates the SIGSYS
# handler branching via synthetic contexts (real nested seccomp needs a device).
echo "== M9: Android seccomp net =="
run -t nettest >/dev/null 2>&1
check "gate-net + dispatch + passthrough branches" 0 $?
nettest_out=$(run -t nettest 2>/dev/null)
check_contains "gate-trapped reissue -> ENOSYS" "gate-net: regs0=-38" "$nettest_out"
# sigprocmask(how, &m, &m) — one variable for both masks — must not lose the
# requested set to the writability probe, which validates by zeroing.
check_contains "an aliased sigprocmask unblocks only what it named" \
    "nettest sigprocmask aliased: mask=0x800 old=0xa00 -> OK" "$nettest_out"
# ...and the argument checks the kernel makes before it changes anything. The
# call never reaches it, so a wrong sigsetsize passed silently and an out-of-
# range `how` was taken as SIG_SETMASK — replacing the mask, not adding to it.
check_contains "sigprocmask refuses a bad sigsetsize and a bad how" \
    "nettest sigprocmask args: size=-22 kept=1 how=-22 kept=1 -> OK" \
    "$nettest_out"
blocktest_out=$(run -t blocktest 2>&1); blocktest_rc=$?
check "blocklist gates reissue -> ENOSYS" 0 $blocktest_rc
# Android allows accept4 and blocks accept, and glibc and musl issue only the
# latter on AArch64: a blocked accept is made as the accept4 it is, with and
# without an address to fill, and is ENOSYS only when accept4 is blocked too.
check_contains "a blocked accept is made as accept4, which is allowed" \
    "blocktest accept(blocked, accept4 allowed)=" "$blocktest_out"
check_contains "...and the whole of it holds" \
    "both blocked=-38 accept4=" "$blocktest_out"
check_absent "...with no failing leg" "-> FAIL" "$blocktest_out"
