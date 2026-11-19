// The step diagnostics' single-threaded guest (run_tests.sh, "the step
// diagnostics": --max-insn, --reg-trace, ARM64EMU_PROF/RING/TPC/COV).
//
// Libc-free and deterministic: nothing it computes depends on the run (no
// AT_RANDOM-seeded pointer guard, no pid), so the register dump --max-insn N
// prints is the same in every engine and every run, and the harness compares
// them byte for byte. Its shape is what the checks count on:
//
//   _start      2 instructions
//   10 rounds   of 1009: `mov x21` (1), the inner loop, 200 times its 5
//               instructions (1000) -- a block the JIT chains to itself,
//               where a deadline is overrun most easily --, a write of one
//               '.' to stdout (5, the svc the 5th: icount 1008 + 1009 r),
//               the round's count and branch (3)
//   the tail    exit(0)
//
// So each inner-loop instruction retires 2000 times (the profile's top
// counts), and the first instruction past the first inner loop, `mov x0, #1`
// (insn 0xd2800020), runs at icount 1003 -- past the coverage finder's 1000
// instructions of warm-up, and in none of the first 1003.
        .text
        .global _start
_start:
        mov     x19, #0                 // rounds done
        mov     x20, #0x1234            // accumulator
round:
        mov     x21, #200
inner:
        eor     x20, x20, x21
        add     x20, x20, x20, lsl #3
        ror     x20, x20, #7
        subs    x21, x21, #1
        b.ne    inner
        mov     x0, #1                  // write(1, dot, 1)
        adr     x1, dot
        mov     x2, #1
        mov     x8, #64
        svc     #0
        add     x19, x19, #1
        cmp     x19, #10
        b.ne    round
        mov     x0, #0                  // exit(0)
        mov     x8, #93
        svc     #0
dot:    .ascii  "."
