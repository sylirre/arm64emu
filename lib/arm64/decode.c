/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* A64 instruction decoder/executor — integer/branch/load-store (M1).
 * FP/SIMD lives in exec_fpsimd.c (M4). System regs in sysreg.c (M2).
 * One decoder for both of the CPU's modes (cpu.h, CpuEngine.sys/smp). The
 * EL1-only checks and traps are the architecture's own, decided by the EL and
 * the EL1 configuration, so in user mode, where the guest stays at EL0 under
 * the configuration sysreg_init gives it, they decide what Linux's would;
 * where guest threads run concurrently (smp) the LSE atomics, exclusives,
 * LDAR/STLR and barriers are host-atomic ones. Guest memory is reached
 * through mem.h, whose slow paths the program defines. */
#include "cpu.h"
#include <string.h>
#include "mem.h"
#include "esr.h"
#include "sysreg.h"
#include <stdio.h>

/* FP/SIMD group hook (M4), implemented in exec_fpsimd.c. */
void exec_fpsimd(CPU *c, u32 insn);

/* SCTLR_EL1.nTWI/nTWE: 0 traps an EL0 WFI/WFE that would wait. */
#define SCTLR_nTWI (1ULL << 16)
#define SCTLR_nTWE (1ULL << 18)

/* The trap of an EL0 WFI (ti 0) or WFE (ti 1) to EL1: EC 0x01, with
 * CV=1 and COND=0b1110 (unconditional) and TI in ISS[1:0]. */
__attribute__((cold)) static void wfx_trap(CPU *c, unsigned ti) {
    cpu_raise_sync(c, esr_make(EC_WFx, (1u << 24) | (0xEu << 20) | ti), 0);
}

__attribute__((cold)) static void undefined(CPU *c, u32 insn) {
    /* Architecturally an UNDEFINED instruction takes a synchronous exception
     * with EC_UNKNOWN. During bring-up we also log it. */
    if (g_trace)
        fprintf(stderr, "UNDEF insn 0x%08x at pc=0x%llx\n",
                insn, (unsigned long long)c->cur_insn_pc);
    cpu_raise_sync(c, esr_make(EC_UNKNOWN, 0), 0);
}

/* CPACR_EL1.FPEN guard, one call per FP/SIMD dispatch point (data-processing
 * and every vector load/store family). Raises the EC 0x07 access trap and
 * returns false when the current EL's FP access is trapped. It runs before
 * the fine-grained decode, so an unallocated FP-space encoding traps 0x07
 * rather than UNDEF while trapping is active — unobservable for real guests
 * (trapping is off out of reset, and in user mode for good: the FPEN Linux
 * gives a process, sysreg_init). */
static inline bool fp_access_ok(CPU *c) {
    if (__builtin_expect(!c->fp_trapped, 1)) return true;
    cpu_fp_trap(c);
    return false;
}

/* The unprivileged loads and stores (LDTR/STTR, SETxT, CPYxRT/WT) at EL1
 * access memory with EL0's permissions: c->ldst_unpriv, which a full
 * system's MMU reads, is set across each such access, and the D-TLB tag
 * context follows it (cpu_refresh_dtlb_ctx). At EL0, where a user-mode
 * guest always is, they are the plain forms. Every load and store brackets
 * its access so; nearly always the flag is already what it needs, and then
 * the bracket is a compare. */
#define unpriv_at_el1(c, t)   ((t) && (c)->el == 1)
static inline void set_ldst_unpriv(CPU *c, bool v) {
    if (UNLIKELY(c->ldst_unpriv != v)) {
        c->ldst_unpriv = v;
        cpu_refresh_dtlb_ctx(c);
    }
}

/* ---------- arithmetic helpers ---------- */

/* add_with_carry has two shapes, chosen by the compiler. The first computes
 * NZCV unconditionally, with __int128 for the 64-bit carry and overflow, and
 * throws it away when `flags` is NULL, the common non-S case (plain ADD/SUB);
 * the second hoists the math into an explicit `if (flags)` and does without
 * __int128. `flags` is a runtime value at every call site -- `S ? &fl : NULL`
 * -- so neither compiler sinks the math into the store, and they disagree on
 * which shape is faster: on a 300M-instruction interpreted ADD loop
 * (2026-08-04) the hoisted shape is SLOWER with gcc-13, 8.27s -> 9.07s, and
 * faster with clang, 6.91s -> 6.54s. Whole-boot timings could not resolve
 * either direction above the noise, and neither could, re-measured with gcc
 * when the choice stopped being per program (2026-11-01), a 300M-instruction
 * boot or the user-mode bench kernels, interpreted: repeated interleaved
 * runs came out either way. So gcc takes the first shape, and clang, and any
 * host without __int128 (the 32-bit ones), the second. Static and dynamic
 * instruction counts (the second shape runs ~3-4% fewer) predict the
 * opposite and should not be trusted here. Re-measure both compilers, in both
 * modes, before changing the choice. */
