/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* M37 test: the alignment exclusive, ordered and atomic accesses must have.
 *
 * Without FEAT_LSE2 (ID_AA64MMFR2_EL1.AT is 0 here) every exclusive
 * (LDXR/STXR, their acquire/release and pair forms), every ordered access
 * (LDAR/STLR, LDAPR, LDAPUR/STLUR) and every LSE atomic (LD<op>, SWP, CAS,
 * CASP) takes an Alignment fault -- a data abort, DFSC 0x21 -- when its
 * address is not aligned to the size it accesses, a pair to the whole pair,
 * whatever SCTLR_EL1.A says (AArch64.CheckAlignment). A store-exclusive takes
 * it whatever the monitor holds: the alignment is checked first. The fault
 * says read or write (WnR), an atomic that reads and writes saying write, and
 * FAR is the address the access named.
 *
 * The engines reach these three ways: the interpreter checks in its
 * dispatchers (decode.c access_aligned), the predecoded tier hands them all to
 * it, and the JIT lowers them on one CPU to ordinary probed loads and stores
 * behind an IRO_ALIGN its backends emit. Each misaligned form runs at EL1
 * (EC 0x25), a few at EL0 (EC 0x24), and the same forms run clean aligned and
 * byte-wide. A flat --bin image at EL1, MMU off; scratch at 0x40200000.
 * With the MMU off memory is Device, where every misaligned access faults,
 * which the emulator does not model: no plain access is asserted here, only
 * what Device and Normal memory answer alike.
 *
 * The sync handlers record ESR (x21) and FAR (x22) and skip the instruction;
 * an SVC from EL0 returns to the EL1 continuation in x28. x0=0 on success,
 * else the id of the failing check. */
.arch armv8.4-a

#define LD1 0x96000021                  /* EC 0x25, IL, DFSC 0x21 */
#define ST1 0x96000061                  /* ... and WnR */
#define LD0 0x92000021                  /* EC 0x24: from EL0 */
#define ST0 0x92000061

/* Load the 32-bit constant v into register r. */
.macro LIT r, v
    movz    \r, #((\v) & 0xffff)
    movk    \r, #(((\v) >> 16) & 0xffff), lsl #16
.endm

/* Run insn, whose address is x0 + off; expect the fault esr there, else
 * branch to `to` with x11 = id. */
.macro FAULTS to, id, esr, off, insn:vararg
    movz    x21, #0xffff
    mov     x22, #0
    \insn
    movz    x11, #\id
    LIT     x10, \esr
    cmp     x21, x10
    b.ne    \to
    add     x10, x0, #\off
    cmp     x22, x10
    b.ne    \to
.endm

/* Run insn; expect no fault at all. */
.macro RUNS to, id, insn:vararg
    movz    x21, #0xffff
    \insn
    movz    x11, #\id
    movz    x10, #0xffff
    cmp     x21, x10
    b.ne    \to
.endm

/* At EL1, and at EL0 (where a failed check reports by SVC). */
.macro F id, esr, off, insn:vararg
    FAULTS  fail, \id, \esr, \off, \insn
.endm
.macro N id, insn:vararg
    RUNS    fail, \id, \insn
.endm
.macro F0 id, esr, off, insn:vararg
    FAULTS  el0_fail, \id, \esr, \off, \insn
.endm
.macro N0 id, insn:vararg
    RUNS    el0_fail, \id, \insn
.endm

