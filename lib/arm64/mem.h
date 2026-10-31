/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* Guest memory, as the shared core reaches it.
 *
 * The library's own memory interface: the typed accesses decode.c and the
 * engines make, their hit paths over the CPU's translation caches (inline,
 * here), and the slow paths behind them, which each program
 * defines for the memory it models -- arm64emu a stage-1 MMU over a physical
 * bus, arm64chroot a software page table over the host's address space.
 * Everything a program keeps beyond that stays in its own mmu.h.
 *
 * The caches are part of the CPU (c->eng, cpu.h), never thread-local
 * storage: every guest load, store and instruction fetch starts here, and
 * where thread-local storage is emulated (Bionic) a thread-local access is a
 * library call. */
#ifndef A64_MEM_H
#define A64_MEM_H

#include <string.h>

#include "cpu.h"

#define GUEST_PAGE_SIZE 4096ULL
#define GUEST_PAGE_MASK (GUEST_PAGE_SIZE - 1)

typedef enum { ACC_READ, ACC_WRITE, ACC_EXEC } AccType;

/* ---- the data-side TLB ------------------------------------------------------
 * A direct-mapped cache of VA page -> host page, probed before the slow path;
 * the CPU's (cpu.h: CpuEngine, DTlbEnt). Its entry layout is an ABI shared
 * with the JIT backends, which emit this probe inline: 16 bytes on every
 * host, so an index becomes a byte offset by a shift.
 *
 * tag: VA page | generation (bits 11:2) | SCTLR_EL1.M (bit 1) | EL0 (bit 0),
 *      the low twelve bits the CPU's tagctx. The EL0 bit is the *effective*
 *      access privilege: an LDTR/STTR at EL1 (c->ldst_unpriv, set
 *      transiently by decode.c) tags as EL0, so it misses EL1-filled entries
 *      and shares the EL0-view ones, whose cached permissions are exactly the
 *      EL0 check it needs. The JIT's inline probe takes the low bits from its
 *      dispatcher (dtlb_ctxgen) with ldst_unpriv 0: the fast engines never
 *      execute LDTR/STTR natively (predecode leaves them GENERIC), so
 *      compiled code only probes at its real EL. The generation lets a
 *      program invalidate every entry in O(1) by moving it (genbits); a
 *      program that never does keeps it 0. The VA is taken as given: a
 *      program that ignores a tag byte (TBI) fills entries for the untagged
 *      page, so a tagged access always misses here and its slow path decides.
 * pte: host page | DTLB_R | DTLB_W | DTLB_X. A zeroed entry has no permission
 *      bits, so it can never hit. */
#define DTLB_R 1u
#define DTLB_W 2u
#define DTLB_X 4u
_Static_assert(sizeof(DTlbEnt) == 16, "DTlbEnt layout is a JIT backend ABI");

static inline u64 dtlb_tag(const CPU *c, u64 va) {
    return (va & ~0xfffULL) | c->eng.tagctx;
}
static inline DTlbEnt *dtlb_ent(const CPU *c, u64 va) {
    return &c->eng.dtlb[(va >> 12) & c->eng.dtlb_mask];
}

/* The host address of [va, va+size) if a D-TLB entry maps its page with
 * `need` for the current privilege; NULL otherwise -- a miss, a stale table,
 * an access that crosses a page, or one the entry does not permit. */
static inline u8 *dtlb_hit(CPU *c, u64 va, unsigned size, u64 need) {
    const CpuEngine *e = &c->eng;
    if (e->gen_src && UNLIKELY(__atomic_load_n(e->gen_src, __ATOMIC_ACQUIRE) != e->gen))
        return NULL;
    u64 off = va & 0xfffULL;
    if (UNLIKELY(off + size > 0x1000)) return NULL;
    const DTlbEnt *d = dtlb_ent(c, va);
    if (d->tag != dtlb_tag(c, va) || !(d->pte & need)) return NULL;
    return (u8 *)(uintptr_t)(d->pte & ~0xfffULL) + off;
}

/* ---- the slow paths: the program's ------------------------------------------
 * Each is the whole access -- page-crossing splits, translation, faults,
 * MMIO, refilling the caches -- and returns false after raising the
 * architectural fault. */
bool mem_read_slow(CPU *c, u64 va, unsigned size, u64 *out);
bool mem_write_slow(CPU *c, u64 va, unsigned size, u64 val);
bool mem_read128_slow(CPU *c, u64 va, V128 *out);
bool mem_write128_slow(CPU *c, u64 va, const V128 *val);
bool mem_ifetch_slow(CPU *c, u64 va, u32 *insn_out);
/* The host address of [va, va+size) for `acc`, or NULL. Never raises: NULL
 * means the caller must make the access through mem_read/mem_write, which
 * fault where the architecture does. Where guest threads run concurrently a
 * program resolves the page here rather than answering NULL for a mere
 * miss, so that an atomic access is never demoted to a plain one. */
