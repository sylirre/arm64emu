/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* M35 test: what EL0 may execute of the system instructions.
 *
 * EL0 used to execute every system instruction as if it were EL1: read and
 * write SCTLR_EL1, VBAR_EL1, TPIDR_EL1, SP_EL0, mask interrupts, run TLBI,
 * ERET to ELR_EL1, HLT the machine, idle it with WFI. Now what EL0 may run is
 * decided architecturally, by the EL1 configuration in SCTLR_EL1 (UCT, DZE,
 * UCI, UMA, nTWI, nTWE) and CNTKCTL_EL1 (EL0VCTEN, EL0PCTEN), and everything
 * else is UNDEFINED at EL0 and taken to EL1 (EC 0x00) -- the ID registers
 * included, which a guest kernel emulates (no FEAT_IDST here) -- while a WFI
 * or WFE that would wait traps (EC 0x01, TI in ISS[1:0]).
 *
 * Two configurations, Linux's and its opposite, run the same EL0 routine
 * (el0_shared) with the expected outcome in registers: under --jit, the
 * routine's block translated under the first must not survive into the
 * second, where it would carry out an inline DC ZVA or counter read EL0 may
 * no longer execute.
 *
 * The lower-EL sync handler records EC (x21) and ESR (x22) and skips the
 * instruction; an SVC returns to the EL1 continuation in x28, with x11 the id
 * of the check EL0 failed (0 = none). x0=0 on success, else that id. */

#define ALLOW   0xffff
#define UNDEF   0x00
#define WFX     0x01

/* At EL0: run insn, expect outcome exp (a constant); on a miss, report id. */
.macro T id, exp, insn:vararg
    movz    x21, #0xffff
    \insn
    movz    x11, #\id
    movz    x10, #\exp
    cmp     x21, x10
    b.ne    el0_fail
.endm

/* At EL0: the same, the expected outcome in register xr. */
.macro TR id, xr, insn:vararg
    movz    x21, #0xffff
    \insn
    movz    x11, #\id
    cmp     x21, \xr
    b.ne    el0_fail
.endm

/* At EL1: run the EL0 code at lbl until it SVCs, then check x11. */
.macro EL0RUN lbl
    adr     x9, \lbl
    msr     elr_el1, x9
    movz    x9, #0x3c0                  /* EL0t, DAIF masked */
    msr     spsr_el1, x9
    adr     x28, 1f
    eret
1:
    cbnz    x11, fail
.endm

.text
.global _start
_start:
    movz    x0, #0x4018, lsl #16        /* SP_EL1 = 0x40180000 */
    mov     sp, x0
    movz    x0, #0x4019, lsl #16        /* SP_EL0 = 0x40190000 */
    msr     sp_el0, x0
    movz    x0, #0x4001, lsl #16        /* VBAR = 0x40010000 */
    msr     vbar_el1, x0
    movz    x1, #0x4020, lsl #16        /* 0x40200000: EL0 scratch line */
    isb

    /* ---- Linux's configuration: DZE, UCT, UCI; no UMA; nTWE but not
     * nTWI; of the counters only the virtual one. ---- */
    movz    x20, #0xc838
    movk    x20, #0x04c4, lsl #16       /* 0x04c4c838 */
    msr     sctlr_el1, x20
    movz    x9, #0x2                    /* EL0VCTEN */
    msr     cntkctl_el1, x9
    isb
    EL0RUN  el0_linux
    mrs     x9, sctlr_el1               /* 40: EL0's MSR changed nothing */
    movz    x11, #40
    cmp     x9, x20
    b.ne    fail

    movz    x12, #ALLOW                 /* DC ZVA */
    movz    x13, #ALLOW                 /* CNTVCT_EL0 */
    movz    x14, #UNDEF                 /* CNTPCT_EL0 */
    movz    x15, #UNDEF                 /* MSR DAIFSet */
    movz    x16, #ALLOW                 /* DC CVAU */
    movz    x17, #ALLOW                 /* CTR_EL0 */
    EL0RUN  el0_shared

    /* ---- the opposite: UMA; no DZE, UCT, UCI; nTWE clear; only the
     * physical counter. ---- */
    movz    x20, #0x0a38
    movk    x20, #0x00c0, lsl #16       /* 0x00c00a38 */
    msr     sctlr_el1, x20
    movz    x9, #0x1                    /* EL0PCTEN */
    msr     cntkctl_el1, x9
    isb
    movz    x12, #UNDEF
    movz    x13, #UNDEF
    movz    x14, #ALLOW
    movz    x15, #ALLOW
    movz    x16, #UNDEF
    movz    x17, #UNDEF
    EL0RUN  el0_shared
    EL0RUN  el0_opposite

    movz    x0, #0
    hlt     #0
