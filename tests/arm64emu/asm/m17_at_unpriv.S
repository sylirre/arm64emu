/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* M17 test: LDTR/STTR unprivileged semantics + AT S1E1R/W, S1E0R/W -> PAR_EL1.
 *
 * MMU on (4 KB granule): L1[1] points at an L2 table with three 2 MB identity
 * blocks — [0] 0x40000000 code/vectors/stack AP=00 (EL1 only), [1] 0x40200000
 * AP=01 (EL0+EL1 RW, "user"), [2] 0x40400000 AP=00 ("priv" data). At EL1:
 *  - LDTR/STTR to the user block must work (incl. LDTRSW sign-extension);
 *  - LDTR/STTR to the priv block must data-abort (EC 0x25) with a permission
 *    DFSC, WnR reflecting the direction — and no base/target side effects;
 *  - the SIMD&FP "unprivileged" encodings (V=1, op2 bits[11:10]=0b10) are
 *    unallocated and must UNDEF (EC 0x00);
 *  - AT S1E1R/S1E0R/S1E1W set PAR_EL1: F=0 + correct PA (+ATTR from MAIR) on
 *    success, F=1 + FST on permission (0x0f) and translation (0x05) failures;
 *  - the unprivileged FEAT_MOPS forms take the EL0 view the same way: SETxT
 *    fills the user block but permission-faults on the priv block (WnR, no
 *    byte written) where a plain SETx succeeds; CPYxWT faults on a priv
 *    destination (WnR), CPYxRT on a priv source (read), and both copy
 *    between user pages.
 * The sync handler records EC (x21) and full ESR (x22), skips the insn.
 * x0=0 on success, else the failing-check id. */