void *mem_host_ptr_slow(CPU *c, u64 va, unsigned size, AccType acc);
/* Non-faulting read for diagnostics: false if the VA does not translate. */
bool mem_peek(CPU *c, u64 va, unsigned size, u64 *out);

/* Invalidate the CPU's translation caches (TLBI and the writes to the
 * registers translation depends on), or one page of them (VA-form TLBI). */
void tlb_flush_all(CPU *c);
void tlb_flush_page(CPU *c, u64 va);
/* AT S1E1R/W, S1E0R/W: the PAR_EL1 value for va. Never faults. */
u64  mmu_at_s1(CPU *c, u64 va, bool is_write, bool as_el0);

/* ---- typed accesses -----------------------------------------------------------
 * Return false if a fault was raised (the caller abandons the instruction and
 * lets the exception take effect); on success *out holds the data. Sizes are
 * 1 to 8 bytes. Host and guest are both little-endian, so a copy preserves
 * byte order; the power-of-two sizes copy as one load or store, and the
 * others are the pieces of an access split at a page boundary (1..7). */
static inline u64 mem_ld(const u8 *p, unsigned size) {
    switch (size) {
        case 1: return *p;
        case 2: { u16 v; memcpy(&v, p, 2); return v; }
        case 4: { u32 v; memcpy(&v, p, 4); return v; }
        case 8: { u64 v; memcpy(&v, p, 8); return v; }
        default: { u64 v = 0; memcpy(&v, p, size & 7); return v; }
    }
}
static inline void mem_st(u8 *p, unsigned size, u64 v) {
    switch (size) {
        case 1: *p = (u8)v; break;
        case 2: { u16 x = (u16)v; memcpy(p, &x, 2); break; }
        case 4: { u32 x = (u32)v; memcpy(p, &x, 4); break; }
        case 8: memcpy(p, &v, 8); break;
        default: memcpy(p, &v, size & 7); break;
    }
}

static inline bool mem_read(CPU *c, u64 va, unsigned size, u64 *out) {
    const u8 *p = dtlb_hit(c, va, size, DTLB_R);
    if (LIKELY(p != NULL)) { *out = mem_ld(p, size); return true; }
    return mem_read_slow(c, va, size, out);
}
static inline bool mem_write(CPU *c, u64 va, unsigned size, u64 val) {
    u8 *p = dtlb_hit(c, va, size, DTLB_W);
    if (LIKELY(p != NULL)) { mem_st(p, size, val); return true; }
    return mem_write_slow(c, va, size, val);
}
/* SIMD 128-bit accesses: one probe and one 16-byte copy inside a page; the
 * slow path's two halves are observably the same (same bytes, same fault). */
static inline bool mem_read128(CPU *c, u64 va, V128 *out) {
    const u8 *p = dtlb_hit(c, va, 16, DTLB_R);
    if (LIKELY(p != NULL)) { memcpy(out, p, 16); return true; }
    return mem_read128_slow(c, va, out);
}
static inline bool mem_write128(CPU *c, u64 va, const V128 *val) {
    u8 *p = dtlb_hit(c, va, 16, DTLB_W);
    if (LIKELY(p != NULL)) { memcpy(p, val, 16); return true; }
    return mem_write128_slow(c, va, val);
}

/* Host pointer for [va, va+size) if it lies within one page the CPU may
 * access with `acc`; NULL if the caller must go through mem_read/mem_write
 * (see mem_host_ptr_slow). The host-pointer seam of the shared core: the
 * MOPS copies, and host-atomic accesses where guest threads run at once. */
static inline void *mem_host_ptr(CPU *c, u64 va, unsigned size, AccType acc) {
    u64 need = acc == ACC_WRITE ? DTLB_W : acc == ACC_EXEC ? DTLB_X : DTLB_R;
    u8 *p = dtlb_hit(c, va, size, need);
    if (LIKELY(p != NULL)) return p;
    return mem_host_ptr_slow(c, va, size, acc);
}

/* Instruction fetch. The PC's 4-byte alignment is tested here, not only in
 * the slow path: a branch to a misaligned address inside the page already
 * cached would otherwise never reach it, so the PC-alignment exception never
 * happened -- the misparsed word was decoded instead (SIGILL, or a plausible
 * instruction, where a kernel reports SIGBUS/BUS_ADRALN), and a target in the
 * page's last three bytes read past its host backing. It is free: the cached
 * page base has no bits below 12, and the compared key keeps va's low two
 * (mask ~0xffc), so a misaligned VA misses and the slow path raises the
 * fault. */
static inline bool mem_ifetch(CPU *c, u64 va, u32 *insn_out) {
    const FetchCache *f = &c->eng.fetch[0];        /* the current EL's */
    if (LIKELY(f->host && f->page == (va & ~0xffcULL))) {
        u32 v;
        memcpy(&v, f->host + (va & 0xfffULL), 4);
        *insn_out = v;
        return true;
    }
    return mem_ifetch_slow(c, va, insn_out);
}

#endif /* A64_MEM_H */