#if defined(__SIZEOF_INT128__) && !defined(__clang__)
static u64 add_with_carry(u64 x, u64 y, int cin, bool is64, u32 *flags) {
    u64 res;
    u32 N, Z, C, V;
    if (is64) {
        unsigned __int128 u = (unsigned __int128)x + (unsigned __int128)y + (unsigned)cin;
        res = (u64)u;
        C = (u32)((u >> 64) & 1);
        __int128 s = (__int128)(s64)x + (__int128)(s64)y + cin;
        V = ((__int128)(s64)res != s);
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
    if (flags) *flags = (N ? PS_N : 0) | (Z ? PS_Z : 0) | (C ? PS_C : 0) | (V ? PS_V : 0);
    return res;
}
#else
static u64 add_with_carry(u64 x, u64 y, int cin, bool is64, u32 *flags) {
    u64 res;
    if (is64) {
        u64 t = x + y;
        res = t + (unsigned)cin;
        if (flags) {                     /* #9: skip NZCV math when discarded */
            u32 C = (t < x) | (res < t);
            /* signed overflow: operands same sign, result differs */
            u32 V = (u32)((~(x ^ y) & (x ^ res)) >> 63);
            u32 N = (u32)(res >> 63);
            u32 Z = (res == 0);
            *flags = (N ? PS_N : 0) | (Z ? PS_Z : 0) | (C ? PS_C : 0) | (V ? PS_V : 0);
        }
    } else {
        u32 xx = (u32)x, yy = (u32)y;
        u64 u = (u64)xx + (u64)yy + (unsigned)cin;
        res = (u32)u;
        if (flags) {                     /* #9: skip NZCV math when discarded */
            u32 C = (u32)((u >> 32) & 1);
            s64 s = (s64)(s32)xx + (s64)(s32)yy + cin;
            u32 V = ((s64)(s32)(u32)res != s);
            u32 N = (u32)((u32)res >> 31);
            u32 Z = ((u32)res == 0);
            *flags = (N ? PS_N : 0) | (Z ? PS_Z : 0) | (C ? PS_C : 0) | (V ? PS_V : 0);
        }
    }
    return res;
}
#endif

static void set_logical_flags(CPU *c, u64 res, bool is64) {
    u32 f = 0;
    if (is64) { if (res >> 63) f |= PS_N; if (res == 0) f |= PS_Z; }
    else { u32 r = (u32)res; if (r >> 31) f |= PS_N; if (r == 0) f |= PS_Z; }
    c->nzcv = f;   /* C=V=0 */
}

static u64 shift_reg(u64 v, unsigned type, unsigned amount, bool is64) {
    unsigned w = is64 ? 64 : 32;
    amount &= (w - 1);
    if (!is64) v = (u32)v;
    switch (type) {
        case 0: return v << amount;                                  /* LSL */
        case 1: return is64 ? (v >> amount) : ((u32)v >> amount);    /* LSR */
        case 2: return is64 ? (u64)((s64)v >> amount)               /* ASR */
                            : (u64)(u32)((s32)(u32)v >> amount);
        default: return is64 ? ror64(v, amount) : ror32((u32)v, amount); /* ROR */
    }
}

/* ARMv8 CRC32/CRC32C: bit-reflected CRC over `bytes` low bytes of `data`,
 * accumulator `acc`. poly is the reflected polynomial (0xEDB88320 for CRC32,
 * 0x82F63B78 for CRC32C). Matches the hardware instruction the kernel uses.
 * Slicing-by-8 (lazily built 8x256 tables per polynomial): CRC32X consumes
 * its 8 bytes in one combined lookup instead of 64 bit-steps — ext4/btrfs
 * metadata checksumming leans on this, and every engine executes CRC32
 * through this helper (predecode leaves the family GENERIC), so it is an
 * all-tier win. */
static void crc32_tab_init(u32 t[8][256], u32 poly) {
    for (unsigned i = 0; i < 256; i++) {
        u32 c = i;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (poly & (u32)(-(s32)(c & 1)));
        t[0][i] = c;
    }
    for (unsigned i = 0; i < 256; i++)
        for (unsigned j = 1; j < 8; j++)
            t[j][i] = (t[j - 1][i] >> 8) ^ t[0][t[j - 1][i] & 0xff];
}

static u32 crc32_step(u32 acc, u64 data, unsigned bytes, u32 poly) {
    static u32 tab[2][8][256];
    unsigned p = (poly == 0x82F63B78u);
    if (tab[p][0][1] == 0) crc32_tab_init(tab[p], poly);
    const u32 (*t)[256] = tab[p];
    if (bytes == 8) {
        acc ^= (u32)data;
        u32 hi = (u32)(data >> 32);
        return t[7][acc & 0xff] ^ t[6][(acc >> 8) & 0xff] ^
               t[5][(acc >> 16) & 0xff] ^ t[4][acc >> 24] ^
               t[3][hi & 0xff] ^ t[2][(hi >> 8) & 0xff] ^
               t[1][(hi >> 16) & 0xff] ^ t[0][hi >> 24];
    }
    for (unsigned i = 0; i < bytes; i++)
        acc = (acc >> 8) ^ t[0][(acc ^ (u32)(data >> (8 * i))) & 0xff];
    return acc;
}

static u64 extend_reg(u64 v, unsigned option, unsigned shift) {
    u64 out;
    switch (option & 0x3) {
        case 0: out = (u8)v;  break;
        case 1: out = (u16)v; break;
        case 2: out = (u32)v; break;
        default: out = v;     break;
    }
    if (option & 0x4) {  /* signed */
        unsigned bits = (option & 3) == 0 ? 8 : (option & 3) == 1 ? 16 : (option & 3) == 2 ? 32 : 64;
        out = sign_extend(out, bits);
    }
    return out << shift;
}

static u64 ror_within(u64 v, unsigned r, unsigned width) {
    if (width == 64) return ror64(v, r);
    v = (u32)v; r %= width;
    return r ? ((v >> r) | (v << (width - r))) & 0xffffffffULL : v;
}

/* 64-bit-form computation (the replication fills all 64 bits; a 32-bit
 * caller truncates — valid because immN==0 bounds esize to <=32 there). */
static bool decode_bitmasks_slow(unsigned immN, unsigned imms, unsigned immr,
                                 u64 *wmask, u64 *tmask) {
    u32 nimms = ((immN & 1) << 6) | ((~imms) & 0x3f);
    if (nimms == 0) return false;
    int len = 31 - __builtin_clz(nimms);
    if (len < 1) return false;
    unsigned levels = (1u << len) - 1;
    unsigned S = imms & levels;
    unsigned R = immr & levels;
    unsigned diff = (S - R) & levels;
    unsigned esize = 1u << len;
    u64 welem = ones(S + 1);
    u64 telem = ones(diff + 1);
    u64 emask = (esize == 64) ? ~0ULL : ((1ULL << esize) - 1);
    welem &= emask; telem &= emask;
    unsigned r = R % esize;
    u64 w = r ? (((welem >> r) | (welem << (esize - r))) & emask) : welem;
    u64 wm = 0, tm = 0;
    for (unsigned i = 0; i < 64; i += esize) { wm |= w << i; tm |= telem << i; }
    *wmask = wm;
    *tmask = tm;
    return true;
}

/* Memoized front (#10): every logical-immediate and every UBFM/SBFM/BFM
 * (i.e. all LSL/LSR/ASR/UBFX/... aliases) funnels through here, and the
 * clz+rotate+replicate computation depends only on the 13-bit
 * immN:immr:imms key. The 32-bit view is the truncated 64-bit result;
 * immN==1 is the one 64-bit-only encoding (esize 64 > 32). Speeds the
 * portable interpreter and the exec_a64 helper fallback; pd_bitmasks in
 * predecode.c is left alone (already computed once at predecode-fill time). */
static bool decode_bitmasks(unsigned immN, unsigned imms, unsigned immr,
                            bool is64, u64 *wmask, u64 *tmask) {
    static struct { u64 w, t; u8 state; } memo[1u << 13];   /* 0 unfilled/1 ok/2 bad */
    unsigned key = ((immN & 1) << 12) | ((immr & 0x3f) << 6) | (imms & 0x3f);
    if (memo[key].state == 0) {
        u64 w = 0, t = 0;
        memo[key].state = decode_bitmasks_slow(immN, imms, immr, &w, &t) ? 1 : 2;
        memo[key].w = w; memo[key].t = t;
    }
    if (memo[key].state == 2) return false;
    if (!is64 && immN) return false;
    u64 wm = memo[key].w, tm = memo[key].t;
    if (!is64) { wm = (u32)wm; tm = (u32)tm; }
    if (wmask) *wmask = wm;
    if (tmask) *tmask = tm;
    return true;
}

/* ---------- field extraction ---------- */
#define BIT(i)      ((insn >> (i)) & 1u)
#define BITS(hi,lo) ((insn >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1))

/* ================= DP-immediate ================= */
static void dp_immediate(CPU *c, u32 insn) {
    unsigned t = BITS(28, 23);
    bool sf = BIT(31);
    unsigned Rd = BITS(4, 0), Rn = BITS(9, 5);

    if (t == 0x20 || t == 0x21) {                 /* PC-rel: ADR/ADRP */
        u32 immlo = BITS(30, 29), immhi = BITS(23, 5);
        s64 imm = (s64)sign_extend(((u64)immhi << 2) | immlo, 21);
        if (BIT(31) == 0) set_x(c, Rd, c->cur_insn_pc + imm);
        else set_x(c, Rd, (c->cur_insn_pc & ~0xfffULL) + ((u64)imm << 12));
        return;
    }
    if (t == 0x22) {                              /* add/sub immediate */
        bool op = BIT(30), S = BIT(29), sh = BIT(22);
        u64 imm = BITS(21, 10);
        if (sh) imm <<= 12;
        u64 n = reg_xsp(c, Rn);
        u32 fl;
        u32 *flp = S ? &fl : NULL;         /* NULL: NZCV not wanted */
        u64 r = op ? add_with_carry(n, ~imm, 1, sf, flp)
                   : add_with_carry(n, imm, 0, sf, flp);
        if (S) { c->nzcv = fl; set_x_sz(c, Rd, sf, r); }
        else set_xsp(c, Rd, sf ? r : (u32)r);
        return;
    }
    if (t == 0x24) {                              /* logical immediate */
        unsigned opc = BITS(30, 29), N = BIT(22), immr = BITS(21, 16), imms = BITS(15, 10);
        u64 wmask;
        if (!decode_bitmasks(N, imms, immr, sf, &wmask, NULL)) { undefined(c, insn); return; }
        u64 n = reg_x(c, Rn), r;
        switch (opc) {
            case 0: r = n & wmask; break;
            case 1: r = n | wmask; break;
            case 2: r = n ^ wmask; break;
            default: r = n & wmask; break;
        }
        if (!sf) r = (u32)r;
        if (opc == 3) { set_logical_flags(c, r, sf); set_x(c, Rd, r); }
        else set_xsp(c, Rd, r);
        return;
    }
    if (t == 0x25) {                              /* move wide immediate */
        unsigned opc = BITS(30, 29), hw = BITS(22, 21), imm16 = BITS(20, 5);
        if (!sf && hw >= 2) { undefined(c, insn); return; }  /* 32-bit: hw in {0,1} only */
        unsigned shift = hw * 16;
        u64 r;
        if (opc == 0) r = ~((u64)imm16 << shift);             /* MOVN */
        else if (opc == 2) r = (u64)imm16 << shift;           /* MOVZ */
        else if (opc == 3) {                                  /* MOVK */
            u64 cur = reg_x(c, Rd);
            r = (cur & ~((u64)0xffff << shift)) | ((u64)imm16 << shift);
        } else { undefined(c, insn); return; }
        set_x_sz(c, Rd, sf, r);
        return;
    }
    if (t == 0x26) {                              /* bitfield SBFM/BFM/UBFM */
        unsigned opc = BITS(30, 29), N = BIT(22), immr = BITS(21, 16), imms = BITS(15, 10);
        /* opc==11 has no bitfield instruction, and N must equal sf. (The
         * sf==0/N==1 half is already rejected inside decode_bitmasks; this
         * catches sf==1/N==0, which was running as a 32-bit-pattern bitfield
         * on a 64-bit register.) */
        if (opc == 3 || N != (unsigned)(sf != 0)) { undefined(c, insn); return; }
        u64 wmask, tmask;
        if (!decode_bitmasks(N, imms, immr, sf, &wmask, &tmask)) { undefined(c, insn); return; }
        u64 src = reg_x(c, Rn);
        u64 ror = ror_within(src, immr, sf ? 64 : 32);
        u64 bot = (opc == 1) ? ((reg_x(c, Rd) & ~wmask) | (ror & wmask)) : (ror & wmask);
        u64 top;
        if (opc == 0) { u64 sb = (src >> imms) & 1; top = sb ? ~0ULL : 0ULL; }  /* SBFM */
        else if (opc == 2) top = 0;                                            /* UBFM */
        else top = reg_x(c, Rd);                                              /* BFM */
        u64 r = (top & ~tmask) | (bot & tmask);
        set_x_sz(c, Rd, sf, r);
        return;
    }
    if (t == 0x27) {                              /* EXTR */
        unsigned Rm = BITS(20, 16), imms = BITS(15, 10);
        /* N (bit22) must equal sf, bit21 must be 0, and the 32-bit form only
         * allows lsb<32 — otherwise unallocated. Guarding also avoids the host
         * UB of shifting a u32 by 32-lsb when lsb>=32. */
        if (BITS(30, 29) != 0 || BIT(22) != (unsigned)(sf != 0) || BIT(21) != 0 ||
            (!sf && (imms & 0x20))) {
            undefined(c, insn); return;
        }
        /* EXTR Rd, Rn, Rm, #lsb : result = (Rn:Rm) >> lsb */
        u64 hi = reg_x(c, Rn), lo = reg_x(c, Rm);
        unsigned lsb = imms;
        u64 r;
        if (sf) r = lsb ? ((hi << (64 - lsb)) | (lo >> lsb)) : lo;
        else { u32 h = (u32)hi, l = (u32)lo; r = lsb ? (u32)((h << (32 - lsb)) | (l >> lsb)) : l; }
        set_x_sz(c, Rd, sf, r);
        return;
    }
    undefined(c, insn);
}

/* ================= DP-register ================= */
static void dp_register(CPU *c, u32 insn) {
    bool sf = BIT(31);
    unsigned Rd = BITS(4, 0), Rn = BITS(9, 5), Rm = BITS(20, 16);
    unsigned op24 = BITS(28, 24);

    if (op24 == 0x0a) {                            /* logical shifted register */
        unsigned opc = BITS(30, 29), shift = BITS(23, 22), N = BIT(21), imm6 = BITS(15, 10);
        if (!sf && (imm6 & 0x20)) { undefined(c, insn); return; }  /* imm6>=32 in 32-bit: unallocated */
        u64 op2 = shift_reg(reg_x(c, Rm), shift, imm6, sf);
        if (N) op2 = ~op2;
        u64 n = reg_x(c, Rn), r;
        switch (opc) {
            case 0: r = n & op2; break;     /* AND/BIC */
            case 1: r = n | op2; break;     /* ORR/ORN */
            case 2: r = n ^ op2; break;     /* EOR/EON */
            default: r = n & op2; break;    /* ANDS/BICS */
        }
        if (!sf) r = (u32)r;
        if (opc == 3) set_logical_flags(c, r, sf);
        set_x(c, Rd, r);
        return;
    }
    if (op24 == 0x0b) {
        bool ext = BIT(21);
        bool op = BIT(30), S = BIT(29);
        u64 op2, n; u32 fl;
        if (ext) {                                 /* add/sub extended register */
            unsigned option = BITS(15, 13), imm3 = BITS(12, 10);
            /* shift amount 5-7 is unallocated, and bits[23:22] (opt) are RES0
             * for the extended-register form — a nonzero opt was decoded as a
             * plain extended add/sub. */
            if (imm3 > 4 || BITS(23, 22) != 0) { undefined(c, insn); return; }
            op2 = extend_reg(reg_x(c, Rm), option, imm3);
            n = reg_xsp(c, Rn);
        } else {                                   /* add/sub shifted register */
            unsigned shift = BITS(23, 22), imm6 = BITS(15, 10);
            /* ROR (shift==3) is unallocated for add/sub; imm6>=32 in 32-bit too. */
            if (shift == 3 || (!sf && (imm6 & 0x20))) { undefined(c, insn); return; }
            op2 = shift_reg(reg_x(c, Rm), shift, imm6, sf);
            n = reg_x(c, Rn);
        }
        u32 *flp = S ? &fl : NULL;         /* NULL: NZCV not wanted */
        u64 r = op ? add_with_carry(n, ~op2, 1, sf, flp)
                   : add_with_carry(n, op2, 0, sf, flp);
        if (S) { c->nzcv = fl; set_x_sz(c, Rd, sf, r); }
        else if (ext) set_xsp(c, Rd, sf ? r : (u32)r);
        else set_x_sz(c, Rd, sf, r);
        return;
    }
    if (op24 == 0x1b) {                            /* data processing (3 source) */
        unsigned op31 = BITS(23, 21), o0 = BIT(15), Ra = BITS(14, 10);
        if (BITS(30, 29) != 0) { undefined(c, insn); return; }  /* op54 is RES0 */
        /* The widening/high multiplies (op31!=0: S/UMADDL, S/UMSUBL, S/UMULH)
         * require sf=1; sf=0 is unallocated. MADD/MSUB (op31==0) are valid both. */
        if (!sf && op31 != 0) { undefined(c, insn); return; }
        u64 n = reg_x(c, Rn), m = reg_x(c, Rm), a = reg_x(c, Ra), r;
        switch ((op31 << 1) | o0) {
            case 0x0: r = a + n * m; break;                                   /* MADD */
            case 0x1: r = a - n * m; break;                                   /* MSUB */
            case 0x2: r = a + (u64)((s64)(s32)(u32)n * (s64)(s32)(u32)m); break; /* SMADDL */
            case 0x3: r = a - (u64)((s64)(s32)(u32)n * (s64)(s32)(u32)m); break; /* SMSUBL */
            case 0x4: r = (u64)smulh64((s64)n, (s64)m); break;                /* SMULH */
            case 0xa: r = a + (u64)((u64)(u32)n * (u64)(u32)m); break;        /* UMADDL */
            case 0xb: r = a - (u64)((u64)(u32)n * (u64)(u32)m); break;        /* UMSUBL */
            case 0xc: r = umulh64(n, m); break;                               /* UMULH */
            default: undefined(c, insn); return;
        }
        set_x_sz(c, Rd, sf, r);
        return;
    }
    if (op24 == 0x1a) {
        unsigned op21 = BITS(28, 21);
        if (op21 == 0xd0) {
            if (BITS(15, 10) == 0) {               /* add/sub with carry */
                bool op = BIT(30), S = BIT(29);
                u32 fl;
                u32 *flp = S ? &fl : NULL; /* NULL: NZCV not wanted */
                int cin = (c->nzcv & PS_C) ? 1 : 0;
                u64 m = reg_x(c, Rm), n = reg_x(c, Rn);
                u64 r = op ? add_with_carry(n, ~m, cin, sf, flp)
                           : add_with_carry(n, m, cin, sf, flp);
                if (S) c->nzcv = fl;
                set_x_sz(c, Rd, sf, r);
                return;
            }
            /* RMIF (FEAT_FLAGM): rotate Xn right by imm6, move tmp<3:0> into
             * the NZCV bits selected by mask (bit3=N .. bit0=V). */
            if (BIT(31) && !BIT(30) && BIT(29) && BITS(14, 10) == 1 && BIT(4) == 0) {
                unsigned imm6 = BITS(20, 15), mask = BITS(3, 0);
                u64 t = reg_x(c, Rn);
                if (imm6) t = (t >> imm6) | (t << (64 - imm6));
                unsigned nib = (((c->nzcv >> 28) & 0xf) & ~mask) | ((unsigned)t & mask);
                c->nzcv = nib << 28;
                return;
            }
            /* SETF8/SETF16 (FEAT_FLAGM): narrowing-overflow flags from the low
             * 8/16 bits of Wn (register in the Rn field): N=sign, Z=zero,
             * V=bit msb+1 EOR msb; C is unchanged. */
            if (!BIT(31) && !BIT(30) && BIT(29) && BITS(20, 16) == 0 &&
                (BITS(15, 10) == 0x02 || BITS(15, 10) == 0x12) && BITS(4, 0) == 0x0d) {
                unsigned msb = BIT(14) ? 15 : 7;
                u64 w = reg_x(c, Rn);
                u32 f = c->nzcv & PS_C;
                if ((w >> msb) & 1) f |= PS_N;
                if ((w & ((2ULL << msb) - 1)) == 0) f |= PS_Z;
                if (((w >> (msb + 1)) ^ (w >> msb)) & 1) f |= PS_V;
                c->nzcv = f;
                return;
            }
            undefined(c, insn);
            return;
        }
        if (op21 == 0xd2) {                        /* conditional compare */
            /* S (bit29) must be 1 — CCMP/CCMN always write NZCV — and bit10
             * (o2) and bit4 (o3) are RES0. */
            if (!BIT(29) || BIT(10) || BIT(4)) { undefined(c, insn); return; }
            bool op = BIT(30);
            unsigned cond = BITS(15, 12), nzcv = BITS(3, 0);
            bool is_imm = BIT(11);
            u64 n = reg_x(c, Rn);
            u64 m = is_imm ? BITS(20, 16) : reg_x(c, Rm);
            if (cond_holds(c, cond)) {
                u32 fl;
                if (op) add_with_carry(n, ~m, 1, sf, &fl);   /* CCMP */
                else add_with_carry(n, m, 0, sf, &fl);       /* CCMN */
                c->nzcv = fl;
            } else {
                c->nzcv = ((nzcv & 8) ? PS_N : 0) | ((nzcv & 4) ? PS_Z : 0) |
                          ((nzcv & 2) ? PS_C : 0) | ((nzcv & 1) ? PS_V : 0);
            }
            return;
        }
        if (op21 == 0xd4) {                        /* conditional select */
            /* S (bit29) must be 0 — the family never writes NZCV — and bit11
             * is RES0 (only bit10 selects the increment/invert variant). */
            if (BIT(29) || BIT(11)) { undefined(c, insn); return; }
            bool op = BIT(30), o2 = BIT(10);
            unsigned cond = BITS(15, 12);
            u64 n = reg_x(c, Rn), m = reg_x(c, Rm), r;
            if (cond_holds(c, cond)) r = n;
            else {
                if (!op && !o2) r = m;             /* CSEL */
                else if (!op && o2) r = m + 1;     /* CSINC */
                else if (op && !o2) r = ~m;        /* CSINV */
                else r = 0 - m;                    /* CSNEG (unsigned negation:
                                                    * INT64_MIN is arithmetic,
                                                    * not signed overflow) */
            }
            set_x_sz(c, Rd, sf, r);
            return;
        }
        if (op21 == 0xd6) {
            if (BIT(30)) {                         /* data processing (1 source) */
                unsigned opcode = BITS(15, 10);
                u64 n = reg_x(c, Rn), r;
                switch (opcode) {
                    case 0x00: {  /* RBIT */
                        u64 v = sf ? n : (u32)n, o = 0;
                        unsigned w = sf ? 64 : 32;
                        for (unsigned i = 0; i < w; i++) if ((v >> i) & 1) o |= 1ULL << (w - 1 - i);
                        r = o; break;
                    }
                    case 0x01: {  /* REV16 */
                        u64 v = n, o = 0; unsigned w = sf ? 8 : 4;
                        for (unsigned i = 0; i < w; i += 2) {
                            o |= ((v >> (i * 8)) & 0xff) << ((i + 1) * 8);
                            o |= ((v >> ((i + 1) * 8)) & 0xff) << (i * 8);
                        }
                        r = o; break;
                    }
                    case 0x02: {  /* REV32 (64-bit) or REV (32-bit) */
                        if (sf) { u64 v = n, o = 0;
                            for (unsigned g = 0; g < 2; g++) {
                                u32 word = (v >> (g * 32)) & 0xffffffff;
                                u32 sw = __builtin_bswap32(word);
                                o |= (u64)sw << (g * 32);
                            }
                            r = o;
                        } else r = __builtin_bswap32((u32)n);
                        break;
                    }
                    case 0x03: r = __builtin_bswap64(n); break;  /* REV (64-bit) */
                    case 0x04: {  /* CLZ */
                        if (sf) r = n ? __builtin_clzll(n) : 64;
                        else r = (u32)n ? __builtin_clz((u32)n) : 32;
                        break;
                    }
                    case 0x05: {  /* CLS */
                        unsigned w = sf ? 64 : 32;
                        u64 v = sf ? n : (u32)n;
                        u64 sign = (v >> (w - 1)) & 1;
                        unsigned cnt = 0;
                        for (int i = w - 2; i >= 0; i--) { if (((v >> i) & 1) == sign) cnt++; else break; }
                        r = cnt; break;
                    }
                    default: undefined(c, insn); return;
                }
                set_x_sz(c, Rd, sf, r);
                return;
            } else {                               /* data processing (2 source) */
                unsigned opcode = BITS(15, 10);
                u64 n = reg_x(c, Rn), m = reg_x(c, Rm), r;
                switch (opcode) {
                    case 0x02: /* UDIV */
                        if (sf) r = (m == 0) ? 0 : n / m;
                        else { u32 a = n, b = m; r = b ? a / b : 0; }
                        break;
                    case 0x03: /* SDIV */
                        if (sf) { s64 a = n, b = m; r = (b == 0) ? 0 : (u64)((b == -1 && a == INT64_MIN) ? a : a / b); }
                        else { s32 a = (s32)(u32)n, b = (s32)(u32)m; r = b ? (u32)((b == -1 && a == INT32_MIN) ? a : a / b) : 0; }
                        break;
                    case 0x08: r = shift_reg(n, 0, m, sf); break;  /* LSLV */
                    case 0x09: r = shift_reg(n, 1, m, sf); break;  /* LSRV */
                    case 0x0a: r = shift_reg(n, 2, m, sf); break;  /* ASRV */
                    case 0x0b: r = shift_reg(n, 3, m, sf); break;  /* RORV */
                    case 0x10: r = crc32_step((u32)n, m, 1, 0xEDB88320); break; /* CRC32B  */
                    case 0x11: r = crc32_step((u32)n, m, 2, 0xEDB88320); break; /* CRC32H  */
                    case 0x12: r = crc32_step((u32)n, m, 4, 0xEDB88320); break; /* CRC32W  */
                    case 0x13: r = crc32_step((u32)n, m, 8, 0xEDB88320); break; /* CRC32X  */
                    case 0x14: r = crc32_step((u32)n, m, 1, 0x82F63B78); break; /* CRC32CB */
                    case 0x15: r = crc32_step((u32)n, m, 2, 0x82F63B78); break; /* CRC32CH */
                    case 0x16: r = crc32_step((u32)n, m, 4, 0x82F63B78); break; /* CRC32CW */
                    case 0x17: r = crc32_step((u32)n, m, 8, 0x82F63B78); break; /* CRC32CX */
                    default: undefined(c, insn); return;
                }
                set_x_sz(c, Rd, sf, r);
                return;
            }
        }
    }
    undefined(c, insn);
}

/* ================= loads/stores ================= */

static u64 ldst_extended_offset(CPU *c, u32 insn, unsigned size) {
    unsigned Rm = BITS(20, 16), option = BITS(15, 13);
    bool S = BIT(12);
    unsigned shift = S ? size : 0;
    /* option==011 => LSL/UXTX (64-bit, no real extend) */
    return extend_reg(reg_x(c, Rm), option, shift);
}

/* Returns false if the access faulted (an abort was raised); callers MUST then
 * abort the instruction WITHOUT applying base-register writeback, because the
 * faulting instruction is re-executed after the abort handler returns and a
 * writeback applied here would be applied a second time (corrupting the base). */
static bool do_load(CPU *c, unsigned Rt, u64 va, unsigned size, unsigned opc) {
    unsigned bytes = 1u << size;
    if (opc == 2 && size == 3) return true;   /* PRFM: no register write */
    u64 raw;
    if (!mem_read(c, va, bytes, &raw)) return false;
    bool sign = (opc == 2) || (opc == 3);
    bool ext64 = (opc == 2) ? true : (opc == 3) ? false : (size == 3);
    u64 val = sign ? sign_extend(raw, bytes * 8) : raw;
    if (!ext64) val = (u32)val;
    set_x(c, Rt, val);
    return true;
}

/* SIMD/FP register memory access of `bytes` (1,2,4,8,16); zero-extends loads.
 * Returns false on fault (see do_load on why writeback must then be skipped). */
static bool vreg_load(CPU *c, unsigned Vt, u64 va, unsigned bytes) {
    V128 val; val.d[0] = 0; val.d[1] = 0;
    if (bytes == 16) { if (!mem_read128(c, va, &val)) return false; }
    else { u64 t = 0; if (!mem_read(c, va, bytes, &t)) return false; val.d[0] = t; }
    c->v[Vt] = val;
    return true;
}
static bool vreg_store(CPU *c, unsigned Vt, u64 va, unsigned bytes) {
    if (bytes == 16) return mem_write128(c, va, &c->v[Vt]);
    return mem_write(c, va, bytes, c->v[Vt].d[0]);
}

/* The atomics' and exclusives' implementations stay out of line: inlined
 * into exec_a64 they took the inliner's budget from the plain loads and
 * stores (ldst_register, ldst_pair), which are far more frequent, and next to
 * an atomic access a call costs little. */
#define NOINL __attribute__((noinline))

/* ============ ARMv8.1-A LSE atomics ============
 * Atomic memory operations (LDADD/LDCLR/LDEOR/LDSET/LDSMAX/LDSMIN/LDUMAX/
 * LDUMIN and SWP) plus Compare-and-Swap (CAS/CASP) and LDAPR. Modern glibc
 * selects these via HWCAP_ATOMICS; -march=armv8.1-a+ code uses them
 * unconditionally. Two implementations, by the CPU's mode: plain
 * read-modify-writes on one CPU, host atomics where guest threads run
 * concurrently (CpuEngine.smp). */

/* ---- one CPU ----
 * A plain read-modify-write through mem_read/mem_write is architecturally
 * atomic on a single in-order CPU, and the A/R acquire/release bits need no
 * fences. A fault from either access aborts the whole instruction with no
 * register update (mem_read/mem_write raise the data abort). Alignment is not
 * checked, matching the exclusives below; the compare-fail path of CAS skips
 * the architectural write-permission check (documented laxity, same family
 * as TODO_OPCODES §5). These are also the concurrent forms' way through
 * memory with no host pointer to operate on (mem_host_ptr): the access then
 * faults where the architecture does -- CAS `strict`, writing the old value
 * back when the comparison fails, since the architecture translates a CAS as
 * a write whatever it compares. */

static NOINL void ldst_atomic_up(CPU *c, u32 insn) {
    unsigned size = BITS(31, 30);
    unsigned Rs = BITS(20, 16), o3 = BIT(15), opc = BITS(14, 12);
    unsigned Rn = BITS(9, 5), Rt = BITS(4, 0);
    unsigned bytes = 1u << size, esize = 8u * bytes;
    u64 va = reg_xsp(c, Rn);

    /* o3==1 is SWP (opc==0) or LDAPR (opc==4); other opc are LD<op>. */
    if (o3 && opc != 0) {
        if (opc == 4) {                          /* LDAPR: load-acquire RCpc */
            u64 v;
            if (mem_read(c, va, bytes, &v)) set_x(c, Rt, v);
            return;
        }
        undefined(c, insn);
        return;
    }

    u64 emask = (bytes == 8) ? ~0ULL : ((1ULL << esize) - 1);
    u64 operand = reg_x(c, Rs) & emask, old, res;
    if (!mem_read(c, va, bytes, &old)) return;
    if (o3) {
        res = operand;                                            /* SWP */
    } else {
        s64 so = (s64)sign_extend(old, esize), sn = (s64)sign_extend(operand, esize);
        switch (opc) {
            case 0: res = old + operand; break;                   /* LDADD  */
            case 1: res = old & ~operand; break;                  /* LDCLR  */
            case 2: res = old ^ operand; break;                   /* LDEOR  */
            case 3: res = old | operand; break;                   /* LDSET  */
            case 4: res = (so > sn) ? old : operand; break;       /* LDSMAX */
            case 5: res = (so < sn) ? old : operand; break;       /* LDSMIN */
            case 6: res = (old > operand) ? old : operand; break; /* LDUMAX */
            default: res = (old < operand) ? old : operand; break;/* LDUMIN */
        }
    }
    if (!mem_write(c, va, bytes, res & emask)) return;
    set_x(c, Rt, old);   /* ST<op> forms use Rt==31 and discard */
}

/* CAS/CASA/CASL/CASAL: compare Rs, swap Rt, return old in Rs. */
static NOINL void ldst_cas_up(CPU *c, u32 insn, bool strict) {
    unsigned size = BITS(31, 30);
    unsigned Rs = BITS(20, 16), Rn = BITS(9, 5), Rt = BITS(4, 0);
    unsigned bytes = 1u << size;
    u64 emask = (bytes == 8) ? ~0ULL : ((1ULL << (8u * bytes)) - 1);
    u64 va = reg_xsp(c, Rn), old;
    if (!mem_read(c, va, bytes, &old)) return;
    if (old == (reg_x(c, Rs) & emask)) {
        if (!mem_write(c, va, bytes, reg_x(c, Rt) & emask)) return;
    } else if (strict) {
        if (!mem_write(c, va, bytes, old)) return;
    }
    set_x(c, Rs, old);
}

/* CASP/CASPA/CASPL/CASPAL: 2-register compare-and-swap (Rs:Rs+1 vs Rt:Rt+1).
 * sz bit30: 0=two 32-bit words (8-byte total), 1=two 64-bit (16-byte). The
 * pair is naturally aligned, so it never crosses a page: once the first
 * write succeeds the second cannot fault. */
static NOINL void ldst_casp_up(CPU *c, u32 insn, bool strict) {
    bool sz = BIT(30);
    unsigned Rs = BITS(20, 16), Rn = BITS(9, 5), Rt = BITS(4, 0);
    unsigned bytes = sz ? 8 : 4;
    u64 emask = sz ? ~0ULL : 0xffffffffULL;
    u64 va = reg_xsp(c, Rn), lo, hi;
    if (!mem_read(c, va, bytes, &lo)) return;
    if (!mem_read(c, va + bytes, bytes, &hi)) return;
    if (lo == (reg_x(c, Rs) & emask) && hi == (reg_x(c, Rs + 1) & emask)) {
        if (!mem_write(c, va, bytes, reg_x(c, Rt) & emask)) return;
        if (!mem_write(c, va + bytes, bytes, reg_x(c, Rt + 1) & emask)) return;
    } else if (strict) {
        if (!mem_write(c, va, bytes, lo)) return;
        if (!mem_write(c, va + bytes, bytes, hi)) return;
    }
    set_x(c, Rs, lo);
    set_x(c, Rs + 1, hi);
}

/* ---- guest threads running concurrently ----
 * Host __atomic builtins on the translated host pointer, so they are correct
 * under real guest threads (M6) as well as single-threaded. A (bit23/bit22
 * for exclusives) = acquire, R = release. */

/* Map the acquire/release bits to a C memory order. */
static int lse_order(bool acq, bool rel) {
    if (acq && rel) return __ATOMIC_ACQ_REL;
    if (acq) return __ATOMIC_ACQUIRE;
    if (rel) return __ATOMIC_RELEASE;
    return __ATOMIC_RELAXED;
}

/* Raise an alignment fault (unaligned atomic) as a data abort -> SIGBUS. */
static void atomic_align_fault(CPU *c, u64 va, bool write) {
    cpu_raise_sync(c, esr_make(c->el ? EC_DABORT_SAME : EC_DABORT_LOWER,
                               iss_dabort(write, FSC_ALIGN)), va);
}

/* Perform one LD<op>/SWP on a `bytes`-wide host location. Returns the old value
 * (zero-extended). opc/o3 select the operation. */
#define LSE_RMW(TY, STY)                                                        \
    do {                                                                        \
        TY *pp = (TY *)hp;                                                       \
        TY exp = __atomic_load_n(pp, __ATOMIC_RELAXED), des;                     \
        TY opnd = (TY)operand;                                                   \
        if (o3 && opc == 0) { old = __atomic_exchange_n(pp, opnd, mo); break; }  \
        do {                                                                     \
            TY o = exp;                                                          \
            switch (opc) {                                                       \
                case 0: des = o + opnd; break;                 /* LDADD */       \
                case 1: des = o & (TY)~opnd; break;            /* LDCLR */       \
                case 2: des = o ^ opnd; break;                 /* LDEOR */       \
                case 3: des = o | opnd; break;                 /* LDSET */       \
                case 4: des = ((STY)o > (STY)opnd) ? o : opnd; break; /* LDSMAX */ \
                case 5: des = ((STY)o < (STY)opnd) ? o : opnd; break; /* LDSMIN */ \
                case 6: des = (o > opnd) ? o : opnd; break;    /* LDUMAX */      \
                default: des = (o < opnd) ? o : opnd; break;   /* LDUMIN */      \
            }                                                                    \
        } while (!__atomic_compare_exchange_n(pp, &exp, des, true, mo,           \
                                              __ATOMIC_RELAXED));                \
        old = exp;                                                               \
    } while (0)

static NOINL void ldst_atomic_smp(CPU *c, u32 insn) {
    unsigned size = BITS(31, 30);
    bool A = BIT(23), R = BIT(22);
    unsigned Rs = BITS(20, 16), o3 = BIT(15), opc = BITS(14, 12);
    unsigned Rn = BITS(9, 5), Rt = BITS(4, 0);
    unsigned bytes = 1u << size;

    /* o3==1 is SWP (opc==0) or LDAPR (opc==4); other opc are LD<op>. */
    if (o3 && opc != 0) {
        if (opc == 4) {                          /* LDAPR: load-acquire RCpc */
            u64 v;
            if (!mem_read(c, reg_xsp(c, Rn), bytes, &v)) return;
            set_x(c, Rt, v);
            return;
        }
        undefined(c, insn);
        return;
    }

    u64 va = reg_xsp(c, Rn);
    u64 operand = reg_x(c, Rs);
    if (va & (bytes - 1)) { atomic_align_fault(c, va, true); return; }
    void *hp = mem_host_ptr(c, va, bytes, ACC_WRITE);
    if (!hp) { ldst_atomic_up(c, insn); return; }   /* faults as it should */

    int mo = lse_order(A, R);
    u64 old = 0;
    switch (bytes) {
        case 1: LSE_RMW(u8, s8); break;
        case 2: LSE_RMW(u16, s16); break;
        case 4: LSE_RMW(u32, s32); break;
        default: LSE_RMW(u64, s64); break;
    }
    set_x(c, Rt, old);   /* ST<op> forms use Rt==31 and discard */
}

/* CAS/CASA/CASL/CASAL: compare Rs, swap Rt, return old in Rs. */
static NOINL void ldst_cas_smp(CPU *c, u32 insn) {
    unsigned size = BITS(31, 30);
    bool A = BIT(22), R = BIT(15);
    unsigned Rs = BITS(20, 16), Rn = BITS(9, 5), Rt = BITS(4, 0);
    unsigned bytes = 1u << size;
    u64 va = reg_xsp(c, Rn);
    if (va & (bytes - 1)) { atomic_align_fault(c, va, true); return; }
    void *hp = mem_host_ptr(c, va, bytes, ACC_WRITE);
    if (!hp) { ldst_cas_up(c, insn, true); return; }
    int mo = lse_order(A, R);
    /* Strong CAS: a spurious failure would leave the compare register equal to
     * the expected value, which the guest reads as success while memory was not
     * updated — a lost store that breaks every CAS-based lock. */
    u64 cmp = reg_x(c, Rs), newv = reg_x(c, Rt), old;
    switch (bytes) {
        case 1: { u8  e = (u8)cmp;  __atomic_compare_exchange_n((u8 *)hp,  &e, (u8)newv,  false, mo, __ATOMIC_RELAXED); old = e; break; }
        case 2: { u16 e = (u16)cmp; __atomic_compare_exchange_n((u16 *)hp, &e, (u16)newv, false, mo, __ATOMIC_RELAXED); old = e; break; }
        case 4: { u32 e = (u32)cmp; __atomic_compare_exchange_n((u32 *)hp, &e, (u32)newv, false, mo, __ATOMIC_RELAXED); old = e; break; }
        default:{ u64 e = cmp;      __atomic_compare_exchange_n((u64 *)hp, &e, newv,      false, mo, __ATOMIC_RELAXED); old = e; break; }
    }
    set_x(c, Rs, old);
}

/* CASP/CASPA/CASPL/CASPAL: 2-register compare-and-swap (Rs:Rs+1 vs Rt:Rt+1).
 * sz bit30: 0=two 32-bit words (8-byte total), 1=two 64-bit (16-byte). */
static NOINL void ldst_casp_smp(CPU *c, u32 insn) {
    bool sz = BIT(30);
    bool A = BIT(22), R = BIT(15);
    unsigned Rs = BITS(20, 16), Rn = BITS(9, 5), Rt = BITS(4, 0);
    unsigned bytes = sz ? 16 : 8;
    u64 va = reg_xsp(c, Rn);
    if (va & (bytes - 1)) { atomic_align_fault(c, va, true); return; }
    void *hp = mem_host_ptr(c, va, bytes, ACC_WRITE);
    if (!hp) { ldst_casp_up(c, insn, true); return; }
    int mo = lse_order(A, R);
    if (!sz) {                                   /* pair of 32-bit -> one u64 */
        u64 cmp = (reg_x(c, Rs) & 0xffffffff) | (reg_x(c, Rs + 1) << 32);
        u64 newv = (reg_x(c, Rt) & 0xffffffff) | (reg_x(c, Rt + 1) << 32);
        u64 e = cmp;
        __atomic_compare_exchange_n((u64 *)hp, &e, newv, false, mo, __ATOMIC_RELAXED);
        set_x(c, Rs, (u32)e);
        set_x(c, Rs + 1, (u32)(e >> 32));
    } else {                                      /* pair of 64-bit -> u128 */
#ifdef __SIZEOF_INT128__
        unsigned __int128 cmp = (unsigned __int128)reg_x(c, Rs) |
                                ((unsigned __int128)reg_x(c, Rs + 1) << 64);
        unsigned __int128 newv = (unsigned __int128)reg_x(c, Rt) |
                                 ((unsigned __int128)reg_x(c, Rt + 1) << 64);
        unsigned __int128 e = cmp;
        __atomic_compare_exchange_n((unsigned __int128 *)hp, &e, newv, false, mo,
                                    __ATOMIC_RELAXED);
        set_x(c, Rs, (u64)e);
        set_x(c, Rs + 1, (u64)(e >> 64));
#else
        /* 32-bit hosts lack lock-free 128-bit CAS: serialize on a global mutex.
         * Correct (single interpreter thread per lock) at the cost of speed. */
        extern void casp16_mutex_lock(void);
        extern void casp16_mutex_unlock(void);
        casp16_mutex_lock();
        u64 lo, hi;
        bool ok1 = mem_read(c, va, 8, &lo), ok2 = mem_read(c, va + 8, 8, &hi);
        if (ok1 && ok2) {
            if (lo == reg_x(c, Rs) && hi == reg_x(c, Rs + 1)) {
                mem_write(c, va, 8, reg_x(c, Rt));
                mem_write(c, va + 8, 8, reg_x(c, Rt + 1));
            }
            set_x(c, Rs, lo);
            set_x(c, Rs + 1, hi);
        }
        casp16_mutex_unlock();
#endif
    }
}

static void ldst_atomic(CPU *c, u32 insn) {
    if (c->eng.smp) ldst_atomic_smp(c, insn); else ldst_atomic_up(c, insn);
}
static void ldst_cas(CPU *c, u32 insn) {
    if (c->eng.smp) ldst_cas_smp(c, insn); else ldst_cas_up(c, insn, false);
}
static void ldst_casp(CPU *c, u32 insn) {
    if (c->eng.smp) ldst_casp_smp(c, insn); else ldst_casp_up(c, insn, false);
}
/* ============ end LSE atomics ============ */

/* CheckSPAlignment (SCTLR_EL1.SA/SA0): when SP is the base register of a memory
 * access and the check is enabled for the current EL (c->sp_chk), SP must be
 * 16-byte aligned, else the access takes an SP-alignment fault (EC 0x26).
 * Returns true if the access may proceed; false after raising the fault.
 * Called before the access and any base writeback, matching the
 * architecture's per-instruction check. Inline, since every load and store
 * asks; the fault is out of line. User mode has never checked: its
 * sysreg_init leaves SA0 clear (a Linux kernel does set it, and answers the
 * fault with SIGBUS; adopting that would be a behaviour change of its own). */
__attribute__((cold)) static bool sp_align_fault(CPU *c) {
    cpu_raise_sync(c, esr_make(EC_SP_ALIGN, 0), 0);
    return false;
}
static inline bool sp_align_ok(CPU *c, unsigned Rn) {
    if (Rn != 31 || !c->sp_chk) return true;       /* not SP, or not checked */
    if ((reg_xsp(c, 31) & 15) == 0) return true;   /* 16-byte aligned */
    return sp_align_fault(c);
}

/* FEAT_LRCPC2: LDAPUR/STLUR, load-acquire RCpc / store-release with an
 * unscaled 9-bit immediate. The acquire/release ordering takes nothing extra
 * here: arm64emu's single in-order CPU has none to give, and in arm64chroot it
 * piggybacks on the host's ordinary load/store ordering (same as LDAR/LDAPR).
 * The access is an ordinary unscaled load/store. No writeback, and no SIMD&FP
 * forms at LRCPC=2. */
static void ldst_rcpc_unscaled(CPU *c, u32 insn) {
    unsigned size = BITS(31, 30), opc = BITS(23, 22);
    unsigned Rn = BITS(9, 5), Rt = BITS(4, 0);
    /* The opc==2/size==3 slot is unallocated here (no PRFM in this space) and
     * must not reach do_load, which would treat it as a prefetch no-op. */
    if ((opc == 2 && size == 3) || (opc == 3 && size >= 2)) {
        undefined(c, insn);
        return;
    }
    if (!sp_align_ok(c, Rn)) return;
    u64 va = reg_xsp(c, Rn) + (u64)(s64)sign_extend(BITS(20, 12), 9);
    if (opc == 0) mem_write(c, va, 1u << size, reg_x(c, Rt));   /* STLUR */
    else do_load(c, Rt, va, size, opc);                         /* LDAPUR* */
}

/* ---------------- FEAT_MOPS: Memory Copy and Memory Set ----------------
 * CPYFx (memcpy, forced forward), CPYx (memmove) and SETx, in the "Option A"
 * register format, matching qemu's implementation choice step for step so the
 * guest-visible intermediate state is differentially testable: the prologue
 * performs up to the next page boundary and only then rewrites Xd[,Xs] to the
 * final address and Xn to -(bytes remaining), writing NZCV=0000 to advertise
 * Option A; M does the whole-page middle; E does the sub-page tail and raises
 * the EC 0x27 mismatch exception if it finds a page or more still to do.
 * M/E executed with PSTATE.C set (an Option B state) raise EC 0x27 with the
 * wrong-option bit; in arm64chroot loop.c plays the kernel's role for those
 * (arm64_mops_reset_regs + restart at the prologue). Restartability across
 * data aborts: P keeps the registers in input format until it completes, M/E
 * fold progress into Xn after every bounded step, so re-executing the faulting
 * instruction always resumes correctly. A step never crosses a page on either
 * address, which makes each step fault-atomic on RAM (permissions are
 * page-granular). The unprivileged T forms take EL0's view at EL1 and are the
 * plain forms at EL0. */

static u64 mops_page_limit(u64 addr)     { return ((addr + 0x1000) & ~0xfffULL) - addr; }
static u64 mops_page_limit_rev(u64 addr) { return (addr & 0xfffULL) + 1; }

/* ISS for EC_MOP (same layout qemu's syn_mop builds, = ESR_ELx for EC 0x27):
 * isSET[24] | options[22:19] | fromEpilogue[18] | wrongOption[17] |
 * OptionA[16] | destreg[14:10] | srcreg[9:5] | sizereg[4:0]. */
static u32 mops_iss(u32 insn, bool wrong_option) {
    bool is_set = BITS(23, 22) == 3;
    unsigned options = is_set ? BITS(13, 12) : BITS(15, 12);
    unsigned epilogue = is_set ? (BITS(15, 14) == 2) : (BITS(23, 22) == 2);
    return ((u32)is_set << 24) | (options << 19) | (epilogue << 18)
         | ((u32)wrong_option << 17) | (1u << 16)
         | (BITS(4, 0) << 10) | (BITS(20, 16) << 5) | BITS(9, 5);
}

/* At EL0 the family is gated by SCTLR_EL1.MSCEn (UNDEF when clear; arm64chroot's
 * kernel-role sysreg_init sets it, as Linux does when it advertises
 * HWCAP2_MOPS); at EL1 it is always enabled (no EL2, so HCRX_EL2.MSCEn doesn't
 * apply). */
static bool mops_enabled_ok(CPU *c, u32 insn) {
    if (c->el == 0 && !((c->sctlr[1] >> 33) & 1)) { undefined(c, insn); return false; }
    return true;
}

/* One bounded memset step: at most to the next page boundary. Fast path is
 * the program's host pointer (mem_host_ptr: in arm64emu a D-TLB hit on a RAM
 * page with W granted — the same fast path mem_write uses, so JIT code-page
 * coherence is preserved; in arm64chroot the translated host pointer, with the
 * coherence of ordinary stores — translated code relies on IC IVAU); on a miss, a single byte through the faulting slow
 * path, which also fills the D-TLB so the next step goes fast. Returns bytes
 * done; 0 means a fault was raised. */
static u64 mops_set_step(CPU *c, u64 toaddr, u64 setsize, u8 data, bool unpriv) {
    u64 n = mops_page_limit(toaddr);
    if (n > setsize) n = setsize;
    set_ldst_unpriv(c, unpriv);
    void *hp = mem_host_ptr(c, toaddr, (unsigned)n, ACC_WRITE);
    if (hp) { set_ldst_unpriv(c, 0); memset(hp, data, (size_t)n); return n; }
    bool ok = mem_write(c, toaddr, 1, data);
    set_ldst_unpriv(c, 0);
    return ok ? 1 : 0;
}

/* One bounded copy step. rev=true means toaddr/fromaddr point at the *last*
 * byte and the step works downwards. memmove makes any same-chunk overlap
 * safe; the read and write sides carry independent unprivileged views (the
 * CPYxRT/WT forms). */
static u64 mops_copy_step(CPU *c, u64 toaddr, u64 fromaddr, u64 copysize,
                          bool wunpriv, bool runpriv, bool rev) {
    u64 n = rev ? mops_page_limit_rev(toaddr) : mops_page_limit(toaddr);
    u64 m = rev ? mops_page_limit_rev(fromaddr) : mops_page_limit(fromaddr);
    if (m < n) n = m;
    if (n > copysize) n = copysize;
    set_ldst_unpriv(c, wunpriv);
    void *w = mem_host_ptr(c, rev ? toaddr - (n - 1) : toaddr,
                           (unsigned)n, ACC_WRITE);
    set_ldst_unpriv(c, runpriv);
    void *r = mem_host_ptr(c, rev ? fromaddr - (n - 1) : fromaddr,
                           (unsigned)n, ACC_READ);
    set_ldst_unpriv(c, 0);
    if (w && r) { memmove(w, r, (size_t)n); return n; }
    u64 byte;
    set_ldst_unpriv(c, runpriv);
    if (!mem_read(c, fromaddr, 1, &byte)) { set_ldst_unpriv(c, 0); return 0; }
    set_ldst_unpriv(c, wunpriv);
    bool ok = mem_write(c, toaddr, 1, byte);
    set_ldst_unpriv(c, 0);
    return ok ? 1 : 0;
}

static void mops_set(CPU *c, u32 insn, unsigned stage) {
    unsigned Rd = BITS(4, 0), Rn = BITS(9, 5), Rs = BITS(20, 16);
    bool unpriv = unpriv_at_el1(c, BIT(12));  /* SETxT: EL0-view stores at EL1 */
    u8 data = (u8)reg_x(c, Rs);               /* Rs may be XZR */
    if (stage == 0) {                                               /* SETP */
        u64 toaddr = reg_x(c, Rd), setsize = reg_x(c, Rn);
        if (setsize > 0x7fffffffffffffffULL) setsize = 0x7fffffffffffffffULL;
        u64 stagesetsize = mops_page_limit(toaddr);
        if (stagesetsize > setsize) stagesetsize = setsize;
        while (stagesetsize) {
            set_x(c, Rd, toaddr);             /* input format until completion */
            set_x(c, Rn, setsize);
            u64 step = mops_set_step(c, toaddr, stagesetsize, data, unpriv);
            if (!step) return;
            toaddr += step; setsize -= step; stagesetsize -= step;
        }
        set_x(c, Rd, toaddr + setsize);
        set_x(c, Rn, 0 - setsize);
        c->nzcv = 0;                          /* NZCV=0000: Option A */
        return;
    }
    u64 xn = reg_x(c, Rn);                                   /* SETM / SETE */
    if (xn == 0) return;                      /* nothing left: NOP, no checks */
    if (c->nzcv & PS_C) {
        cpu_raise_sync(c, esr_make(EC_MOP, mops_iss(insn, true)), 0);
        return;
    }
    u64 toaddr = reg_x(c, Rd) + xn, setsize = 0 - xn;
    if (stage == 2 && setsize >= 0x1000) {    /* SETE takes only the tail */
        cpu_raise_sync(c, esr_make(EC_MOP, mops_iss(insn, false)), 0);
        return;
    }
    u64 stagesetsize = (stage == 1) ? (setsize & ~0xfffULL) : setsize;
    while (stagesetsize) {
        u64 step = mops_set_step(c, toaddr, setsize, data, unpriv);
        if (!step) return;
        toaddr += step; setsize -= step;
        stagesetsize = (step >= stagesetsize) ? 0 : stagesetsize - step;
        set_x(c, Rn, 0 - setsize);
    }
}

static void mops_cpy(CPU *c, u32 insn, unsigned stage, bool move) {
    unsigned Rd = BITS(4, 0), Rn = BITS(9, 5), Rs = BITS(20, 16);
    bool wunpriv = unpriv_at_el1(c, BIT(12)), runpriv = unpriv_at_el1(c, BIT(13));
    if (stage == 0) {                                            /* CPY[F]P */
        bool fwd = true;
        u64 toaddr = reg_x(c, Rd), fromaddr = reg_x(c, Rs), copysize = reg_x(c, Rn);
        if (move) {
            /* Direction: backward only when the source starts below an
             * overlapping destination; non-overlap is IMPDEF-forward. */
            if (copysize > 0x007fffffffffffffULL) copysize = 0x007fffffffffffffULL;
            u64 fs = fromaddr & 0xffffffffffffffULL, ts = toaddr & 0xffffffffffffffULL;
            u64 fe = (fromaddr + copysize) & 0xffffffffffffffULL;
            if (fs < ts && fe > ts) fwd = false;
        } else if (copysize > 0x7fffffffffffffffULL) {
            copysize = 0x7fffffffffffffffULL;
        }
        if (fwd) {
            u64 stagecopysize = mops_page_limit(toaddr), m = mops_page_limit(fromaddr);
            if (m < stagecopysize) stagecopysize = m;
            if (stagecopysize > copysize) stagecopysize = copysize;
            while (stagecopysize) {
                set_x(c, Rd, toaddr);         /* input format until completion */
                set_x(c, Rs, fromaddr);
                set_x(c, Rn, copysize);
                u64 step = mops_copy_step(c, toaddr, fromaddr, stagecopysize,
                                          wunpriv, runpriv, false);
                if (!step) return;
                toaddr += step; fromaddr += step; copysize -= step; stagecopysize -= step;
            }
            set_x(c, Rd, toaddr + copysize);
            set_x(c, Rs, fromaddr + copysize);
            set_x(c, Rn, 0 - copysize);
        } else {
            /* Backward: work from the last byte down. The completed-P register
             * format is the same as the input format (Xn stays positive, which
             * is how M/E recognise the direction). */
            u64 t = toaddr + copysize - 1, f = fromaddr + copysize - 1;
            u64 stagecopysize = mops_page_limit_rev(t), m = mops_page_limit_rev(f);
            if (m < stagecopysize) stagecopysize = m;
            if (stagecopysize > copysize) stagecopysize = copysize;
            while (stagecopysize) {
                set_x(c, Rn, copysize);
                u64 step = mops_copy_step(c, t, f, stagecopysize,
                                          wunpriv, runpriv, true);
                if (!step) return;
                copysize -= step; stagecopysize -= step; t -= step; f -= step;
            }
            set_x(c, Rn, copysize);
        }
        c->nzcv = 0;                          /* NZCV=0000: Option A */
        return;
    }
    u64 xn = reg_x(c, Rn);                              /* CPY[F]M / CPY[F]E */
    if (xn == 0) return;
    if (c->nzcv & PS_C) {
        cpu_raise_sync(c, esr_make(EC_MOP, mops_iss(insn, true)), 0);
        return;
    }
    bool fwd = !move || (s64)xn < 0;
    u64 toaddr, fromaddr, copysize;
    if (fwd) {
        toaddr = reg_x(c, Rd) + xn; fromaddr = reg_x(c, Rs) + xn; copysize = 0 - xn;
    } else {
        copysize = xn;
        toaddr = reg_x(c, Rd) + copysize - 1; fromaddr = reg_x(c, Rs) + copysize - 1;
    }
    if (stage == 2 && copysize >= 0x1000) {   /* CPY[F]E takes only the tail */
        cpu_raise_sync(c, esr_make(EC_MOP, mops_iss(insn, false)), 0);
        return;
    }
    /* M runs while a full page remains; E runs to zero. */
    while (stage == 1 ? copysize >= 0x1000 : copysize > 0) {
        u64 step = mops_copy_step(c, toaddr, fromaddr, copysize,
                                  wunpriv, runpriv, !fwd);
        if (!step) return;
        if (fwd) { toaddr += step; fromaddr += step; }
        else     { toaddr -= step; fromaddr -= step; }
        copysize -= step;
        set_x(c, Rn, fwd ? 0 - copysize : copysize);
    }
}

static void mops(CPU *c, u32 insn) {
    unsigned op1 = BITS(23, 22);              /* CPY stage; 11 = SET family */
    unsigned Rd = BITS(4, 0), Rn = BITS(9, 5), Rs = BITS(20, 16);
    if (BITS(31, 30) != 0) { undefined(c, insn); return; }
    if (op1 == 3) {
        unsigned stage = BITS(15, 14);        /* 0 P, 1 M, 2 E; 3 unallocated */
        /* bit26 set = SETG* (MTE tag-setting): not implemented, UNDEF.
         * Rd==Rn, Rd==Rs, Rn==Rs, Rd/Rn==31 are CONSTRAINED UNPREDICTABLE;
         * UNDEF like qemu (Rs==31 is a valid XZR value operand). */
        if (BIT(26) || stage == 3 ||
            Rs == Rn || Rs == Rd || Rn == Rd || Rd == 31 || Rn == 31) {
            undefined(c, insn); return;
        }
        if (!mops_enabled_ok(c, insn)) return;
        mops_set(c, insn, stage);
    } else {
        if (Rs == Rn || Rs == Rd || Rn == Rd || Rd == 31 || Rs == 31 || Rn == 31) {
            undefined(c, insn); return;
        }
        if (!mops_enabled_ok(c, insn)) return;
        mops_cpy(c, insn, op1, BIT(26));
    }
}

static void ldst_register(CPU *c, u32 insn) {
    unsigned size = BITS(31, 30), opc = BITS(23, 22);
    unsigned Rn = BITS(9, 5), Rt = BITS(4, 0);
    bool V = BIT(26);
    bool is_store, is_load;
    unsigned bytes, scale;
    if (V) {
        is_load = opc & 1;
        /* opc<1> selects the 128-bit Q form, which is defined only for size==0;
         * size!=0 with opc&2 is unallocated. */
        if ((opc & 2) && size != 0) { undefined(c, insn); return; }
        if (!fp_access_ok(c)) return;
        bytes = (opc & 2) ? 16 : (1u << size);
        scale = (opc & 2) ? 4 : size;
    } else {
        is_load = (opc != 0);
        bytes = 1u << size;
        scale = size;
    }
    is_store = !is_load;

    /* compute effective address and optional writeback */
    u64 va, base = reg_xsp(c, Rn);
    if (!sp_align_ok(c, Rn)) return;
    int wb = 0;          /* 0 none, 1 post, 2 pre */
    bool unpriv = false; /* LDTR/STTR: access checked with EL0 permissions */
    if (BIT(24)) {                              /* unsigned immediate offset */
        va = base + ((u64)BITS(21, 10) << scale);
    } else if (BIT(21)) {                       /* register offset or atomic */
        if (BITS(11, 10) == 0 && !V) {          /* LSE atomic memory operation */
            ldst_atomic(c, insn);
            return;
        }
        if (BITS(11, 10) != 2) { undefined(c, insn); return; }
        va = base + ldst_extended_offset(c, insn, scale);
    } else {
        unsigned mode = BITS(11, 10);
        s64 imm9 = (s64)sign_extend(BITS(20, 12), 9);
        if (mode == 0 || mode == 2) {           /* unscaled / unprivileged LDTR/STTR */
            if (mode == 2) {
                if (V) { undefined(c, insn); return; }  /* no SIMD&FP unprivileged form */
                /* At EL0 LDTR/STTR behave as LDR/STR; EL2+ isn't modeled here. */
                unpriv = unpriv_at_el1(c, true);
            }
            va = base + imm9;
        }
        else if (mode == 1) { va = base; wb = 1; }  /* post */
        else { va = base + imm9; wb = 2; }          /* pre */
        if (wb == 1) base = base + imm9;            /* post writeback value */
    }

    /* opc==3 is the "load signed, 32-bit result" column, which exists only for
     * the byte and halfword sizes: size==2 (LDRSW takes opc==2) and size==3 are
     * both unallocated. LSE atomics reach this function too but return above,
     * so opc here is always the size/sign field. size==3 was executed as a
     * 64-bit signed load truncated to 32; size==2 as a sign-extending word load
     * truncated to 32 — qemu raises SIGILL for both. */
    if (!V && opc == 3 && size >= 2) { undefined(c, insn); return; }

    bool ok;
    set_ldst_unpriv(c, unpriv); /* set only across this one access (mmu.c acc_el0) */
    if (V) ok = is_store ? vreg_store(c, Rt, va, bytes) : vreg_load(c, Rt, va, bytes);
    else   ok = is_store ? mem_write(c, va, bytes, reg_x(c, Rt)) : do_load(c, Rt, va, size, opc);
    set_ldst_unpriv(c, 0);      /* fault paths return through here too (no longjmp) */
    if (!ok) return;   /* faulted: do NOT write back the base (instruction re-executes) */

    if (wb == 1) set_xsp(c, Rn, base);
    else if (wb == 2) set_xsp(c, Rn, va);
}

static void ldst_pair(CPU *c, u32 insn) {
    unsigned opc = BITS(31, 30);
    bool V = BIT(26);
    unsigned mode = BITS(25, 23);   /* 000 STNP/LDNP,001 post,010 offset,011 pre */
    bool L = BIT(22);
    u64 imm7 = sign_extend(BITS(21, 15), 7);
    unsigned Rt2 = BITS(14, 10), Rn = BITS(9, 5), Rt = BITS(4, 0);

    unsigned scale, esz;
    bool signed_word = false;
    if (V) {
        /* opc==3 is unallocated. It must be rejected before scale is derived:
         * scale = opc+2 would make esz 32, and a 32-byte "element" reaches
         * vreg_load/vreg_store, whose non-16 path passes the size straight to
         * mem_read/mem_write — i.e. a 32-byte memcpy into (or out of) a u64
         * stack slot. Guest-triggerable emulator stack corruption one way and
         * host stack disclosure into guest memory the other. */
        if (opc == 3) { undefined(c, insn); return; }
        scale = opc + 2; esz = 1u << scale;              /* S/D/Q = 4/8/16 bytes */
    } else {
        if (opc == 3) { undefined(c, insn); return; }    /* unallocated (was run as a 32-bit pair) */
        scale = (opc == 2) ? 3 : 2; esz = 1u << scale; signed_word = (opc == 1);
    }
    u64 offset = imm7 << scale;

    u64 base = reg_xsp(c, Rn), addr;
    if (!sp_align_ok(c, Rn)) return;
    bool wb = false; u64 wbval = 0;
    switch (mode) {
        case 0: addr = base + offset; break;                            /* STNP/LDNP (non-temporal) */
        case 1: addr = base; wb = true; wbval = base + offset; break;   /* post */
        case 2: addr = base + offset; break;                            /* offset */
        case 3: addr = base + offset; wb = true; wbval = addr; break;   /* pre */
        default: undefined(c, insn); return;
    }
    if (V) {
        if (!fp_access_ok(c)) return;
        bool ok = L ? (vreg_load(c, Rt, addr, esz) && vreg_load(c, Rt2, addr + esz, esz))
                    : (vreg_store(c, Rt, addr, esz) && vreg_store(c, Rt2, addr + esz, esz));
        if (!ok) return;   /* faulted: skip writeback (instruction re-executes) */
    } else if (L) {
        if (esz == 8) {                    /* LDP Xt: one 16-byte access */
            V128 v;
            if (!mem_read128(c, addr, &v)) return;
            set_x(c, Rt, v.d[0]); set_x(c, Rt2, v.d[1]);
        } else {
            u64 a, b;
            if (!mem_read(c, addr, esz, &a)) return;
            if (!mem_read(c, addr + esz, esz, &b)) return;
            if (signed_word) { set_x(c, Rt, sign_extend(a, 32)); set_x(c, Rt2, sign_extend(b, 32)); }
            else { set_x(c, Rt, (u32)a); set_x(c, Rt2, (u32)b); }
        }
    } else {
        if (esz == 8) {                    /* STP Xt: one 16-byte access */
            V128 v;
            v.d[0] = reg_x(c, Rt); v.d[1] = reg_x(c, Rt2);
            if (!mem_write128(c, addr, &v)) return;
        } else {
            if (!mem_write(c, addr, esz, reg_x(c, Rt))) return;
            if (!mem_write(c, addr + esz, esz, reg_x(c, Rt2))) return;
        }
    }
    if (wb) set_xsp(c, Rn, wbval);
}

static void ldst_literal(CPU *c, u32 insn) {
    unsigned opc = BITS(31, 30);
    bool V = BIT(26);
    unsigned Rt = BITS(4, 0);
    u64 off = sign_extend(BITS(23, 5), 19) << 2;
    u64 va = c->cur_insn_pc + off;
    if (V) {                                     /* SIMD&FP: LDR St/Dt/Qt (literal) */
        if (opc == 3) { undefined(c, insn); return; }   /* UNALLOCATED */
        if (!fp_access_ok(c)) return;
        vreg_load(c, Rt, va, 4u << opc);         /* opc 0/1/2 -> 4/8/16 bytes */
        return;
    }
    u64 raw;
    switch (opc) {
        case 0: if (mem_read(c, va, 4, &raw)) set_x(c, Rt, (u32)raw); break;       /* LDR Wt */
        case 1: if (mem_read(c, va, 8, &raw)) set_x(c, Rt, raw); break;            /* LDR Xt */
        case 2: if (mem_read(c, va, 4, &raw)) set_x(c, Rt, sign_extend(raw, 32)); break; /* LDRSW */
        default: break;  /* PRFM literal: nop */
    }
}

/* On one CPU: plain accesses, and an address match is the monitor. */
static NOINL void excl_up(CPU *c, u32 insn, u64 va) {
    unsigned size = BITS(31, 30);
    bool o2 = BIT(23), L = BIT(22), o1 = BIT(21);
    unsigned Rs = BITS(20, 16), Rt2 = BITS(14, 10), Rt = BITS(4, 0);
    unsigned bytes = 1u << size;
    if (o2) {                                  /* LDAR / STLR (ordered, non-exclusive) */
        if (L) { u64 v; if (mem_read(c, va, bytes, &v)) set_x(c, Rt, v); }
        else mem_write(c, va, bytes, reg_x(c, Rt));
        return;
    }
    if (!o1) {                                 /* single-register exclusive */
        if (L) {                               /* LDXR / LDAXR */
            u64 v;
            if (!mem_read(c, va, bytes, &v)) return;
            set_x(c, Rt, v);
            c->excl_valid = true; c->excl_addr = va; c->excl_size = bytes;
        } else {                               /* STXR / STLXR */
            if (c->excl_valid && c->excl_addr == va) {
                if (!mem_write(c, va, bytes, reg_x(c, Rt))) return;
                set_x(c, Rs, 0);
            } else {
                set_x(c, Rs, 1);
            }
            c->excl_valid = false;
        }
        return;
    }
    /* pair exclusive LDXP/STXP */
    if (L) {
        u64 a, b;
        if (!mem_read(c, va, bytes, &a)) return;
        if (!mem_read(c, va + bytes, bytes, &b)) return;
        set_x(c, Rt, a); set_x(c, Rt2, b);
        c->excl_valid = true; c->excl_addr = va; c->excl_size = bytes * 2;
    } else {
        if (c->excl_valid && c->excl_addr == va) {
            if (!mem_write(c, va, bytes, reg_x(c, Rt))) return;
            if (!mem_write(c, va + bytes, bytes, reg_x(c, Rt2))) return;
            set_x(c, Rs, 0);
        } else set_x(c, Rs, 1);
        c->excl_valid = false;
    }
}

/* Where guest threads run concurrently: LDAR/STLR are host atomic accesses,
 * and the exclusive monitor compares the value LDXR loaded (a host CAS).
 * Memory with no host pointer (mem_host_ptr) is accessed plainly, which
 * faults where the architecture does. */
static NOINL void excl_smp(CPU *c, u32 insn, u64 va) {
    unsigned size = BITS(31, 30);
    bool o2 = BIT(23), L = BIT(22), o1 = BIT(21);
    unsigned Rs = BITS(20, 16), Rt2 = BITS(14, 10), Rt = BITS(4, 0);
    unsigned bytes = 1u << size;
    if (o2) {                                  /* LDAR / STLR (ordered, non-exclusive) */
        /* These are the architecture's acquire/release accesses and commonly
         * pair with CAS/exclusives on the same word (locks). They must be host
         * ATOMIC accesses, not plain memcpy, or a release store races
         * non-atomically with another thread's CAS and the lock loses updates.
         * Falls back to a fenced plain access only when a stable host pointer
         * isn't available (page-crossing/unmapped). */
        void *hp = mem_host_ptr(c, va, bytes, L ? ACC_READ : ACC_WRITE);
        if (L) {                               /* LDAR: load-acquire */
            u64 v;
            if (hp) {
                switch (bytes) {
                    case 1: v = __atomic_load_n((u8 *)hp, __ATOMIC_ACQUIRE); break;
                    case 2: v = __atomic_load_n((u16 *)hp, __ATOMIC_ACQUIRE); break;
                    case 4: v = __atomic_load_n((u32 *)hp, __ATOMIC_ACQUIRE); break;
                    default: v = __atomic_load_n((u64 *)hp, __ATOMIC_ACQUIRE); break;
                }
                set_x(c, Rt, v);
            } else {
                if (mem_read(c, va, bytes, &v)) set_x(c, Rt, v);
                __atomic_thread_fence(__ATOMIC_ACQUIRE);
            }
        } else {                               /* STLR: store-release */
            u64 v = reg_x(c, Rt);
            if (hp) {
                switch (bytes) {
                    case 1: __atomic_store_n((u8 *)hp, (u8)v, __ATOMIC_RELEASE); break;
                    case 2: __atomic_store_n((u16 *)hp, (u16)v, __ATOMIC_RELEASE); break;
                    case 4: __atomic_store_n((u32 *)hp, (u32)v, __ATOMIC_RELEASE); break;
                    default: __atomic_store_n((u64 *)hp, v, __ATOMIC_RELEASE); break;
                }
            } else {
                __atomic_thread_fence(__ATOMIC_RELEASE);
                mem_write(c, va, bytes, v);
            }
        }
        return;
    }
    bool acq = BIT(15);   /* o0: LDAXR/STLXR ordering */
    if (!o1) {                                 /* single-register exclusive */
        if (L) {                               /* LDXR / LDAXR */
            u64 v;
            if (!mem_read(c, va, bytes, &v)) return;
            set_x(c, Rt, v);
            c->excl_valid = true; c->excl_addr = va; c->excl_size = bytes;
            c->excl_val = v;                   /* recorded for the STXR CAS */
            if (acq) __atomic_thread_fence(__ATOMIC_ACQUIRE);
        } else {                               /* STXR / STLXR */
            unsigned res = 1;
            if (c->excl_valid && c->excl_addr == va && c->excl_size == bytes) {
                /* SMP-correct: compare-and-swap against the value LDXR loaded.
                 * A concurrent write by another guest thread changed memory ->
                 * the CAS fails -> STXR fails, exactly as the architecture
                 * requires. Falls back to a plain store only when the address
                 * can't be a stable host pointer (page-crossing/unmapped). */
                void *hp = mem_host_ptr(c, va, bytes, ACC_WRITE);
                if (hp) {
                    u64 want = reg_x(c, Rt), exp = c->excl_val;
                    if (acq) __atomic_thread_fence(__ATOMIC_RELEASE);
                    bool ok;
                    switch (bytes) {
                        case 1: { u8  e=(u8)exp;  ok=__atomic_compare_exchange_n((u8*)hp,&e,(u8)want,false,__ATOMIC_SEQ_CST,__ATOMIC_RELAXED); break; }
                        case 2: { u16 e=(u16)exp; ok=__atomic_compare_exchange_n((u16*)hp,&e,(u16)want,false,__ATOMIC_SEQ_CST,__ATOMIC_RELAXED); break; }
                        case 4: { u32 e=(u32)exp; ok=__atomic_compare_exchange_n((u32*)hp,&e,(u32)want,false,__ATOMIC_SEQ_CST,__ATOMIC_RELAXED); break; }
                        default:{ u64 e=exp;      ok=__atomic_compare_exchange_n((u64*)hp,&e,want,false,__ATOMIC_SEQ_CST,__ATOMIC_RELAXED); break; }
                    }
                    res = ok ? 0 : 1;
                } else {
                    if (acq) __atomic_thread_fence(__ATOMIC_RELEASE);
                    if (!mem_write(c, va, bytes, reg_x(c, Rt))) return;
                    res = 0;
                }
            }
            set_x(c, Rs, res);
            c->excl_valid = false;
        }
        return;
    }
    /* pair exclusive LDXP/STXP */
    if (L) {
        u64 a, b;
        if (!mem_read(c, va, bytes, &a)) return;
        if (!mem_read(c, va + bytes, bytes, &b)) return;
        set_x(c, Rt, a); set_x(c, Rt2, b);
        c->excl_valid = true; c->excl_addr = va; c->excl_size = bytes * 2;
        c->excl_val = a; c->excl_val2 = b;
        if (acq) __atomic_thread_fence(__ATOMIC_ACQUIRE);
    } else {
        unsigned res = 1;
        if (c->excl_valid && c->excl_addr == va && c->excl_size == bytes * 2) {
            if (acq) __atomic_thread_fence(__ATOMIC_RELEASE);
            if (bytes == 8) {                  /* 16-byte pair: one u128 CAS */
                void *hp = mem_host_ptr(c, va, 16, ACC_WRITE);
                if (hp) {
#ifdef __SIZEOF_INT128__
                    unsigned __int128 exp = (unsigned __int128)c->excl_val |
                                            ((unsigned __int128)c->excl_val2 << 64);
                    unsigned __int128 want = (unsigned __int128)reg_x(c, Rt) |
                                             ((unsigned __int128)reg_x(c, Rt2) << 64);
                    res = __atomic_compare_exchange_n((unsigned __int128 *)hp, &exp,
                            want, false, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED) ? 0 : 1;
#else
                    extern void casp16_mutex_lock(void);
                    extern void casp16_mutex_unlock(void);
                    casp16_mutex_lock();
                    u64 cur0, cur1;
                    if (mem_read(c, va, 8, &cur0) && mem_read(c, va + 8, 8, &cur1) &&
                        cur0 == c->excl_val && cur1 == c->excl_val2) {
                        mem_write(c, va, 8, reg_x(c, Rt));
                        mem_write(c, va + 8, 8, reg_x(c, Rt2));
                        res = 0;
                    }
                    casp16_mutex_unlock();
#endif
                } else {
                    if (!mem_write(c, va, 8, reg_x(c, Rt))) return;
                    if (!mem_write(c, va + 8, 8, reg_x(c, Rt2))) return;
                    res = 0;
                }
            } else {                            /* 8-byte pair: one u64 CAS */
                void *hp = mem_host_ptr(c, va, 8, ACC_WRITE);
                if (hp) {
                    u64 exp = (c->excl_val & 0xffffffff) | (c->excl_val2 << 32);
                    u64 want = (reg_x(c, Rt) & 0xffffffff) | (reg_x(c, Rt2) << 32);
                    res = __atomic_compare_exchange_n((u64 *)hp, &exp, want, false,
                            __ATOMIC_SEQ_CST, __ATOMIC_RELAXED) ? 0 : 1;
                } else {
                    if (!mem_write(c, va, 4, reg_x(c, Rt))) return;
                    if (!mem_write(c, va + 4, 4, reg_x(c, Rt2))) return;
                    res = 0;
                }
            }
        }
        set_x(c, Rs, res);
        c->excl_valid = false;
    }
}

static void ldst_exclusive(CPU *c, u32 insn) {
    bool o2 = BIT(23), o1 = BIT(21);
    unsigned Rn = BITS(9, 5);
    u64 va = reg_xsp(c, Rn);
    if (!sp_align_ok(c, Rn)) return;

    if (o2 && o1) {                            /* CAS/CASA/CASL/CASAL (LSE) */
        ldst_cas(c, insn);
        return;
    }
    if (!o2 && o1 && BIT(31) == 0) {           /* CASP/CASPA/CASPL/CASPAL (LSE) */
        ldst_casp(c, insn);
        return;
    }
    /* Ordered and exclusive accesses, by the CPU's mode. */
    if (c->eng.smp) excl_smp(c, insn, va);
    else excl_up(c, insn, va);
}

/* AdvSIMD load/store multiple structures: LD1/ST1 (contiguous, 1-4 registers)
 * and LD2/3/4/ST2/3/4 (de-interleaved). Used pervasively for memcpy/memset and
 * NEON block I/O by both EDK2 and Linux. Supports the post-indexed form. */
static void ldst_vector_multi(CPU *c, u32 insn) {
    if (!fp_access_ok(c)) return;
    unsigned Q = BIT(30), post = BIT(23), L = BIT(22), Rm = BITS(20, 16);
    unsigned opcode = BITS(15, 12), size = BITS(11, 10), Rn = BITS(9, 5), Rt = BITS(4, 0);
    unsigned nregs, sel;     /* sel = interleave factor (1 = contiguous LD1/ST1) */
    switch (opcode) {
        case 0x0: nregs = 4; sel = 4; break;   /* LD4/ST4 */
        case 0x2: nregs = 4; sel = 1; break;   /* LD1/ST1 x4 */
        case 0x4: nregs = 3; sel = 3; break;   /* LD3/ST3 */
        case 0x6: nregs = 3; sel = 1; break;   /* LD1/ST1 x3 */
        case 0x7: nregs = 1; sel = 1; break;   /* LD1/ST1 x1 */
        case 0x8: nregs = 2; sel = 2; break;   /* LD2/ST2 */
        case 0xa: nregs = 2; sel = 1; break;   /* LD1/ST1 x2 */
        default: undefined(c, insn); return;
    }
    unsigned regbytes = Q ? 16 : 8;
    u64 base = reg_xsp(c, Rn), addr = base;
    if (!sp_align_ok(c, Rn)) return;
    unsigned total = nregs * regbytes;

    if (sel == 1) {                            /* contiguous: whole registers */
        for (unsigned r = 0; r < nregs; r++) {
            unsigned vt = (Rt + r) & 31;
            if (L) {
                V128 v; v.d[0] = v.d[1] = 0;
                if (Q) { if (!mem_read128(c, addr, &v)) return; }
                else   { u64 t; if (!mem_read(c, addr, 8, &t)) return; v.d[0] = t; }
                c->v[vt] = v;
            } else {
                if (Q) { if (!mem_write128(c, addr, &c->v[vt])) return; }
                else   { if (!mem_write(c, addr, 8, c->v[vt].d[0])) return; }
            }
            addr += regbytes;
        }
    } else {                                   /* de-interleaved element by element */
        unsigned ebytes = 1u << size, elems = regbytes / ebytes;
        if (L) for (unsigned r = 0; r < nregs; r++) { c->v[(Rt + r) & 31].d[0] = 0; c->v[(Rt + r) & 31].d[1] = 0; }
        for (unsigned e = 0; e < elems; e++)
            for (unsigned r = 0; r < nregs; r++) {
                unsigned vt = (Rt + r) & 31, off = e * ebytes;
                if (L) { u64 t = 0; if (!mem_read(c, addr, ebytes, &t)) return; memcpy(&c->v[vt].b[off], &t, ebytes); }
                else   { u64 t = 0; memcpy(&t, &c->v[vt].b[off], ebytes); if (!mem_write(c, addr, ebytes, t)) return; }
                addr += ebytes;
            }
    }

    if (post) set_xsp(c, Rn, base + ((Rm == 31) ? total : reg_x(c, Rm)));
}

/* AdvSIMD load/store single structure: LD1/2/3/4 and ST1/2/3/4 to/from a single
 * lane, plus LD1R/2R/3R/4R (load one element and replicate it across all lanes).
 * Single-lane loads leave the other lanes of the destination unchanged. Used by
 * memset/strlen-class routines and struct-of-arrays NEON code. Post-indexed
 * form supported. */
static void ldst_vector_single(CPU *c, u32 insn) {
    if (!fp_access_ok(c)) return;
    unsigned Q = BIT(30), post = BIT(23), L = BIT(22), R = BIT(21);
    unsigned Rm = BITS(20, 16), opcode = BITS(15, 13), S = BIT(12);
    unsigned size = BITS(11, 10), Rn = BITS(9, 5), Rt = BITS(4, 0);

    unsigned scale = opcode >> 1;                       /* opcode<2:1> */
    unsigned selem = (((opcode & 1) << 1) | R) + 1;    /* 1..4 registers */
    bool replicate = false;
    unsigned index = 0, ebytes = 0;

    switch (scale) {
        case 0: ebytes = 1; index = (Q << 3) | (S << 2) | size; break;       /* B */
        case 1:
            if (size & 1) { undefined(c, insn); return; }
            ebytes = 2; index = (Q << 2) | (S << 1) | (size >> 1); break;     /* H */
        case 2:
            if (size & 2) { undefined(c, insn); return; }
            if ((size & 1) == 0) { ebytes = 4; index = (Q << 1) | S; }        /* S */
            else { if (S) { undefined(c, insn); return; } ebytes = 8; index = Q; } /* D */
            break;
        default:                                          /* scale == 3: replicate */
            if (!L || S) { undefined(c, insn); return; }
            replicate = true; ebytes = 1u << size; break;
    }

    u64 base = reg_xsp(c, Rn), addr = base;
    if (!sp_align_ok(c, Rn)) return;
    unsigned total = selem * ebytes;

    for (unsigned r = 0; r < selem; r++) {
        unsigned vt = (Rt + r) & 31;
        u64 elem = 0;
        if (replicate) {
            if (!mem_read(c, addr, ebytes, &elem)) return;
            V128 v; v.d[0] = v.d[1] = 0;
            unsigned lanes = (Q ? 16 : 8) / ebytes;
            for (unsigned i = 0; i < lanes; i++) memcpy(&v.b[i * ebytes], &elem, ebytes);
            c->v[vt] = v;
        } else if (L) {                                   /* load one lane, rest unchanged */
            if (!mem_read(c, addr, ebytes, &elem)) return;
            memcpy(&c->v[vt].b[index * ebytes], &elem, ebytes);
        } else {                                          /* store one lane */
            memcpy(&elem, &c->v[vt].b[index * ebytes], ebytes);
            if (!mem_write(c, addr, ebytes, elem)) return;
        }
        addr += ebytes;
    }

    if (post) set_xsp(c, Rn, base + ((Rm == 31) ? total : reg_x(c, Rm)));
}

static void loads_stores(CPU *c, u32 insn) {
    unsigned b2927 = BITS(29, 27);
    /* Ordered by dynamic frequency (#15): the register forms and pairs are
     * far hotter than exclusives/vector-structure/MOPS. The tests key on
     * disjoint b2927 values (and disjoint subfields within 0x1/0x3), so
     * reordering cannot change which handler wins. */
    if (b2927 == 0x7) { ldst_register(c, insn); return; }
    if (b2927 == 0x5) { ldst_pair(c, insn); return; }
    if (b2927 == 0x3 && BITS(25, 24) == 0) { ldst_literal(c, insn); return; }
    if (b2927 == 0x1 && BITS(26, 24) == 0) { ldst_exclusive(c, insn); return; }
    if (b2927 == 0x1 && BIT(26) == 1 && BIT(25) == 0 && BIT(24) == 0) {
        ldst_vector_multi(c, insn); return;    /* AdvSIMD load/store multiple structures */
    }
    if (b2927 == 0x1 && BIT(26) == 1 && BIT(25) == 0 && BIT(24) == 1) {
        ldst_vector_single(c, insn); return;   /* AdvSIMD load/store single structure */
    }
    if (b2927 == 0x3 && BIT(26) == 0 && BITS(25, 24) == 1 && BIT(21) == 0 &&
        BITS(11, 10) == 0) {
        ldst_rcpc_unscaled(c, insn); return;   /* FEAT_LRCPC2 LDAPUR/STLUR */
    }
    if (b2927 == 0x3 && BITS(25, 24) == 1 && BIT(21) == 0 && BITS(11, 10) == 1) {
        mops(c, insn); return;                 /* FEAT_MOPS CPYx/SETx (size==0 checked inside) */
    }
    undefined(c, insn);
}

/* ================= branches / exceptions / system ================= */
static void branch_system(CPU *c, u32 insn) {
    unsigned top6 = BITS(31, 26);

    if (top6 == 0x05) { c->pc = c->cur_insn_pc + (sign_extend(BITS(25, 0), 26) << 2); return; } /* B */
    if (top6 == 0x25) {                                                  /* BL */
        set_x(c, 30, c->cur_insn_pc + 4);
        c->pc = c->cur_insn_pc + (sign_extend(BITS(25, 0), 26) << 2);
        return;
    }
    if (BITS(31, 24) == 0x54 && BIT(4) == 0) {                            /* B.cond */
        if (cond_holds(c, BITS(3, 0)))
            c->pc = c->cur_insn_pc + (sign_extend(BITS(23, 5), 19) << 2);
        return;
    }
    if (BITS(30, 25) == 0x1a) {                                           /* CBZ/CBNZ */
        bool sf = BIT(31), op = BIT(24);
        unsigned Rt = BITS(4, 0);
        u64 v = reg_x_sz(c, Rt, sf);
        bool take = op ? (v != 0) : (v == 0);
        if (take) c->pc = c->cur_insn_pc + (sign_extend(BITS(23, 5), 19) << 2);
        return;
    }
    if (BITS(30, 25) == 0x1b) {                                           /* TBZ/TBNZ */
        bool op = BIT(24);
        unsigned bitpos = (BIT(31) << 5) | BITS(23, 19);
        unsigned Rt = BITS(4, 0);
        u64 v = reg_x(c, Rt);
        bool set = (v >> bitpos) & 1;
        bool take = op ? set : !set;
        if (take) c->pc = c->cur_insn_pc + (sign_extend(BITS(18, 5), 14) << 2);
        return;
    }
    if (BITS(31, 24) == 0xd4) {                                           /* exception generation */
        unsigned opc = BITS(23, 21), ll = BITS(1, 0), imm16 = BITS(20, 5);
        if (opc == 0 && ll == 1) {                                        /* SVC */
            systrace_svc(c);
            exception_take(c, EXC_SYNC, esr_make(EC_SVC64, imm16), 0, c->pc);
        } else if (opc == 0 && ll == 2) {                                 /* HVC */
            /* HVC is UNDEFINED at EL0 — guest userspace must not reach the PSCI
             * conduit (it could otherwise power off / reboot the machine).
             * A user-mode guest is always at EL0. */
            if (c->el == 0) { undefined(c, insn); return; }
            if (smccc_conduit && smccc_conduit(c, true)) return;
            undefined(c, insn);
        } else if (opc == 0 && ll == 3) {                                 /* SMC */
            if (c->el == 0) { undefined(c, insn); return; }
            if (smccc_conduit && smccc_conduit(c, false)) return;
            undefined(c, insn);
        } else if (opc == 1 && ll == 0) {                                 /* BRK */
            cpu_raise_sync(c, esr_make(EC_BRK64, imm16), 0);
        } else if (opc == 2 && ll == 0 && c->el != 0) {                   /* HLT: stop machine (test exit); EL0 may not halt */
            fprintf(stderr, "[HLT #%u] x0=0x%llx icount=%llu\n", imm16,
                    (unsigned long long)c->x[0], (unsigned long long)c->icount);
            c->stop = true;
        } else undefined(c, insn);
        return;
    }
    if (BITS(31, 25) == 0x6b) {                                           /* unconditional branch (reg) */
        unsigned opc = BITS(24, 21), Rn = BITS(9, 5);
        u64 tgt = reg_x(c, Rn);
        switch (opc) {
            case 0: c->pc = tgt; return;                                  /* BR */
            case 1: set_x(c, 30, c->cur_insn_pc + 4); c->pc = tgt; return;/* BLR */
            case 2: c->pc = reg_x(c, Rn); return;                         /* RET */
            case 4: {                                                     /* ERET */
                if (c->el == 0) { undefined(c, insn); return; }           /* EL1+ only */
                u32 spsr = (u32)c->spsr[c->el];
                u64 elr = c->elr[c->el];
                cpu_unpack_spsr(c, spsr);
                c->pc = elr;
                return;
            }
            default: undefined(c, insn); return;
        }
    }
    if (BITS(31, 24) == 0xd5) {                                           /* system instructions */
        unsigned L = BIT(21), op0 = BITS(20, 19), op1 = BITS(18, 16);
        unsigned CRn = BITS(15, 12), CRm = BITS(11, 8), op2 = BITS(7, 5), Rt = BITS(4, 0);
        if (L == 0 && op0 == 0 && op1 == 3 && CRn == 2 && Rt == 31) {     /* hints */
            if (CRm == 0 && op2 == 2) {                       /* WFE */
                /* A pending event completes WFE immediately (and clears it);
                 * otherwise halt until an event/interrupt. Without this an
                 * SEVL/WFE pair only made progress when the timer tick fired.
                 * At EL0 a WFE that would wait traps to EL1 unless
                 * SCTLR_EL1.nTWE lets it (Linux does). A halt is the
                 * program's to serve: in user mode the run loop takes it as
                 * a no-op. */
                if (c->event) c->event = false;
                else if (c->el == 0 && !(c->sctlr[1] & SCTLR_nTWE)) wfx_trap(c, 1);
                else c->halted = true;
            }
            else if (CRm == 0 && op2 == 3) {                  /* WFI */
                /* ...and a WFI unless SCTLR_EL1.nTWI does: Linux leaves it
                 * clear and skips the instruction, so a user program cannot
                 * idle the CPU. User mode sets it (sysreg_init) and the
                 * halt comes to the same thing. */
                if (c->el == 0 && !(c->sctlr[1] & SCTLR_nTWI)) wfx_trap(c, 0);
                else c->halted = true;
            }
            else if (CRm == 0 && (op2 == 4 || op2 == 5)) c->event = true;  /* SEV / SEVL */
            /* NOP/YIELD/DGH/etc -> no-op */
            return;
        }
        if (L == 0 && op0 == 0 && op1 == 3 && CRn == 3 && Rt == 31) {     /* barriers/CLREX */
            if (op2 == 2) c->excl_valid = false;              /* CLREX */
            else if ((op2 == 4 || op2 == 5) && c->eng.smp)    /* DSB / DMB */
                /* Map the guest barrier to a host fence where guest threads
                 * run concurrently. Essential on weakly ordered hosts (ARM):
                 * without it the host reorders our plain memcpy-based guest
                 * loads/stores across threads, breaking the guest memory
                 * model (e.g. mutex owner-field corruption). On one in-order
                 * CPU it orders nothing more. */
                __atomic_thread_fence(__ATOMIC_SEQ_CST);
            /* ISB (op2==6)/SB: no ordering effect on data in this interpreter */
            return;
        }
        /* MSR(imm)/SYS/SYSL/MRS/MSR(reg) -> sysreg.c (M2) */
        sysreg_exec(c, insn);
        return;
    }
    undefined(c, insn);
}

/* ================= top-level dispatch ================= */
void exec_a64(CPU *c, u32 insn) {
    switch ((insn >> 25) & 0xf) {
        case 0x8: case 0x9: dp_immediate(c, insn); break;     /* 100x */
        case 0xa: case 0xb: branch_system(c, insn); break;    /* 101x */
        case 0x4: case 0x6: case 0xc: case 0xe: loads_stores(c, insn); break; /* x1x0 */
        case 0x5: case 0xd: dp_register(c, insn); break;      /* x101 */
        case 0x7: case 0xf:                                   /* x111 FP/SIMD */
            if (fp_access_ok(c)) exec_fpsimd(c, insn);
            break;
        default: undefined(c, insn); break;                   /* 00xx reserved */
    }
}