.arch armv9.3-a                         /* FEAT_MOPS (sections 11-15) */
.text
.global _start
_start:
    /* stack (SP_EL1) inside the EL1-only code block */
    movz    x0, #0x4018, lsl #16        /* 0x40180000 */
    mov     sp, x0

    /* exception vectors */
    movz    x0, #0x4001, lsl #16        /* 0x40010000 */
    msr     vbar_el1, x0
    isb

    /* L1 table at 0x40090000: [0] 1 GB identity block (device space, AP=01),
     * [1] -> L2 table at 0x40098000 */
    movz    x1, #0x4009, lsl #16
    movz    x2, #0x0441                 /* block: AF | AP=01 | valid */
    str     x2, [x1]                    /* L1[0] -> PA 0 */
    movz    x3, #0x4009, lsl #16
    movk    x3, #0x8003                 /* 0x40098003: table descriptor */
    str     x3, [x1, #8]

    /* L2 blocks: AF|valid = 0x401; +AP=01 (1<<6) for the user block */
    movz    x4, #0x4009, lsl #16
    movk    x4, #0x8000                 /* L2 base 0x40098000 */
    movz    x5, #0x4000, lsl #16
    movk    x5, #0x0401                 /* L2[0]: 0x40000000 AP=00 */
    str     x5, [x4]
    movz    x5, #0x4020, lsl #16
    movk    x5, #0x0441                 /* L2[1]: 0x40200000 AP=01 */
    str     x5, [x4, #8]
    movz    x5, #0x4040, lsl #16
    movk    x5, #0x0401                 /* L2[2]: 0x40400000 AP=00 */
    str     x5, [x4, #16]

    msr     ttbr0_el1, x1
    movz    x5, #0x00ff                 /* MAIR index0 = Normal WB (0xff) */
    msr     mair_el1, x5

    /* TCR_EL1 = 0x0000000280803519 (T0SZ=25 => start at level 1) */
    movz    x5, #0x3519
    movk    x5, #0x8080, lsl #16
    movk    x5, #0x0002, lsl #32
    msr     tcr_el1, x5
    isb

    mrs     x6, sctlr_el1
    orr     x6, x6, #1                  /* enable MMU */
    msr     sctlr_el1, x6
    isb

    movz    x20, #0x4020, lsl #16       /* user block 0x40200000 */
    movz    x24, #0x4040, lsl #16       /* priv block 0x40400000 */

    /* ---- 1: plain EL1 access to the priv block works ---- */
    movz    x2, #0xdead
    movk    x2, #0xbeef, lsl #16
    str     x2, [x24]
    ldr     x3, [x24]
    movz    x11, #1
    cmp     x3, x2
    b.ne    fail

    /* ---- 2: LDTR/STTR round-trip through the user block ---- */
    movz    x2, #0x1122
    movk    x2, #0x3344, lsl #16
    sttr    x2, [x20, #8]
    ldtr    x3, [x20, #8]
    movz    x11, #2
    cmp     x3, x2
    b.ne    fail

    /* ---- 3: LDTRSW sign-extends ---- */
    movz    x2, #0x0001
    movk    x2, #0x8000, lsl #16        /* 0x80000001 */
    sttr    x2, [x20, #16]              /* 64-bit store; low word is the target */
    ldtrsw  x3, [x20, #16]
    movz    x11, #3
    sxtw    x2, w2                      /* expected: 0xffffffff80000001 */
    cmp     x3, x2
    b.ne    fail

    /* ---- 4: LDTR from the priv block -> DABORT, perm fault, read ---- */
    movz    x21, #0xffff
    mov     x3, #0
    ldtr    x3, [x24]
    movz    x11, #4
    cmp     x21, #0x25                  /* EC_DABORT_SAME */
    b.ne    fail
    movz    x11, #5
    and     x2, x22, #0x3f              /* DFSC: permission fault, level 3 */
    cmp     x2, #0x0f
    b.ne    fail
    movz    x11, #6
    tbnz    x22, #6, fail               /* WnR must be clear on a read */
    movz    x11, #7
    cbnz    x3, fail                    /* target register untouched on fault */

    /* ---- 5: STTR to the priv block -> DABORT with WnR, no memory write ---- */
    movz    x21, #0xffff
    movz    x2, #0x5a5a
    sttr    x2, [x24]
    movz    x11, #8
    cmp     x21, #0x25
    b.ne    fail
    movz    x11, #9
    and     x2, x22, #0x3f
    cmp     x2, #0x0f
    b.ne    fail
    movz    x11, #10
    tbz     x22, #6, fail               /* WnR must be set on a write */
    ldr     x3, [x24]                   /* priv word unchanged (still 0xbeefdead) */
    movz    x2, #0xdead
    movk    x2, #0xbeef, lsl #16
    movz    x11, #11
    cmp     x3, x2
    b.ne    fail

    /* ---- 6: SIMD&FP "unprivileged" encodings are unallocated -> UNDEF ---- */
    movz    x21, #0xffff
    .inst   0xfc400820                  /* would-be "ldtr d0,[x1]" (V=1 mode=2) */
    movz    x11, #12
    cbnz    x21, fail                   /* EC_UNKNOWN == 0 */
    movz    x21, #0xffff
    .inst   0x7c400820                  /* h-sized V=1 mode=2 form */
    movz    x11, #13
    cbnz    x21, fail

    /* ---- 7: AT S1E1R on the priv block: F=0, PA + ATTR correct ---- */
    at      s1e1r, x24
    mrs     x2, par_el1
    movz    x11, #14
    tbnz    x2, #0, fail                /* F==0 */
    movz    x11, #15
    lsr     x3, x2, #12
    and     x3, x3, #0xfffffffff        /* PA[47:12] */
    lsr     x5, x24, #12
    cmp     x3, x5                      /* == 0x40400 (identity) */
    b.ne    fail
    movz    x11, #16
    lsr     x3, x2, #56                 /* ATTR = MAIR byte 0 = 0xff */
    cmp     x3, #0xff
    b.ne    fail

    /* ---- 8: AT S1E0R user block ok; priv block perm-faults ---- */
    at      s1e0r, x20
    mrs     x2, par_el1
    movz    x11, #17
    tbnz    x2, #0, fail                /* F==0 */
    at      s1e0r, x24
    mrs     x2, par_el1
    movz    x11, #18
    tbz     x2, #0, fail                /* F==1 */
    movz    x11, #19
    ubfx    x3, x2, #1, #6              /* FST */
    cmp     x3, #0x0f                   /* permission, level 3 */
    b.ne    fail

    /* ---- 9: AT S1E1W user block ok (AP=01 is EL1-writable) ---- */
    at      s1e1w, x20
    mrs     x2, par_el1
    movz    x11, #20
    tbnz    x2, #0, fail

    /* ---- 10: AT S1E1R on an unmapped VA -> translation fault L1 ---- */
    movz    x25, #0x8000, lsl #16       /* 0x80000000: L1[2] invalid */
    at      s1e1r, x25
    mrs     x2, par_el1
    movz    x11, #21
    tbz     x2, #0, fail                /* F==1 */
    movz    x11, #22
    ubfx    x3, x2, #1, #6
    cmp     x3, #0x05                   /* translation fault, level 1 */
    b.ne    fail

    /* x2 = 0xabababababababab: the fill byte 0xab replicated */
    movz    x2, #0xabab
    movk    x2, #0xabab, lsl #16
    movk    x2, #0xabab, lsl #32
    movk    x2, #0xabab, lsl #48
    mov     x7, #0xab                   /* SETx data register */

    /* ---- 11: SETPT/SETMT/SETET into the user block fill it ---- */
    add     x6, x20, #0x100
    mov     x5, #64
    setpt   [x6]!, x5!, x7
    setmt   [x6]!, x5!, x7
    setet   [x6]!, x5!, x7
    movz    x11, #23
    cbnz    x5, fail                    /* whole size consumed */
    movz    x11, #24
    ldr     x3, [x20, #0x100]
    cmp     x3, x2
    b.ne    fail
    ldr     x3, [x20, #0x138]           /* last doubleword */
    cmp     x3, x2
    b.ne    fail

    /* ---- 12: SETPT into the priv block -> DABORT, perm, WnR, unwritten ---- */
    movz    x21, #0xffff
    add     x6, x24, #0x100
    mov     x5, #64
    setpt   [x6]!, x5!, x7
    movz    x11, #25
    cmp     x21, #0x25
    b.ne    fail
    movz    x11, #26
    and     x3, x22, #0x3f
    cmp     x3, #0x0f
    b.ne    fail
    movz    x11, #27
    tbz     x22, #6, fail               /* WnR set: a store */
    movz    x11, #28
    ldr     x3, [x24, #0x100]           /* nothing written */
    cbnz    x3, fail

    /* ---- 13: plain SETP/SETM/SETE into the priv block works at EL1 ---- */
    add     x6, x24, #0x100
    mov     x5, #64
    setp    [x6]!, x5!, x7
    setm    [x6]!, x5!, x7
    sete    [x6]!, x5!, x7
    movz    x11, #29
    cbnz    x5, fail
    ldr     x3, [x24, #0x138]
    cmp     x3, x2
    b.ne    fail

    /* ---- 14: CPYPWT to a priv destination faults as a store; CPYPRT from
     *          a priv source faults as a load ---- */
    movz    x21, #0xffff
    add     x6, x24, #0x200             /* dest: priv */
    add     x8, x20, #0x100             /* src: user (filled in 11) */
    mov     x5, #64
    cpypwt  [x6]!, [x8]!, x5!
    movz    x11, #30
    cmp     x21, #0x25
    b.ne    fail
    movz    x11, #31
    tbz     x22, #6, fail               /* WnR set */
    movz    x11, #32
    ldr     x3, [x24, #0x200]           /* nothing written */
    cbnz    x3, fail
    movz    x21, #0xffff
    add     x6, x20, #0x200             /* dest: user */
    add     x8, x24, #0x100             /* src: priv (filled in 13) */
    mov     x5, #64
    cpyprt  [x6]!, [x8]!, x5!
    movz    x11, #33
    cmp     x21, #0x25
    b.ne    fail
    movz    x11, #34
    tbnz    x22, #6, fail               /* WnR clear: a load */
    movz    x11, #35
    ldr     x3, [x20, #0x200]           /* nothing written */
    cbnz    x3, fail

    /* ---- 15: CPYPT/CPYMT/CPYET copy between user pages ---- */
    add     x6, x20, #0x200             /* dest: user */
    add     x8, x20, #0x100             /* src: user (filled in 11) */
    mov     x5, #64
    cpypt   [x6]!, [x8]!, x5!
    cpymt   [x6]!, [x8]!, x5!
    cpyet   [x6]!, [x8]!, x5!
    movz    x11, #36
    cbnz    x5, fail
    movz    x11, #37
    ldr     x3, [x20, #0x238]
    cmp     x3, x2
    b.ne    fail

    movz    x0, #0
    hlt     #0
fail:
    mov     x0, x11
    hlt     #0

    /* vector table base = 0x40010000; sync from current EL/SP_ELx at +0x200.
     * Record EC (x21) + full ESR (x22), skip the faulting instruction. */
    .org    0x10200
    mrs     x22, esr_el1
    lsr     x21, x22, #26
    and     x21, x21, #0x3f
    mrs     x23, elr_el1
    add     x23, x23, #4
    msr     elr_el1, x23
    eret
