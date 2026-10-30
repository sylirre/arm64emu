#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Sylirre
# pd-vs-interpreter consistency: run the same deterministic boot to several
# exact --max-insn stopping points (the JIT's interpret-tail makes the stop
# instruction-exact) and require byte-identical serial output AND final CPU
# state (registers, sp, pc, el, nzcv, daif, icount).
#
# Timer-driven IRQ *delivery points* can differ between the modes (the
# interpreter polls devices every 1024 loop iterations, the predecoded tier at
# its slice boundaries), so a guest that reads the timer very late in a long boot may
# print different timestamps while both runs stay individually deterministic
# (see docs/arm64emu/pd.md). The windows below are empirically interleave-identical;
# a divergence here means a bug in a native pd handler. Bisect: binary-search
# --max-insn for the first divergent state, then ARM64EMU_PD_MAX=N (dispatch only PD
# ops <= N natively, the rest through exec_a64; 0 = pure interpreter) to
# isolate the handler class. FP data-processing and system/exception ops are
# always PD_GENERIC, so the suspects are the native integer, branch and
# load/store handlers (see src/arm64emu/jit/predecode.c).
#
# Env: ARM64EMU_BIOS, ARM64EMU_KERNEL, ARM64EMU_INITRD override the images;
#      ARM64EMU_POINTS overrides the checkpoint list;
#      ARM64EMU_TIMEOUT is the per-run wall-clock ceiling (seconds).
set -u
# The runtime default is the host wall clock (ARM64EMU_RTCLOCK=1); pin the deterministic
# instruction-count clock so cross-engine state is reproducible and byte-identical.
# Overridable for debugging.
export ARM64EMU_RTCLOCK="${ARM64EMU_RTCLOCK:-0}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
# ARM64EMU_EMU / ARM64EMU_RUNNER: alternate binary and launcher prefix (qemu-aarch64).
EMU="${ARM64EMU_EMU:-$ROOT/build/arm64emu}"
BIOS=${ARM64EMU_BIOS:-/usr/share/qemu-efi-aarch64/QEMU_EFI.fd}
KERNEL=${ARM64EMU_KERNEL:-$HOME/Image.gz}
INITRD=${ARM64EMU_INITRD:-$HOME/initrd}
POINTS=${ARM64EMU_POINTS:-"1000000 4000000 16000000 64000000 300000000"}
# Ceiling per boot. The deepest checkpoint retires in seconds, so this only ever
# fires on a run that has stopped making progress. It matters more here than in
# the asm suite: these two runs are backgrounded, so without it a wedged boot
# hangs `wait` forever, and killing the harness leaves both emulators behind as
# orphans that sit at 0% CPU asleep in poll(), invisible to any load-based check.
ARM64EMU_TIMEOUT="${ARM64EMU_TIMEOUT:-900}"

[ -x "$EMU" ] || { echo "build arm64emu first"; exit 1; }
[ -r "$BIOS" ] || { echo "SKIP: no firmware at $BIOS"; exit 0; }
[ -r "$KERNEL" ] && [ -r "$INITRD" ] || { echo "SKIP: no kernel/initrd"; exit 0; }

OUT="$(mktemp -d)"; trap 'rm -rf "$OUT"' EXIT
pass=0; fail=0
for N in $POINTS; do
    # Reference = plain interpreter (--no-pd); comparand = the predecoded tier
    # (now the default, so no flag).
    timeout -k 5 "$ARM64EMU_TIMEOUT" ${ARM64EMU_RUNNER:-} "$EMU" --no-pd --bios "$BIOS" --kernel "$KERNEL" --initrd "$INITRD" \
        --append console=ttyAMA0 --max-insn "$N" \
        </dev/null >"$OUT/i.out" 2>"$OUT/i.err" & pi=$!
    timeout -k 5 "$ARM64EMU_TIMEOUT" ${ARM64EMU_RUNNER:-} "$EMU" --bios "$BIOS" --kernel "$KERNEL" --initrd "$INITRD" \
        --append console=ttyAMA0 --max-insn "$N" \
        </dev/null >"$OUT/j.out" 2>"$OUT/j.err" & pj=$!
    wait "$pi"; ri=$?
    wait "$pj"; rj=$?
    if [ "$ri" = 124 ] || [ "$ri" = 137 ] || [ "$rj" = 124 ] || [ "$rj" = 137 ]; then
        echo "FAIL maxinsn=$N (no exit within ${ARM64EMU_TIMEOUT}s)"
        fail=$((fail+1)); continue
    fi
    if cmp -s "$OUT/i.out" "$OUT/j.out" && cmp -s "$OUT/i.err" "$OUT/j.err"; then
        echo "PASS maxinsn=$N (output + state byte-identical)"
        pass=$((pass+1))
    else
        echo "FAIL maxinsn=$N"
        diff "$OUT/i.err" "$OUT/j.err" | head -12
        fail=$((fail+1))
    fi
done
echo "----"
echo "passed=$pass failed=$fail"
[ "$fail" -eq 0 ]
