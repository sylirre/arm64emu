/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* pd_run: the predecoded interpreter tier, arm64chroot's default engine — a
 * per-thread decode cache of PDEnts (classified by lib/arm64's pd_fill, on a
 * miss) executed by direct-threaded dispatch. The handlers are kept diffable
 * against arm64emu's jit/pdrun.c until the two are merged. */
#include "machine.h"
#include "thread.h"
#include "pdrun.h"

__thread PDEnt g_pdcache[PD_SIZE];

int g_predecode = 1;   /* cleared by -nopd */

/* ---- helpers transcribed from decode.c (which must stay unmodified) ---- */

static inline u64 pd_awc(u64 x, u64 y, int cin, bool is64, u32 *flags) {
    u64 res;
    u32 N, Z, C, V;
    if (is64) {
        u64 t = x + y;
        res = t + (unsigned)cin;
        C = (t < x) | (res < t);
        V = (u32)((~(x ^ y) & (x ^ res)) >> 63);
        N = (u32)(res >> 63);
        Z = (res == 0);
    } else {
        u32 xx = (u32)x, yy = (u32)y;
        u64 u = (u64)xx + (u64)yy + (unsigned)cin;
        res = (u32)u;
        C = (u32)((u >> 32) & 1);
        s64 s = (s64)(s32)xx + (s64)(s32)yy + cin;
        V = ((s64)(s32)(u32)res != s);
        N = (u32)((u32)res >> 31);
        Z = ((u32)res == 0);
    }
    *flags = (N ? PS_N : 0) | (Z ? PS_Z : 0) | (C ? PS_C : 0) | (V ? PS_V : 0);
    return res;
}

static inline void pd_logical_flags(CPU *c, u64 res, bool is64) {
    u32 f = 0;
    if (is64) { if (res >> 63) f |= PS_N; if (res == 0) f |= PS_Z; }
    else { u32 r = (u32)res; if (r >> 31) f |= PS_N; if (r == 0) f |= PS_Z; }
    c->nzcv = f;   /* C=V=0 */
}

static inline u64 pd_shift_reg(u64 v, unsigned type, unsigned amount, bool is64) {
    unsigned w = is64 ? 64 : 32;
    amount &= (w - 1);
    if (!is64) v = (u32)v;
    switch (type) {
        case 0: return v << amount;
        case 1: return is64 ? (v >> amount) : ((u32)v >> amount);
        case 2: return is64 ? (u64)((s64)v >> amount)
                            : (u64)(u32)((s32)(u32)v >> amount);
        default: return is64 ? ror64(v, amount) : ror32((u32)v, amount);
    }
}

static inline u64 pd_extend_reg(u64 v, unsigned option, unsigned shift) {
    u64 out;
    switch (option & 0x3) {
        case 0: out = (u8)v;  break;
        case 1: out = (u16)v; break;
        case 2: out = (u32)v; break;
        default: out = v;     break;
    }
    if (option & 0x4) {
        unsigned bits = (option & 3) == 0 ? 8 : (option & 3) == 1 ? 16 : (option & 3) == 2 ? 32 : 64;
        out = sign_extend(out, bits);
    }
    return out << shift;
}

/* ---- direct-threaded dispatch ----
 * Conventions (identical to exec_a64's): on entry to a handler,
 * c->cur_insn_pc is the instruction address and c->pc is already +4; branch
 * ops overwrite c->pc. Loads write Rt only after the access succeeds; base
 * writeback commits only after all accesses succeed (a faulted instruction
 * re-executes).
 *
 * Each handler ends in NEXT, which fetches/validates/dispatches the next
 * instruction itself (computed goto). Replicating the dispatch jump into
 * every handler gives each one its own branch-predictor slot that learns the
 * guest's instruction sequencing; a single shared switch site mispredicts
 * nearly every dispatch. pd_run executes instructions back-to-back and
 * returns to emu_loop only when something rare needs the loop's attention
 * (recorded exception, pending signal, stop/halt, fetch fault). */