.text
.global _start
_start:
    movz    x9, #0x4018, lsl #16        /* SP_EL1 = 0x40180000 */
    mov     sp, x9
    movz    x9, #0x4019, lsl #16        /* SP_EL0 = 0x40190000 */
    msr     sp_el0, x9
    movz    x9, #0x4001, lsl #16        /* VBAR = 0x40010000 */
    msr     vbar_el1, x9
    isb
    movz    x0, #0x4020, lsl #16        /* scratch, 64-byte aligned */
    add     x1, x0, #1
    add     x2, x0, #2
    add     x3, x0, #4
    add     x4, x0, #8
    add     x12, x0, #12                /* crosses a 16-byte boundary at 8 bytes */
    movz    x6, #0x55
    movz    x7, #0x66

    /* ---- exclusives ---- */
    clrex
    F       1,  LD1, 1,  ldxrh  w5, [x1]
    F       2,  LD1, 2,  ldxr   w5, [x2]
    F       3,  LD1, 4,  ldaxr  x5, [x3]
    F       4,  LD1, 12, ldxr   x5, [x12]
    F       5,  ST1, 4,  stxr   w8, x6, [x3]        /* no monitor: faults anyway */
    ldxr    x5, [x0]                                /* a monitor on x0 */
    F       6,  ST1, 2,  stlxr  w8, w6, [x2]
    F       7,  ST1, 1,  stxrh  w8, w6, [x1]
    F       8,  LD1, 4,  ldxp   w5, w9, [x3]        /* the pair: 8 bytes */
    F       9,  LD1, 8,  ldaxp  x5, x9, [x4]        /* the pair: 16 bytes */
    F       10, ST1, 8,  stxp   w8, x6, x7, [x4]
    F       11, ST1, 4,  stlxp  w8, w6, w7, [x3]

    /* ---- ordered accesses ---- */
    F       20, LD1, 1,  ldarh  w5, [x1]
    F       21, LD1, 2,  ldar   w5, [x2]
    F       22, LD1, 4,  ldar   x5, [x3]
    F       23, ST1, 1,  stlrh  w6, [x1]
    F       24, ST1, 2,  stlr   w6, [x2]
    F       25, ST1, 4,  stlr   x6, [x3]
    F       26, LD1, 2,  ldapr  w5, [x2]
    F       27, LD1, 4,  ldapr  x5, [x3]
    F       28, LD1, 1,  ldaprh w5, [x1]
    F       29, LD1, 4,  ldapur x5, [x0, #4]
    F       30, LD1, 1,  ldapursh x5, [x0, #1]
    F       31, ST1, 12, stlur  x6, [x0, #12]
    F       32, ST1, 2,  stlur  w6, [x0, #2]

    /* ---- LSE atomics: all fault as writes ---- */
    F       40, ST1, 4,  ldadd  x6, x5, [x3]
    F       41, ST1, 1,  ldaddh w6, w5, [x1]
    F       42, ST1, 2,  stset  w6, [x2]
    F       43, ST1, 4,  swpal  x6, x5, [x3]
    F       44, ST1, 4,  ldumax x6, x5, [x3]
    F       45, ST1, 1,  ldsminh w6, w5, [x1]   /* the JIT's helper form */
    F       46, ST1, 2,  cas    w6, w7, [x2]
    F       47, ST1, 4,  casal  x6, x7, [x3]
    F       48, ST1, 4,  casp   w6, w7, w8, w9, [x3]    /* the pair: 8 bytes */
    F       49, ST1, 8,  casp   x6, x7, x8, x9, [x4]    /* the pair: 16 bytes */

    /* ---- aligned, and byte-wide, they run ---- */
    N       60, ldxrb  w5, [x1]
    N       61, stxrb  w8, w6, [x1]
    N       62, ldarb  w5, [x1]
    N       63, stlrb  w6, [x1]
    N       64, ldaprb w5, [x1]
    N       65, ldapurb w5, [x0, #3]
    N       66, ldaddb w6, w5, [x1]
    N       67, casb   w6, w7, [x1]
    N       68, ldxp   x5, x9, [x0]
    N       69, stxp   w8, x6, x7, [x0]
    N       70, ldar   x5, [x4]
    N       71, ldapr  w5, [x3]
    N       72, stlur  w6, [x0, #4]
    N       73, ldadd  x6, x5, [x4]
    N       74, casp   x6, x7, x8, x9, [x0]
    N       75, ldxp   w5, w9, [x4]
    ldxr    w5, [x3]                        /* aligned: the store succeeds */
    N       77, stxr   w8, w6, [x3]
    movz    x11, #78
    cbnz    w8, fail

    /* ---- from EL0: EC 0x24 ---- */
    adr     x9, el0_code
    msr     elr_el1, x9
    movz    x9, #0x3c0                  /* EL0t, DAIF masked */
    msr     spsr_el1, x9
    adr     x28, el0_back
    eret
el0_back:
    cbnz    x11, fail

    movz    x0, #0
    hlt     #0
fail:
    mov     x0, x11
    hlt     #0

el0_code:
    F0      90, LD0, 4,  ldxr   x5, [x3]
    F0      91, LD0, 4,  ldapr  x5, [x3]
    F0      92, ST0, 2,  stlr   w6, [x2]
    F0      93, LD0, 8,  ldaxp  x5, x9, [x4]
    F0      94, ST0, 4,  ldadd  x6, x5, [x3]
    N0      95, ldxp   x5, x9, [x0]
    movz    x11, #0
    svc     #0
el0_fail:
    svc     #1                          /* x11 names the check */

    /* +0x200: sync from the current EL with SP_ELx. */
    .org    0x10200
    mrs     x21, esr_el1
    mrs     x22, far_el1
    mrs     x23, elr_el1
    add     x23, x23, #4
    msr     elr_el1, x23
    eret

    /* +0x400: sync from a lower EL (AArch64). An SVC ends the EL0
     * excursion at the EL1 continuation in x28; anything else is recorded
     * and skipped. */
    .org    0x10400
    mrs     x21, esr_el1
    lsr     x23, x21, #26
    cmp     x23, #0x15
    b.ne    1f
    br      x28
1:  mrs     x22, far_el1
    mrs     x23, elr_el1
    add     x23, x23, #4
    msr     elr_el1, x23
    eret
