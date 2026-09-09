/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* M34 self-test: FPCR.FZ / FPCR.FZ16 flush-to-zero.
 *
 * Two separate rules. A denormal OPERAND reads as zero of its own sign as it
 * is unpacked (raising IDC); a RESULT below the smallest normal becomes zero
 * as it is rounded (raising UFC, and only UFC -- the inexactness of a value
 * being replaced by zero is not reported). FZ governs single and double,
 * FZ16 governs half, and each is checked with the other one clear so neither
 * can be reaching past its own precision.
 *
 * Three things here look like mistakes and are not, all confirmed against
 * qemu-aarch64: FZ16 flushes a half operand and raises NO IDC where FZ on a
 * single or double raises one; the precision-changing FCVT family ignores
 * FZ16 entirely (FPUnpackCV/FPRoundCV clear it) while still honoring FZ on
 * its single/double side; and FMUL/FABS/FCMP show that flushing changes the
 * VALUE a guest sees, not merely a status bit.
 *
 * The vector check is the one that pins the flag rule: two lanes flush and a
 * third is an ordinary inexact product, so the instruction must come back
 * with UFC from the first two and IXC from the third -- a flushed lane
 * neither reports its own inexactness nor takes its neighbour's away.
 *
 * Pattern per check: set FPCR, clear FPSR, run exactly one FP operation,
 * read FPSR back before any flag-setting integer compare runs.
 *
 * x0=0 on success, else failing-check id. Builds with -DUSERMODE as a Linux
 * user binary (exit code = x0) so every expected value is validated under
 * qemu-aarch64 -cpu max first. */
.arch armv8.5-a+fp16

.macro CKX reg, val, id
    movz    x11, #\id
    ldr     x9, =\val
    cmp     \reg, x9
    b.ne    fail
.endm

/* FPCR.FZ = bit 24, FPCR.FZ16 = bit 19. Each macro also clears FPSR, so the
 * flags read back afterwards belong to the one operation in between. */
.macro MODE_FZ
    movz    x22, #0x0100, lsl #16
    msr     fpcr, x22
    msr     fpsr, xzr
.endm
.macro MODE_FZ16
    movz    x22, #0x0008, lsl #16
    msr     fpcr, x22
    msr     fpsr, xzr
.endm
.macro RDF
    mrs     x13, fpsr
.endm

