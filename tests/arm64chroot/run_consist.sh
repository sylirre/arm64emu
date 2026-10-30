#!/bin/sh
# JIT-vs-interpreter consistency on random FP inputs. The emulator's FP is
# host-C by design (NaN handling, contraction, conversion corners), so the
# oracle here is the interpreter on the SAME host, not qemu: both engines
# must produce identical bits. Usage: tests/arm64chroot/run_consist.sh [build/arm64chroot]
set -u
EMU=${1:-build/arm64chroot}

cd "$(dirname "$0")/../.."
mkdir -p build/tests/arm64chroot
. tests/arm64chroot/hostenv.sh
[ -n "$AGCC" ] || { echo "SKIP fpconsist: no aarch64 C compiler"; exit 0; }

cflags=$(grep -m1 -o 'BUILDFLAGS:[^*]*' tests/arm64chroot/fpconsist.c | sed 's/^BUILDFLAGS: *//')
"$AGCC" -O2 -static $cflags -o build/tests/arm64chroot/fpconsist.bin tests/arm64chroot/fpconsist.c || {
    # Replaying a pack, $AGCC is the stand-in that succeeds only for binaries
    # the pack shipped, so a failure here means the pack predates fpconsist
    # being packed -- an inventory gap, not a broken build, and not a reason
    # to abort the run before the suite proper. Any real compiler failing is
    # still a failure.
    if [ "${A64_ORACLE:-auto}" = recorded ]; then
        echo "SKIP fpconsist: not in this test pack (re-record with make test-pack)"
        exit 0
    fi
    echo "FAIL build fpconsist"; exit 1; }
a=$("$EMU" / build/tests/arm64chroot/fpconsist.bin) || { echo "FAIL fpconsist (interp rc=$?)"; exit 1; }
b=$("$EMU" --jit / build/tests/arm64chroot/fpconsist.bin) || { echo "FAIL fpconsist (jit rc=$?)"; exit 1; }
if [ "$a" = "$b" ]; then
    echo "PASS fpconsist (jit == interpreter: $a)"
else
    echo "FAIL fpconsist"
    echo "  interp: $a"
    echo "  jit:    $b"
    exit 1
fi