void pd_run(CPU *c) {
    /* Range default + per-id overrides: the override is the point. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverride-init"
    static const void *pd_tab[256] = {
        [0 ... 255] = &&L_GENERIC,
        [PD_NOP] = &&L_NOP,
        [PD_B] = &&L_B,
        [PD_BL] = &&L_BL,
        [PD_BCOND] = &&L_BCOND,
        [PD_CBZ64] = &&L_CBZ64,
        [PD_CBZ32] = &&L_CBZ32,
        [PD_CBNZ64] = &&L_CBNZ64,
        [PD_CBNZ32] = &&L_CBNZ32,
        [PD_TBZ] = &&L_TBZ,
        [PD_TBNZ] = &&L_TBNZ,
        [PD_BR] = &&L_BR,
        [PD_BLR] = &&L_BLR,
        [PD_ADD64I] = &&L_ADD64I,
        [PD_ADD32I] = &&L_ADD32I,
        [PD_SUB64I] = &&L_SUB64I,
        [PD_SUB32I] = &&L_SUB32I,
        [PD_ADDS64I] = &&L_ADDS64I,
        [PD_ADDS32I] = &&L_ADDS32I,
        [PD_SUBS64I] = &&L_SUBS64I,
        [PD_SUBS32I] = &&L_SUBS32I,
        [PD_AND64I] = &&L_AND64I,
        [PD_AND32I] = &&L_AND32I,
        [PD_ORR64I] = &&L_ORR64I,
        [PD_ORR32I] = &&L_ORR32I,
        [PD_EOR64I] = &&L_EOR64I,
        [PD_EOR32I] = &&L_EOR32I,
        [PD_ANDS64I] = &&L_ANDS64I,
        [PD_ANDS32I] = &&L_ANDS32I,
        [PD_MOVI] = &&L_MOVI,
        [PD_MOVK64] = &&L_MOVK64,
        [PD_MOVK32] = &&L_MOVK32,
        [PD_ADR] = &&L_ADR,
        [PD_ADRP] = &&L_ADRP,
        [PD_LSL64I] = &&L_LSL64I,
        [PD_LSL32I] = &&L_LSL32I,
        [PD_LSR64I] = &&L_LSR64I,
        [PD_LSR32I] = &&L_LSR32I,
        [PD_ASR64I] = &&L_ASR64I,
        [PD_ASR32I] = &&L_ASR32I,
        [PD_UBFX64] = &&L_UBFX64,
        [PD_UBFX32] = &&L_UBFX32,
        [PD_UBFIZ64] = &&L_UBFIZ64,
        [PD_UBFIZ32] = &&L_UBFIZ32,
        [PD_SBFX64] = &&L_SBFX64,
        [PD_SBFX32] = &&L_SBFX32,
        [PD_EXTR64] = &&L_EXTR64,
        [PD_EXTR32] = &&L_EXTR32,
        [PD_AND64] = &&L_AND64,
        [PD_AND32] = &&L_AND32,
        [PD_BIC64] = &&L_BIC64,
        [PD_BIC32] = &&L_BIC32,
        [PD_ORR64] = &&L_ORR64,
        [PD_ORR32] = &&L_ORR32,
        [PD_ORN64] = &&L_ORN64,
        [PD_ORN32] = &&L_ORN32,
        [PD_EOR64] = &&L_EOR64,
        [PD_EOR32] = &&L_EOR32,
        [PD_EON64] = &&L_EON64,
        [PD_EON32] = &&L_EON32,
        [PD_ANDS64] = &&L_ANDS64,
        [PD_ANDS32] = &&L_ANDS32,
        [PD_BICS64] = &&L_BICS64,
        [PD_BICS32] = &&L_BICS32,
        [PD_AND64S] = &&L_AND64S,
        [PD_AND32S] = &&L_AND64S,
        [PD_ORR64S] = &&L_AND64S,
        [PD_ORR32S] = &&L_AND64S,
        [PD_EOR64S] = &&L_AND64S,
        [PD_EOR32S] = &&L_AND64S,
        [PD_ANDS64S] = &&L_AND64S,
        [PD_ANDS32S] = &&L_AND64S,
        [PD_ADD64R] = &&L_ADD64R,
        [PD_ADD32R] = &&L_ADD32R,
        [PD_SUB64R] = &&L_SUB64R,
        [PD_SUB32R] = &&L_SUB32R,
        [PD_ADDS64R] = &&L_ADDS64R,
        [PD_ADDS32R] = &&L_ADDS32R,
        [PD_SUBS64R] = &&L_SUBS64R,
        [PD_SUBS32R] = &&L_SUBS32R,
        [PD_ADD64RS] = &&L_ADD64RS,
        [PD_ADD32RS] = &&L_ADD64RS,
        [PD_SUB64RS] = &&L_ADD64RS,
        [PD_SUB32RS] = &&L_ADD64RS,
        [PD_ADDS64RS] = &&L_ADD64RS,
        [PD_ADDS32RS] = &&L_ADD64RS,
        [PD_SUBS64RS] = &&L_ADD64RS,
        [PD_SUBS32RS] = &&L_ADD64RS,
        [PD_ADDX64] = &&L_ADDX64,
        [PD_ADDX32] = &&L_ADDX64,
        [PD_SUBX64] = &&L_ADDX64,
        [PD_SUBX32] = &&L_ADDX64,
        [PD_ADDSX64] = &&L_ADDX64,
        [PD_ADDSX32] = &&L_ADDX64,
        [PD_SUBSX64] = &&L_ADDX64,
        [PD_SUBSX32] = &&L_ADDX64,
        [PD_MADD64] = &&L_MADD64,
        [PD_MADD32] = &&L_MADD32,
        [PD_MSUB64] = &&L_MSUB64,
        [PD_MSUB32] = &&L_MSUB32,
        [PD_SMADDL] = &&L_SMADDL,
        [PD_SMSUBL] = &&L_SMSUBL,
        [PD_UMADDL] = &&L_UMADDL,
        [PD_UMSUBL] = &&L_UMSUBL,
        [PD_SMULH] = &&L_SMULH,
        [PD_UMULH] = &&L_UMULH,
        [PD_CSEL64] = &&L_CSEL64,
        [PD_CSEL32] = &&L_CSEL64,
        [PD_CSINC64] = &&L_CSEL64,
        [PD_CSINC32] = &&L_CSEL64,
        [PD_CSINV64] = &&L_CSEL64,
        [PD_CSINV32] = &&L_CSEL64,
        [PD_CSNEG64] = &&L_CSEL64,
        [PD_CSNEG32] = &&L_CSEL64,
        [PD_CCMP64I] = &&L_CCMP64I,
        [PD_CCMP32I] = &&L_CCMP64I,
        [PD_CCMP64R] = &&L_CCMP64I,
        [PD_CCMP32R] = &&L_CCMP64I,
        [PD_CCMN64I] = &&L_CCMP64I,
        [PD_CCMN32I] = &&L_CCMP64I,
        [PD_CCMN64R] = &&L_CCMP64I,
        [PD_CCMN32R] = &&L_CCMP64I,
        [PD_REV64] = &&L_REV64,
        [PD_REVW] = &&L_REVW,
        [PD_CLZ64] = &&L_CLZ64,
        [PD_CLZ32] = &&L_CLZ32,
        [PD_UDIV64] = &&L_UDIV64,
        [PD_UDIV32] = &&L_UDIV32,
        [PD_SDIV64] = &&L_SDIV64,
        [PD_SDIV32] = &&L_SDIV32,
        [PD_LSLV64] = &&L_LSLV64,
        [PD_LSLV32] = &&L_LSLV32,
        [PD_LSRV64] = &&L_LSRV64,
        [PD_LSRV32] = &&L_LSRV32,
        [PD_ASRV64] = &&L_ASRV64,
        [PD_ASRV32] = &&L_ASRV32,
        [PD_RORV64] = &&L_RORV64,
        [PD_RORV32] = &&L_RORV32,
        [PD_LDR64U] = &&L_LDR64U,
        [PD_LDR32U] = &&L_LDR32U,
        [PD_LDRB] = &&L_LDRB,
        [PD_LDRH] = &&L_LDRH,
        [PD_LDRSB64] = &&L_LDRSB64,
        [PD_LDRSB32] = &&L_LDRSB32,
        [PD_LDRSH64] = &&L_LDRSH64,
        [PD_LDRSH32] = &&L_LDRSH32,
        [PD_LDRSW] = &&L_LDRSW,
        [PD_STR64U] = &&L_STR64U,
        [PD_STR32U] = &&L_STR32U,
        [PD_STRB] = &&L_STRB,
        [PD_STRH] = &&L_STRH,
        [PD_LDR64PRE] = &&L_LDR64PRE,
        [PD_LDR64POST] = &&L_LDR64POST,
        [PD_LDR32PRE] = &&L_LDR32PRE,
        [PD_LDR32POST] = &&L_LDR32POST,
        [PD_STR64PRE] = &&L_STR64PRE,
        [PD_STR64POST] = &&L_STR64POST,
        [PD_STR32PRE] = &&L_STR32PRE,
        [PD_STR32POST] = &&L_STR32POST,
        [PD_LDRBPOST] = &&L_LDRBPOST,
        [PD_STRBPOST] = &&L_STRBPOST,
        [PD_LDR64RO] = &&L_LDR64RO,
        [PD_LDR32RO] = &&L_LDR64RO,
        [PD_LDRBRO] = &&L_LDR64RO,
        [PD_LDRHRO] = &&L_LDR64RO,
        [PD_STR64RO] = &&L_LDR64RO,
        [PD_STR32RO] = &&L_LDR64RO,
        [PD_STRBRO] = &&L_LDR64RO,
        [PD_STRHRO] = &&L_LDR64RO,
        [PD_LDRLIT64] = &&L_LDRLIT64,
        [PD_LDRLIT32] = &&L_LDRLIT32,
        [PD_LDRLITV] = &&L_LDRLITV,
        [PD_LDP64] = &&L_LDP64,
        [PD_LDP64PRE] = &&L_LDP64,
        [PD_LDP64POST] = &&L_LDP64,
        [PD_LDP32] = &&L_LDP64,
        [PD_LDP32PRE] = &&L_LDP64,
        [PD_LDP32POST] = &&L_LDP64,
        [PD_STP64] = &&L_STP64,
        [PD_STP64PRE] = &&L_STP64,
        [PD_STP64POST] = &&L_STP64,
        [PD_STP32] = &&L_STP64,
        [PD_STP32PRE] = &&L_STP64,
        [PD_STP32POST] = &&L_STP64,
        [PD_LDRQ] = &&L_LDRQ,
        [PD_STRQ] = &&L_STRQ,
        [PD_LDRV] = &&L_LDRV,
        [PD_STRV] = &&L_STRV,
        [PD_LDRQPRE] = &&L_LDRQPRE,
        [PD_LDRQPOST] = &&L_LDRQPOST,
        [PD_STRQPRE] = &&L_STRQPRE,
        [PD_STRQPOST] = &&L_STRQPOST,
        [PD_LDPQ] = &&L_LDPQ,
        [PD_LDPQPRE] = &&L_LDPQ,
        [PD_LDPQPOST] = &&L_LDPQ,
        [PD_STPQ] = &&L_STPQ,
        [PD_STPQPRE] = &&L_STPQ,
        [PD_STPQPOST] = &&L_STPQ,
        [PD_LDPD] = &&L_LDPD,
        [PD_LDPDPRE] = &&L_LDPD,
        [PD_LDPDPOST] = &&L_LDPD,
        [PD_STPD] = &&L_STPD,
        [PD_STPDPRE] = &&L_STPD,
        [PD_STPDPOST] = &&L_STPD,
    };
#pragma GCC diagnostic pop
    PDEnt *e;
    u32 insn;

#define NEXT do { \
        c->icount++; \
        if (UNLIKELY(c->stop | c->halted)) return; \
        if (UNLIKELY(g_tls.pend_exc.valid)) return; \
        if (UNLIKELY(g_sig_npend)) return; \
        c->cur_insn_pc = c->pc; \
        if (UNLIKELY(!mem_ifetch(c, c->pc, &insn))) return; \
        c->pc += 4; \
        e = &g_pdcache[(c->cur_insn_pc >> 2) & PD_MASK]; \
        if (UNLIKELY(e->insn != insn)) pd_fill(e, insn); \
        goto *pd_tab[e->op]; \
    } while (0)

    /* Entry: emu_loop has just run the rare-event checks for us. */
    c->cur_insn_pc = c->pc;
    if (UNLIKELY(!mem_ifetch(c, c->pc, &insn))) return;
    c->pc += 4;
    e = &g_pdcache[(c->cur_insn_pc >> 2) & PD_MASK];
    if (UNLIKELY(e->insn != insn)) pd_fill(e, insn);
    goto *pd_tab[e->op];

L_GENERIC:
    exec_a64(c, insn); NEXT;
L_NOP:
    NEXT;

    /* ---- branches ---- */
L_B:
    c->pc = c->cur_insn_pc + e->imm; NEXT;
L_BL:
        set_x(c, 30, c->cur_insn_pc + 4);
        c->pc = c->cur_insn_pc + e->imm;
        NEXT;
L_BCOND:
        if (cond_holds(c, e->rd)) c->pc = c->cur_insn_pc + e->imm;
        NEXT;
L_CBZ64:
    if (reg_x(c, e->rd) == 0) c->pc = c->cur_insn_pc + e->imm;
    NEXT;
L_CBZ32:
    if ((u32)reg_x(c, e->rd) == 0) c->pc = c->cur_insn_pc + e->imm;
    NEXT;
L_CBNZ64:
    if (reg_x(c, e->rd) != 0) c->pc = c->cur_insn_pc + e->imm;
    NEXT;
L_CBNZ32:
    if ((u32)reg_x(c, e->rd) != 0) c->pc = c->cur_insn_pc + e->imm;
    NEXT;
L_TBZ:
    if (!((reg_x(c, e->rd) >> e->rm) & 1)) c->pc = c->cur_insn_pc + e->imm;
    NEXT;
L_TBNZ:
    if (((reg_x(c, e->rd) >> e->rm) & 1)) c->pc = c->cur_insn_pc + e->imm;
    NEXT;
L_BR:
    c->pc = reg_x(c, e->rn); NEXT;
L_BLR:
    {
        u64 tgt = reg_x(c, e->rn);           /* read before writing x30 */
        set_x(c, 30, c->cur_insn_pc + 4);
        c->pc = tgt;
        NEXT;
    }

    /* ---- add/sub immediate ---- */
L_ADD64I:
    set_xsp(c, e->rd, reg_xsp(c, e->rn) + e->imm); NEXT;
L_ADD32I:
    set_xsp(c, e->rd, (u32)(reg_xsp(c, e->rn) + e->imm)); NEXT;
L_SUB64I:
    set_xsp(c, e->rd, reg_xsp(c, e->rn) - e->imm); NEXT;
L_SUB32I:
    set_xsp(c, e->rd, (u32)(reg_xsp(c, e->rn) - e->imm)); NEXT;
L_ADDS64I:
    {
        u32 fl; u64 r = pd_awc(reg_xsp(c, e->rn), e->imm, 0, true, &fl);
        c->nzcv = fl; set_x(c, e->rd, r); NEXT;
    }
L_ADDS32I:
    {
        u32 fl; u64 r = pd_awc(reg_xsp(c, e->rn), e->imm, 0, false, &fl);
        c->nzcv = fl; set_x(c, e->rd, (u32)r); NEXT;
    }
L_SUBS64I:
    {   /* imm stored pre-inverted (~imm) */
        u32 fl; u64 r = pd_awc(reg_xsp(c, e->rn), e->imm, 1, true, &fl);
        c->nzcv = fl; set_x(c, e->rd, r); NEXT;
    }
L_SUBS32I:
    {
        u32 fl; u64 r = pd_awc(reg_xsp(c, e->rn), e->imm, 1, false, &fl);
        c->nzcv = fl; set_x(c, e->rd, (u32)r); NEXT;
    }

    /* ---- logical immediate (imm = wmask, already width-truncated) ---- */
L_AND64I:
    set_xsp(c, e->rd, reg_x(c, e->rn) & e->imm); NEXT;
L_AND32I:
    set_xsp(c, e->rd, (u32)(reg_x(c, e->rn) & e->imm)); NEXT;
L_ORR64I:
    set_xsp(c, e->rd, reg_x(c, e->rn) | e->imm); NEXT;
L_ORR32I:
    set_xsp(c, e->rd, (u32)(reg_x(c, e->rn) | e->imm)); NEXT;
L_EOR64I:
    set_xsp(c, e->rd, reg_x(c, e->rn) ^ e->imm); NEXT;
L_EOR32I:
    set_xsp(c, e->rd, (u32)(reg_x(c, e->rn) ^ e->imm)); NEXT;
L_ANDS64I:
    {
        u64 r = reg_x(c, e->rn) & e->imm;
        pd_logical_flags(c, r, true); set_x(c, e->rd, r); NEXT;
    }
L_ANDS32I:
    {
        u64 r = (u32)(reg_x(c, e->rn) & e->imm);
        pd_logical_flags(c, r, false); set_x(c, e->rd, r); NEXT;
    }

    /* ---- move wide / PC-relative ---- */
L_MOVI:
    set_x(c, e->rd, e->imm); NEXT;   /* MOVZ/MOVN, precomputed */
L_MOVK64:
        set_x(c, e->rd, (reg_x(c, e->rd) & ~(0xffffULL << e->rm)) | e->imm);
        NEXT;
L_MOVK32:
        set_x(c, e->rd, (u32)((reg_x(c, e->rd) & ~(0xffffULL << e->rm)) | e->imm));
        NEXT;
L_ADR:
    set_x(c, e->rd, c->cur_insn_pc + e->imm); NEXT;
L_ADRP:
    set_x(c, e->rd, (c->cur_insn_pc & ~0xfffULL) + e->imm); NEXT;

    /* ---- bitfield aliases ---- */
L_LSL64I:
    set_x(c, e->rd, reg_x(c, e->rn) << e->rm); NEXT;
L_LSL32I:
    set_x(c, e->rd, (u32)(reg_x(c, e->rn) << e->rm)); NEXT;
L_LSR64I:
    set_x(c, e->rd, reg_x(c, e->rn) >> e->rm); NEXT;
L_LSR32I:
    set_x(c, e->rd, (u32)reg_x(c, e->rn) >> e->rm); NEXT;
L_ASR64I:
    set_x(c, e->rd, (u64)((s64)reg_x(c, e->rn) >> e->rm)); NEXT;
L_ASR32I:
    set_x(c, e->rd, (u32)((s32)(u32)reg_x(c, e->rn) >> e->rm)); NEXT;
L_UBFX64:
    set_x(c, e->rd, (reg_x(c, e->rn) >> e->rm) & e->imm); NEXT;
L_UBFX32:
    set_x(c, e->rd, (reg_x(c, e->rn) >> e->rm) & e->imm); NEXT;
L_UBFIZ64:
    set_x(c, e->rd, (reg_x(c, e->rn) & e->imm) << e->rm); NEXT;
L_UBFIZ32:
    set_x(c, e->rd, (u32)((reg_x(c, e->rn) & e->imm) << e->rm)); NEXT;
L_SBFX64:
    /* rm = left shift, imm = arithmetic right shift */
        set_x(c, e->rd, (u64)((s64)(reg_x(c, e->rn) << e->rm) >> e->imm));
        NEXT;
L_SBFX32:
        set_x(c, e->rd, (u32)((s32)((u32)reg_x(c, e->rn) << e->rm) >> e->imm));
        NEXT;
L_EXTR64:
    {
        u64 hi = reg_x(c, e->rn), lo = reg_x(c, e->rm);
        set_x(c, e->rd, e->imm ? ((hi << (64 - e->imm)) | (lo >> e->imm)) : lo);
        NEXT;
    }
L_EXTR32:
    {
        u32 h = (u32)reg_x(c, e->rn), l = (u32)reg_x(c, e->rm);
        set_x(c, e->rd, e->imm ? (u32)((h << (32 - e->imm)) | (l >> e->imm)) : l);
        NEXT;
    }

    /* ---- logical shifted register, LSL #0 ---- */
L_AND64:
    set_x(c, e->rd, reg_x(c, e->rn) & reg_x(c, e->rm)); NEXT;
L_AND32:
    set_x(c, e->rd, (u32)(reg_x(c, e->rn) & reg_x(c, e->rm))); NEXT;
L_BIC64:
    set_x(c, e->rd, reg_x(c, e->rn) & ~reg_x(c, e->rm)); NEXT;
L_BIC32:
    set_x(c, e->rd, (u32)(reg_x(c, e->rn) & ~reg_x(c, e->rm))); NEXT;
L_ORR64:
    set_x(c, e->rd, reg_x(c, e->rn) | reg_x(c, e->rm)); NEXT;
L_ORR32:
    set_x(c, e->rd, (u32)(reg_x(c, e->rn) | reg_x(c, e->rm))); NEXT;
L_ORN64:
    set_x(c, e->rd, reg_x(c, e->rn) | ~reg_x(c, e->rm)); NEXT;
L_ORN32:
    set_x(c, e->rd, (u32)(reg_x(c, e->rn) | ~reg_x(c, e->rm))); NEXT;
L_EOR64:
    set_x(c, e->rd, reg_x(c, e->rn) ^ reg_x(c, e->rm)); NEXT;
L_EOR32:
    set_x(c, e->rd, (u32)(reg_x(c, e->rn) ^ reg_x(c, e->rm))); NEXT;
L_EON64:
    set_x(c, e->rd, reg_x(c, e->rn) ^ ~reg_x(c, e->rm)); NEXT;
L_EON32:
    set_x(c, e->rd, (u32)(reg_x(c, e->rn) ^ ~reg_x(c, e->rm))); NEXT;
L_ANDS64:
    {
        u64 r = reg_x(c, e->rn) & reg_x(c, e->rm);
        pd_logical_flags(c, r, true); set_x(c, e->rd, r); NEXT;
    }
L_ANDS32:
    {
        u64 r = (u32)(reg_x(c, e->rn) & reg_x(c, e->rm));
        pd_logical_flags(c, r, false); set_x(c, e->rd, r); NEXT;
    }
L_BICS64:
    {
        u64 r = reg_x(c, e->rn) & ~reg_x(c, e->rm);
        pd_logical_flags(c, r, true); set_x(c, e->rd, r); NEXT;
    }
L_BICS32:
    {
        u64 r = (u32)(reg_x(c, e->rn) & ~reg_x(c, e->rm));
        pd_logical_flags(c, r, false); set_x(c, e->rd, r); NEXT;
    }

    /* ---- logical shifted register with shift (imm = N<<8|type<<6|amt) ---- */
L_AND64S:
    {
        bool is64 = (e->op == PD_AND64S || e->op == PD_ORR64S ||
                     e->op == PD_EOR64S || e->op == PD_ANDS64S);
        u64 op2 = pd_shift_reg(reg_x(c, e->rm), (unsigned)(e->imm >> 6) & 3,
                               (unsigned)e->imm & 63, is64);
        if (e->imm & 256) op2 = ~op2;
        u64 n = reg_x(c, e->rn), r;
        if (e->op == PD_AND64S || e->op == PD_AND32S ||
            e->op == PD_ANDS64S || e->op == PD_ANDS32S) r = n & op2;
        else if (e->op == PD_ORR64S || e->op == PD_ORR32S) r = n | op2;
        else r = n ^ op2;
        if (!is64) r = (u32)r;
        if (e->op == PD_ANDS64S || e->op == PD_ANDS32S) pd_logical_flags(c, r, is64);
        set_x(c, e->rd, r);
        NEXT;
    }

    /* ---- add/sub shifted register, LSL #0 ---- */
L_ADD64R:
    set_x(c, e->rd, reg_x(c, e->rn) + reg_x(c, e->rm)); NEXT;
L_ADD32R:
    set_x(c, e->rd, (u32)(reg_x(c, e->rn) + reg_x(c, e->rm))); NEXT;
L_SUB64R:
    set_x(c, e->rd, reg_x(c, e->rn) - reg_x(c, e->rm)); NEXT;
L_SUB32R:
    set_x(c, e->rd, (u32)(reg_x(c, e->rn) - reg_x(c, e->rm))); NEXT;
L_ADDS64R:
    {
        u32 fl; u64 r = pd_awc(reg_x(c, e->rn), reg_x(c, e->rm), 0, true, &fl);
        c->nzcv = fl; set_x(c, e->rd, r); NEXT;
    }
L_ADDS32R:
    {
        u32 fl; u64 r = pd_awc(reg_x(c, e->rn), reg_x(c, e->rm), 0, false, &fl);
        c->nzcv = fl; set_x(c, e->rd, (u32)r); NEXT;
    }
L_SUBS64R:
    {
        u32 fl; u64 r = pd_awc(reg_x(c, e->rn), ~reg_x(c, e->rm), 1, true, &fl);
        c->nzcv = fl; set_x(c, e->rd, r); NEXT;
    }
L_SUBS32R:
    {
        u32 fl; u64 r = pd_awc(reg_x(c, e->rn), ~reg_x(c, e->rm), 1, false, &fl);
        c->nzcv = fl; set_x(c, e->rd, (u32)r); NEXT;
    }

    /* ---- add/sub shifted register with shift (imm = type<<6|amt) ---- */
L_ADD64RS:
    {
        bool is64 = (e->op == PD_ADD64RS || e->op == PD_SUB64RS ||
                     e->op == PD_ADDS64RS || e->op == PD_SUBS64RS);
        bool sub = (e->op == PD_SUB64RS || e->op == PD_SUB32RS ||
                    e->op == PD_SUBS64RS || e->op == PD_SUBS32RS);
        bool S = (e->op == PD_ADDS64RS || e->op == PD_ADDS32RS ||
                  e->op == PD_SUBS64RS || e->op == PD_SUBS32RS);
        u64 op2 = pd_shift_reg(reg_x(c, e->rm), (unsigned)(e->imm >> 6) & 3,
                               (unsigned)e->imm & 63, is64);
        u32 fl;
        u64 n = reg_x(c, e->rn);
        u64 r = sub ? pd_awc(n, ~op2, 1, is64, &fl)
                    : pd_awc(n, op2, 0, is64, &fl);
        if (S) c->nzcv = fl;
        set_x_sz(c, e->rd, is64, r);
        NEXT;
    }

    /* ---- add/sub extended register (imm = option<<3|imm3) ---- */
L_ADDX64:
    {
        bool is64 = (e->op == PD_ADDX64 || e->op == PD_SUBX64 ||
                     e->op == PD_ADDSX64 || e->op == PD_SUBSX64);
        bool sub = (e->op == PD_SUBX64 || e->op == PD_SUBX32 ||
                    e->op == PD_SUBSX64 || e->op == PD_SUBSX32);
        bool S = (e->op == PD_ADDSX64 || e->op == PD_ADDSX32 ||
                  e->op == PD_SUBSX64 || e->op == PD_SUBSX32);
        u64 op2 = pd_extend_reg(reg_x(c, e->rm), (unsigned)(e->imm >> 3) & 7,
                                (unsigned)e->imm & 7);
        u64 n = reg_xsp(c, e->rn);
        u32 fl;
        u64 r = sub ? pd_awc(n, ~op2, 1, is64, &fl)
                    : pd_awc(n, op2, 0, is64, &fl);
        if (S) { c->nzcv = fl; set_x_sz(c, e->rd, is64, r); }
        else set_xsp(c, e->rd, is64 ? r : (u32)r);
        NEXT;
    }

    /* ---- 3-source ---- */
L_MADD64:
        set_x(c, e->rd, reg_x(c, (unsigned)e->imm) + reg_x(c, e->rn) * reg_x(c, e->rm));
        NEXT;
L_MADD32:
        set_x(c, e->rd, (u32)(reg_x(c, (unsigned)e->imm) + reg_x(c, e->rn) * reg_x(c, e->rm)));
        NEXT;
L_MSUB64:
        set_x(c, e->rd, reg_x(c, (unsigned)e->imm) - reg_x(c, e->rn) * reg_x(c, e->rm));
        NEXT;
L_MSUB32:
        set_x(c, e->rd, (u32)(reg_x(c, (unsigned)e->imm) - reg_x(c, e->rn) * reg_x(c, e->rm)));
        NEXT;
L_SMADDL:
        set_x(c, e->rd, reg_x(c, (unsigned)e->imm) +
              (u64)((s64)(s32)(u32)reg_x(c, e->rn) * (s64)(s32)(u32)reg_x(c, e->rm)));
        NEXT;
L_SMSUBL:
        set_x(c, e->rd, reg_x(c, (unsigned)e->imm) -
              (u64)((s64)(s32)(u32)reg_x(c, e->rn) * (s64)(s32)(u32)reg_x(c, e->rm)));
        NEXT;
L_UMADDL:
        set_x(c, e->rd, reg_x(c, (unsigned)e->imm) +
              (u64)((u64)(u32)reg_x(c, e->rn) * (u64)(u32)reg_x(c, e->rm)));
        NEXT;
L_UMSUBL:
        set_x(c, e->rd, reg_x(c, (unsigned)e->imm) -
              (u64)((u64)(u32)reg_x(c, e->rn) * (u64)(u32)reg_x(c, e->rm)));
        NEXT;
L_SMULH:
        set_x(c, e->rd, (u64)smulh64((s64)reg_x(c, e->rn), (s64)reg_x(c, e->rm)));
        NEXT;
L_UMULH:
        set_x(c, e->rd, umulh64(reg_x(c, e->rn), reg_x(c, e->rm)));
        NEXT;

    /* ---- conditional select (imm = cond) ---- */
L_CSEL64:
    {
        bool is64 = (e->op == PD_CSEL64 || e->op == PD_CSINC64 ||
                     e->op == PD_CSINV64 || e->op == PD_CSNEG64);
        u64 r;
        if (cond_holds(c, (unsigned)e->imm)) r = reg_x(c, e->rn);
        else {
            u64 m = reg_x(c, e->rm);
            if (e->op == PD_CSEL64 || e->op == PD_CSEL32) r = m;
            else if (e->op == PD_CSINC64 || e->op == PD_CSINC32) r = m + 1;
            else if (e->op == PD_CSINV64 || e->op == PD_CSINV32) r = ~m;
            else r = 0 - m;                          /* CSNEG: unsigned
                                                      * negation, so the
                                                      * INT64_MIN operand is
                                                      * arithmetic and not
                                                      * signed overflow */
        }
        set_x_sz(c, e->rd, is64, r);
        NEXT;
    }

    /* ---- conditional compare (imm = cond | imm5<<8 | flags<<32) ---- */
L_CCMP64I:
    {
        bool is64 = (e->op == PD_CCMP64I || e->op == PD_CCMP64R ||
                     e->op == PD_CCMN64I || e->op == PD_CCMN64R);
        bool cmp = (e->op == PD_CCMP64I || e->op == PD_CCMP32I ||
                    e->op == PD_CCMP64R || e->op == PD_CCMP32R);
        bool is_imm = (e->op == PD_CCMP64I || e->op == PD_CCMP32I ||
                       e->op == PD_CCMN64I || e->op == PD_CCMN32I);
        if (cond_holds(c, (unsigned)e->imm & 15)) {
            u64 n = reg_x(c, e->rn);
            u64 m = is_imm ? ((e->imm >> 8) & 31) : reg_x(c, e->rm);
            u32 fl;
            if (cmp) pd_awc(n, ~m, 1, is64, &fl);
            else pd_awc(n, m, 0, is64, &fl);
            c->nzcv = fl;
        } else {
            c->nzcv = (u32)(e->imm >> 32);   /* precomputed flags word */
        }
        NEXT;
    }

    /* ---- 1-source (hot subset) ---- */
L_REV64:
    set_x(c, e->rd, __builtin_bswap64(reg_x(c, e->rn))); NEXT;
L_REVW:
    set_x(c, e->rd, __builtin_bswap32((u32)reg_x(c, e->rn))); NEXT;
L_CLZ64:
    {
        u64 n = reg_x(c, e->rn);
        set_x(c, e->rd, n ? (u64)__builtin_clzll(n) : 64);
        NEXT;
    }
L_CLZ32:
    {
        u32 n = (u32)reg_x(c, e->rn);
        set_x(c, e->rd, n ? (u32)__builtin_clz(n) : 32);
        NEXT;
    }

    /* ---- 2-source ---- */
L_UDIV64:
    {
        u64 n = reg_x(c, e->rn), m = reg_x(c, e->rm);
        set_x(c, e->rd, m == 0 ? 0 : n / m);
        NEXT;
    }
L_UDIV32:
    {
        u32 a = (u32)reg_x(c, e->rn), b = (u32)reg_x(c, e->rm);
        set_x(c, e->rd, b ? a / b : 0);
        NEXT;
    }
L_SDIV64:
    {
        s64 a = (s64)reg_x(c, e->rn), b = (s64)reg_x(c, e->rm);
        set_x(c, e->rd, (b == 0) ? 0 : (u64)((b == -1 && a == INT64_MIN) ? a : a / b));
        NEXT;
    }
L_SDIV32:
    {
        s32 a = (s32)(u32)reg_x(c, e->rn), b = (s32)(u32)reg_x(c, e->rm);
        set_x(c, e->rd, b ? (u32)((b == -1 && a == INT32_MIN) ? a : a / b) : 0);
        NEXT;
    }
L_LSLV64:
    set_x(c, e->rd, pd_shift_reg(reg_x(c, e->rn), 0, (unsigned)reg_x(c, e->rm), true)); NEXT;
L_LSLV32:
    set_x(c, e->rd, (u32)pd_shift_reg(reg_x(c, e->rn), 0, (unsigned)reg_x(c, e->rm), false)); NEXT;
L_LSRV64:
    set_x(c, e->rd, pd_shift_reg(reg_x(c, e->rn), 1, (unsigned)reg_x(c, e->rm), true)); NEXT;
L_LSRV32:
    set_x(c, e->rd, (u32)pd_shift_reg(reg_x(c, e->rn), 1, (unsigned)reg_x(c, e->rm), false)); NEXT;
L_ASRV64:
    set_x(c, e->rd, pd_shift_reg(reg_x(c, e->rn), 2, (unsigned)reg_x(c, e->rm), true)); NEXT;
L_ASRV32:
    set_x(c, e->rd, (u32)pd_shift_reg(reg_x(c, e->rn), 2, (unsigned)reg_x(c, e->rm), false)); NEXT;
L_RORV64:
    set_x(c, e->rd, pd_shift_reg(reg_x(c, e->rn), 3, (unsigned)reg_x(c, e->rm), true)); NEXT;
L_RORV32:
    set_x(c, e->rd, (u32)pd_shift_reg(reg_x(c, e->rn), 3, (unsigned)reg_x(c, e->rm), false)); NEXT;

    /* ---- integer load/store, va = base + imm ---- */
L_LDR64U:
    {
        u64 va = reg_xsp(c, e->rn) + e->imm, v;
        if (mem_read(c, va, 8, &v)) set_x(c, e->rd, v);
        NEXT;
    }
L_LDR32U:
    {
        u64 va = reg_xsp(c, e->rn) + e->imm, v;
        if (mem_read(c, va, 4, &v)) set_x(c, e->rd, (u32)v);
        NEXT;
    }
L_LDRB:
    {
        u64 va = reg_xsp(c, e->rn) + e->imm, v;
        if (mem_read(c, va, 1, &v)) set_x(c, e->rd, v);
        NEXT;
    }
L_LDRH:
    {
        u64 va = reg_xsp(c, e->rn) + e->imm, v;
        if (mem_read(c, va, 2, &v)) set_x(c, e->rd, v);
        NEXT;
    }
L_LDRSB64:
    {
        u64 va = reg_xsp(c, e->rn) + e->imm, v;
        if (mem_read(c, va, 1, &v)) set_x(c, e->rd, sign_extend(v, 8));
        NEXT;
    }
L_LDRSB32:
    {
        u64 va = reg_xsp(c, e->rn) + e->imm, v;
        if (mem_read(c, va, 1, &v)) set_x(c, e->rd, (u32)sign_extend(v, 8));
        NEXT;
    }
L_LDRSH64:
    {
        u64 va = reg_xsp(c, e->rn) + e->imm, v;
        if (mem_read(c, va, 2, &v)) set_x(c, e->rd, sign_extend(v, 16));
        NEXT;
    }
L_LDRSH32:
    {
        u64 va = reg_xsp(c, e->rn) + e->imm, v;
        if (mem_read(c, va, 2, &v)) set_x(c, e->rd, (u32)sign_extend(v, 16));
        NEXT;
    }
L_LDRSW:
    {
        u64 va = reg_xsp(c, e->rn) + e->imm, v;
        if (mem_read(c, va, 4, &v)) set_x(c, e->rd, sign_extend(v, 32));
        NEXT;
    }
L_STR64U:
    mem_write(c, reg_xsp(c, e->rn) + e->imm, 8, reg_x(c, e->rd)); NEXT;
L_STR32U:
    mem_write(c, reg_xsp(c, e->rn) + e->imm, 4, reg_x(c, e->rd)); NEXT;
L_STRB:
    mem_write(c, reg_xsp(c, e->rn) + e->imm, 1, reg_x(c, e->rd)); NEXT;
L_STRH:
    mem_write(c, reg_xsp(c, e->rn) + e->imm, 2, reg_x(c, e->rd)); NEXT;

    /* ---- pre/post-indexed (writeback commits only after success) ---- */
L_LDR64PRE:
    {
        u64 va = reg_xsp(c, e->rn) + e->imm, v;
        if (!mem_read(c, va, 8, &v)) NEXT;
        set_x(c, e->rd, v); set_xsp(c, e->rn, va);
        NEXT;
    }
L_LDR64POST:
    {
        u64 base = reg_xsp(c, e->rn), v;
        if (!mem_read(c, base, 8, &v)) NEXT;
        set_x(c, e->rd, v); set_xsp(c, e->rn, base + e->imm);
        NEXT;
    }
L_LDR32PRE:
    {
        u64 va = reg_xsp(c, e->rn) + e->imm, v;
        if (!mem_read(c, va, 4, &v)) NEXT;
        set_x(c, e->rd, (u32)v); set_xsp(c, e->rn, va);
        NEXT;
    }
L_LDR32POST:
    {
        u64 base = reg_xsp(c, e->rn), v;
        if (!mem_read(c, base, 4, &v)) NEXT;
        set_x(c, e->rd, (u32)v); set_xsp(c, e->rn, base + e->imm);
        NEXT;
    }
L_STR64PRE:
    {
        u64 va = reg_xsp(c, e->rn) + e->imm;
        if (!mem_write(c, va, 8, reg_x(c, e->rd))) NEXT;
        set_xsp(c, e->rn, va);
        NEXT;
    }
L_STR64POST:
    {
        u64 base = reg_xsp(c, e->rn);
        if (!mem_write(c, base, 8, reg_x(c, e->rd))) NEXT;
        set_xsp(c, e->rn, base + e->imm);
        NEXT;
    }
L_STR32PRE:
    {
        u64 va = reg_xsp(c, e->rn) + e->imm;
        if (!mem_write(c, va, 4, reg_x(c, e->rd))) NEXT;
        set_xsp(c, e->rn, va);
        NEXT;
    }
L_STR32POST:
    {
        u64 base = reg_xsp(c, e->rn);
        if (!mem_write(c, base, 4, reg_x(c, e->rd))) NEXT;
        set_xsp(c, e->rn, base + e->imm);
        NEXT;
    }
L_LDRBPOST:
    {
        u64 base = reg_xsp(c, e->rn), v;
        if (!mem_read(c, base, 1, &v)) NEXT;
        set_x(c, e->rd, v); set_xsp(c, e->rn, base + e->imm);
        NEXT;
    }
L_STRBPOST:
    {
        u64 base = reg_xsp(c, e->rn);
        if (!mem_write(c, base, 1, reg_x(c, e->rd))) NEXT;
        set_xsp(c, e->rn, base + e->imm);
        NEXT;
    }

    /* ---- register offset (imm = option<<3|shift) ---- */
L_LDR64RO:
    {
        u64 va = reg_xsp(c, e->rn) +
                 pd_extend_reg(reg_x(c, e->rm), (unsigned)(e->imm >> 3) & 7,
                               (unsigned)e->imm & 7);
        u64 v;
        if (e->op == PD_LDR64RO) { if (mem_read(c, va, 8, &v)) set_x(c, e->rd, v); }
        else if (e->op == PD_LDR32RO) { if (mem_read(c, va, 4, &v)) set_x(c, e->rd, (u32)v); }
        else if (e->op == PD_LDRBRO) { if (mem_read(c, va, 1, &v)) set_x(c, e->rd, v); }
        else if (e->op == PD_LDRHRO) { if (mem_read(c, va, 2, &v)) set_x(c, e->rd, v); }
        else if (e->op == PD_STR64RO) mem_write(c, va, 8, reg_x(c, e->rd));
        else if (e->op == PD_STR32RO) mem_write(c, va, 4, reg_x(c, e->rd));
        else if (e->op == PD_STRBRO) mem_write(c, va, 1, reg_x(c, e->rd));
        else mem_write(c, va, 2, reg_x(c, e->rd));
        NEXT;
    }

    /* ---- literal ---- */
L_LDRLIT64:
    {
        u64 v;
        if (mem_read(c, c->cur_insn_pc + e->imm, 8, &v)) set_x(c, e->rd, v);
        NEXT;
    }
L_LDRLIT32:
    {
        u64 v;
        if (mem_read(c, c->cur_insn_pc + e->imm, 4, &v)) set_x(c, e->rd, (u32)v);
        NEXT;
    }
L_LDRLITV:
    {   /* SIMD&FP literal: rm = byte count 4/8/16; zero-extends into V[rd] */
        V128 v; v.d[0] = 0; v.d[1] = 0;
        u64 va = c->cur_insn_pc + e->imm;
        if (e->rm == 16) { if (mem_read128(c, va, &v)) c->v[e->rd] = v; }
        else { u64 t; if (mem_read(c, va, e->rm, &t)) { v.d[0] = t; c->v[e->rd] = v; } }
        NEXT;
    }

    /* ---- integer pairs (rm = Rt2; both loads land before either write) ---- */
L_LDP64:
    {
        bool is64 = (e->op == PD_LDP64 || e->op == PD_LDP64PRE || e->op == PD_LDP64POST);
        bool post = (e->op == PD_LDP64POST || e->op == PD_LDP32POST);
        bool wb = post || e->op == PD_LDP64PRE || e->op == PD_LDP32PRE;
        unsigned esz = is64 ? 8 : 4;
        u64 base = reg_xsp(c, e->rn);
        u64 addr = post ? base : base + e->imm;
        u64 a, b;
        if (!mem_read(c, addr, esz, &a)) NEXT;
        if (!mem_read(c, addr + esz, esz, &b)) NEXT;
        if (is64) { set_x(c, e->rd, a); set_x(c, e->rm, b); }
        else { set_x(c, e->rd, (u32)a); set_x(c, e->rm, (u32)b); }
        if (wb) set_xsp(c, e->rn, post ? base + e->imm : addr);
        NEXT;
    }
L_STP64:
    {
        bool is64 = (e->op == PD_STP64 || e->op == PD_STP64PRE || e->op == PD_STP64POST);
        bool post = (e->op == PD_STP64POST || e->op == PD_STP32POST);
        bool wb = post || e->op == PD_STP64PRE || e->op == PD_STP32PRE;
        unsigned esz = is64 ? 8 : 4;
        u64 base = reg_xsp(c, e->rn);
        u64 addr = post ? base : base + e->imm;
        if (!mem_write(c, addr, esz, reg_x(c, e->rd))) NEXT;
        if (!mem_write(c, addr + esz, esz, reg_x(c, e->rm))) NEXT;
        if (wb) set_xsp(c, e->rn, post ? base + e->imm : addr);
        NEXT;
    }

    /* ---- FP/SIMD register load/store (rd = Vt) ---- */
L_LDRQ:
    {
        V128 v;
        if (mem_read128(c, reg_xsp(c, e->rn) + e->imm, &v)) c->v[e->rd] = v;
        NEXT;
    }
L_STRQ:
    mem_write128(c, reg_xsp(c, e->rn) + e->imm, &c->v[e->rd]); NEXT;
L_LDRV:
    {   /* rm = byte count 1/2/4/8; high half cleared */
        u64 t;
        if (mem_read(c, reg_xsp(c, e->rn) + e->imm, e->rm, &t)) {
            c->v[e->rd].d[0] = t;
            c->v[e->rd].d[1] = 0;
        }
        NEXT;
    }
L_STRV:
    mem_write(c, reg_xsp(c, e->rn) + e->imm, e->rm, c->v[e->rd].d[0]); NEXT;
L_LDRQPRE:
    {
        u64 va = reg_xsp(c, e->rn) + e->imm;
        V128 v;
        if (!mem_read128(c, va, &v)) NEXT;
        c->v[e->rd] = v; set_xsp(c, e->rn, va);
        NEXT;
    }
L_LDRQPOST:
    {
        u64 base = reg_xsp(c, e->rn);
        V128 v;
        if (!mem_read128(c, base, &v)) NEXT;
        c->v[e->rd] = v; set_xsp(c, e->rn, base + e->imm);
        NEXT;
    }
L_STRQPRE:
    {
        u64 va = reg_xsp(c, e->rn) + e->imm;
        if (!mem_write128(c, va, &c->v[e->rd])) NEXT;
        set_xsp(c, e->rn, va);
        NEXT;
    }
L_STRQPOST:
    {
        u64 base = reg_xsp(c, e->rn);
        if (!mem_write128(c, base, &c->v[e->rd])) NEXT;
        set_xsp(c, e->rn, base + e->imm);
        NEXT;
    }

    /* ---- FP/SIMD pairs (rm = Vt2; first element commits even if the
     * second faults — matches decode.c's vreg_load ordering) ---- */
L_LDPQ:
    {
        bool post = (e->op == PD_LDPQPOST);
        bool wb = post || e->op == PD_LDPQPRE;
        u64 base = reg_xsp(c, e->rn);
        u64 addr = post ? base : base + e->imm;
        V128 v;
        if (!mem_read128(c, addr, &v)) NEXT;
        c->v[e->rd] = v;
        if (!mem_read128(c, addr + 16, &v)) NEXT;
        c->v[e->rm] = v;
        if (wb) set_xsp(c, e->rn, post ? base + e->imm : addr);
        NEXT;
    }
L_STPQ:
    {
        bool post = (e->op == PD_STPQPOST);
        bool wb = post || e->op == PD_STPQPRE;
        u64 base = reg_xsp(c, e->rn);
        u64 addr = post ? base : base + e->imm;
        if (!mem_write128(c, addr, &c->v[e->rd])) NEXT;
        if (!mem_write128(c, addr + 16, &c->v[e->rm])) NEXT;
        if (wb) set_xsp(c, e->rn, post ? base + e->imm : addr);
        NEXT;
    }
L_LDPD:
    {
        bool post = (e->op == PD_LDPDPOST);
        bool wb = post || e->op == PD_LDPDPRE;
        u64 base = reg_xsp(c, e->rn);
        u64 addr = post ? base : base + e->imm;
        u64 t;
        if (!mem_read(c, addr, 8, &t)) NEXT;
        c->v[e->rd].d[0] = t; c->v[e->rd].d[1] = 0;
        if (!mem_read(c, addr + 8, 8, &t)) NEXT;
        c->v[e->rm].d[0] = t; c->v[e->rm].d[1] = 0;
        if (wb) set_xsp(c, e->rn, post ? base + e->imm : addr);
        NEXT;
    }
L_STPD:
    {
        bool post = (e->op == PD_STPDPOST);
        bool wb = post || e->op == PD_STPDPRE;
        u64 base = reg_xsp(c, e->rn);
        u64 addr = post ? base : base + e->imm;
        if (!mem_write(c, addr, 8, c->v[e->rd].d[0])) NEXT;
        if (!mem_write(c, addr + 8, 8, c->v[e->rm].d[0])) NEXT;
        if (wb) set_xsp(c, e->rn, post ? base + e->imm : addr);
        NEXT;
    }

#undef NEXT
}