fail:
    mov     x0, x11
    hlt     #0

/* ======== EL0 code ======== */

el0_linux:
    T       1,  ALLOW, mrs x0, tpidr_el0
    T       2,  ALLOW, msr tpidr_el0, x0
    T       3,  ALLOW, mrs x0, tpidrro_el0
    T       4,  UNDEF, msr tpidrro_el0, x0
    T       5,  UNDEF, mrs x0, sctlr_el1
    and     x22, x22, #0xffffffff
    movz    x10, #0x0200, lsl #16       /* 41: UNDEFINED is EC 0 with IL, ISS 0 */
    movz    x11, #41
    cmp     x22, x10
    b.ne    el0_fail
    T       6,  UNDEF, msr sctlr_el1, xzr
    T       7,  UNDEF, mrs x0, id_aa64isar0_el1
    T       8,  UNDEF, mrs x0, midr_el1
    T       9,  ALLOW, mrs x0, ctr_el0
    T       10, ALLOW, mrs x0, dczid_el0
    T       11, ALLOW, mrs x0, cntfrq_el0
    T       12, UNDEF, mrs x0, daif
    T       13, UNDEF, mrs x0, currentel
    T       14, UNDEF, mrs x0, tpidr_el1
    T       15, UNDEF, msr tpidr_el1, x0
    T       16, UNDEF, mrs x0, sp_el0
    T       17, UNDEF, msr spsel, #1
    T       18, UNDEF, tlbi vmalle1
    T       19, UNDEF, ic iallu
    T       20, UNDEF, dc ivac, x1
    T       21, ALLOW, ic ivau, x1
    T       22, ALLOW, mrs x0, nzcv
    T       23, ALLOW, msr nzcv, x0
    T       24, ALLOW, mrs x0, fpcr
    T       25, UNDEF, msr cntkctl_el1, xzr
    T       26, UNDEF, msr vbar_el1, xzr
    T       27, UNDEF, eret
    movz    x0, #99                     /* a HLT that stops says 99 */
    T       28, UNDEF, hlt #0
    T       29, WFX,   wfi
    and     x22, x22, #0xffffffff
    movz    x10, #0x07e0, lsl #16       /* 30: EC 1, IL, CV, COND 0xe, TI 0 */
    movz    x11, #30
    cmp     x22, x10
    b.ne    el0_fail
    sevl                                /* 31: WFE with an event pending */
    T       31, ALLOW, wfe
    movz    x11, #0
    svc     #0

el0_shared:
    TR      50, x12, dc zva, x1
    TR      51, x13, mrs x0, cntvct_el0
    TR      52, x14, mrs x0, cntpct_el0
    TR      53, x15, msr daifset, #2
    TR      54, x16, dc cvau, x1
    TR      55, x17, mrs x0, ctr_el0
    movz    x11, #0
    svc     #0

el0_opposite:
    T       60, ALLOW, mrs x0, daif
    T       61, ALLOW, msr daifclr, #2
    T       62, ALLOW, mrs x0, cntfrq_el0
    T       63, UNDEF, ic ivau, x1
    T       64, WFX,   wfe                  /* nTWE clear, no event */
    and     x22, x22, #0xffffffff
    movz    x10, #0x0001
    movk    x10, #0x07e0, lsl #16       /* 65: ...and TI 1 for a WFE */
    movz    x11, #65
    cmp     x22, x10
    b.ne    el0_fail
    movz    x11, #0
    svc     #0

el0_fail:
    svc     #1                          /* x11 names the check */

    /* +0x400: sync from lower EL (AArch64). SVC ends the EL0 excursion at the
     * EL1 continuation in x28; anything else is recorded and skipped. */
    .org    0x10400
    mrs     x22, esr_el1
    lsr     x21, x22, #26
    and     x21, x21, #0x3f
    cmp     x21, #0x15
    b.ne    1f
    br      x28
1:  mrs     x23, elr_el1
    add     x23, x23, #4
    msr     elr_el1, x23
    eret