.text
.global _start
_start:
    ldr     x20, =ddat
    ldr     x21, =sdat

    /* ---- FZ: denormal operands ---- */
    ldr     d0, [x20, #0]                /* 2^-1074, the smallest subnormal */
    ldr     d1, [x20, #8]                /* +0.0 */
    MODE_FZ
    fadd    d2, d0, d1
    RDF
    fmov    x12, d2
    CKX     x12, 0x0000000000000000, 1   /* operand flushed: +0.0 */
    CKX     x13, 0x80, 2                 /* IDC */

    /* ---- FZ: denormal result, exact ---- */
    ldr     d0, [x20, #16]               /* 2^-1022, the smallest normal */
    ldr     d1, [x20, #24]               /* 0.5 */
    MODE_FZ
    fmul    d2, d0, d1
    RDF
    fmov    x12, d2
    CKX     x12, 0x0000000000000000, 3
    CKX     x13, 0x8, 4                  /* UFC alone: the product was exact */

    /* ---- FZ: denormal result, INEXACT -- still UFC alone ---- */
    ldr     d1, [x20, #32]               /* 0.6 */
    MODE_FZ
    fmul    d2, d0, d1
    RDF
    fmov    x12, d2
    CKX     x12, 0x0000000000000000, 5
    CKX     x13, 0x8, 6                  /* NOT 0x18: no IXC for a discarded value */

    /* ---- FZ: FABS unpacks nothing ---- */
    ldr     d0, [x20, #40]               /* -2^-1074 */
    MODE_FZ
    fabs    d2, d0
    RDF
    fmov    x12, d2
    CKX     x12, 0x0000000000000001, 7   /* subnormal survives */
    CKX     x13, 0, 8

    /* ---- FZ: a flushed operand changes what compares equal ---- */
    ldr     d0, [x20, #0]
    MODE_FZ
    fcmp    d0, #0.0
    mrs     x12, nzcv
    RDF
    lsr     x12, x12, #28
    CKX     x12, 0x6, 9                  /* Z|C: equal */
    CKX     x13, 0x80, 10                /* IDC */

    /* ---- FZ: FP -> int flushes its operand (and is then exact) ---- */
    ldr     d0, [x20, #48]               /* the largest subnormal */
    MODE_FZ
    fcvtzs  x12, d0
    RDF
    CKX     x12, 0, 11
    CKX     x13, 0x80, 12                /* IDC, no IXC */

    /* ---- FZ: FRECPE's own flush branch ---- */
    ldr     d0, [x20, #56]               /* 2^1023: the estimate is subnormal */
    MODE_FZ
    frecpe  d2, d0
    RDF
    fmov    x12, d2
    CKX     x12, 0x0000000000000000, 13
    CKX     x13, 0x8, 14                 /* UFC */

    /* ---- FZ: narrowing FCVT decides tininess on the operand ---- */
    ldr     d0, [x20, #64]               /* 2^-127: subnormal as a single */
    MODE_FZ
    fcvt    s2, d0
    RDF
    fmov    w12, s2
    CKX     x12, 0x00000000, 15
    CKX     x13, 0x8, 16                 /* UFC */

    /* ---- FZ: widening FCVT flushes its single operand ---- */
    ldr     s0, [x21, #0]                /* 2^-149 */
    MODE_FZ
    fcvt    d2, s0
    RDF
    fmov    x12, d2
    CKX     x12, 0x0000000000000000, 17
    CKX     x13, 0x80, 18                /* IDC */

    /* ---- FZ: lanes flag independently ---- */
    ldr     q0, [x21, #32]               /* 2^-126, 2^-126, 1+ulp, 1.0 */
    ldr     q1, [x21, #48]               /* 0.5, 0.6, 0.6, 0.5 */
    MODE_FZ
    fmul    v2.4s, v0.4s, v1.4s
    RDF
    mov     x12, v2.d[0]
    mov     x14, v2.d[1]
    CKX     x12, 0x0000000000000000, 19  /* both flushed */
    CKX     x14, 0x3f0000003f19999b, 20  /* inexact normal, then exact */
    CKX     x13, 0x18, 21                /* UFC from lanes 0-1, IXC from lane 2 */

    /* ---- FZ does not reach half precision ---- */
    ldr     h0, [x21, #64]               /* 2^-24, the smallest half subnormal */
    ldr     h1, [x21, #66]               /* +0.0 */
    MODE_FZ
    fadd    h2, h0, h1
    RDF
    fmov    w12, s2
    and     x12, x12, #0xffff
    CKX     x12, 0x0001, 22              /* untouched */
    CKX     x13, 0, 23

    /* ---- FZ16 flushes it, and raises nothing ---- */
    MODE_FZ16
    fadd    h2, h0, h1
    RDF
    fmov    w12, s2
    and     x12, x12, #0xffff
    CKX     x12, 0x0000, 24
    CKX     x13, 0, 25                   /* NO IDC: half is carved out */

    /* ---- FZ16: a denormal half result ---- */
    ldr     h0, [x21, #68]               /* 2^-14, the smallest normal half */
    ldr     h1, [x21, #70]               /* 0.5 */
    MODE_FZ16
    fmul    h2, h0, h1
    RDF
    fmov    w12, s2
    and     x12, x12, #0xffff
    CKX     x12, 0x0000, 26
    CKX     x13, 0x8, 27                 /* UFC */

    /* ---- FZ16: FABS unpacks nothing here either ---- */
    ldr     h0, [x21, #64]
    MODE_FZ16
    fabs    h2, h0
    RDF
    fmov    w12, s2
    and     x12, x12, #0xffff
    CKX     x12, 0x0001, 28
    CKX     x13, 0, 29

    /* ---- FZ16: FCVT ignores it (FPRoundCV clears the bit) ---- */
    ldr     s0, [x21, #72]               /* 2^-20: subnormal as a half */
    MODE_FZ16
    fcvt    h2, s0
    RDF
    fmov    w12, s2
    and     x12, x12, #0xffff
    CKX     x12, 0x0010, 30              /* the subnormal half, not zero */
    CKX     x13, 0, 31

    /* ---- FZ16 does not reach double precision ---- */
    ldr     d0, [x20, #0]
    ldr     d1, [x20, #8]
    MODE_FZ16
    fadd    d2, d0, d1
    RDF
    fmov    x12, d2
    CKX     x12, 0x0000000000000001, 32  /* untouched */
    CKX     x13, 0, 33

    /* ---- both bits read back ---- */
    movz    x22, #0x0108, lsl #16
    msr     fpcr, x22
    mrs     x12, fpcr
    msr     fpcr, xzr
    CKX     x12, 0x01080000, 34

    msr     fpcr, xzr
    mov     x0, #0
done:
#ifdef USERMODE
    movz    x8, #93                      /* exit(x0) */
    svc     #0
#else
    hlt     #0
#endif
fail:
    msr     fpcr, xzr
    mov     x0, x11
    b       done

.data
.balign 16
ddat:
    .quad 0x0000000000000001             /* +0:  2^-1074 */
    .quad 0x0000000000000000             /* +8:  +0.0 */
    .quad 0x0010000000000000             /* +16: 2^-1022 */
    .quad 0x3FE0000000000000             /* +24: 0.5 */
    .quad 0x3FE3333333333333             /* +32: 0.6 */
    .quad 0x8000000000000001             /* +40: -2^-1074 */
    .quad 0x000FFFFFFFFFFFFF             /* +48: largest subnormal */
    .quad 0x7FE0000000000000             /* +56: 2^1023 */
    .quad 0x3800000000000000             /* +64: 2^-127 */

.balign 16
sdat:
    .word 0x00000001, 0, 0, 0            /* +0:  2^-149f, then pad */
    .word 0, 0, 0, 0                     /* +16: pad */
    .word 0x00800000, 0x00800000, 0x3F800001, 0x3F800000  /* +32: 2^-126 x2, 1+ulp, 1.0 */
    .word 0x3F000000, 0x3F19999A, 0x3F19999A, 0x3F000000  /* +48: 0.5, 0.6, 0.6, 0.5 */
    .hword 0x0001                        /* +64: 2^-24 (half) */
    .hword 0x0000                        /* +66: +0.0 */
    .hword 0x0400                        /* +68: 2^-14 */
    .hword 0x3800                        /* +70: 0.5 */
    .word 0x35800000                     /* +72: 2^-20f */
    .word 0                              /* +76: pad */
