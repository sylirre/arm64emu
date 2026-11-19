/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* The JIT's coherence with a full system's memory: the runtime is
 * lib/arm64/jit's; this is what it asks of the program (jit.h's
 * JitCoherence: jit_phys_coherence), and the program's own entry points
 * into it.
 *
 * Translations are keyed by physical page (FetchCache.pa_page), and code
 * changes reach them through stores, not IC IVAU: pages holding
 * translations are marked in g_jit_code_bitmap; the D-TLB refuses the W bit
 * for them (mmu.c), so every store to such a page reaches mem_write's slow
 * path, which calls jit_invalidate_phys_range after committing the store,
 * as device DMA and flash programming do (memory.c). Cross-page SMC (module
 * loads, kernel alternatives, DMA) is exact. IC IVAU drops the (usually
 * already-dropped) target page's blocks precisely -- never a full flush; a
 * boot issues hundreds of thousands of them. A VA that names other memory
 * now misses at dispatch, by the host-page check, and TLB maintenance moves
 * the CPU's code epoch for the jump cache (mmu.c). */
#include <stdlib.h>
#include <string.h>

#include "machine.h"
#include "mmu.h"
#include "jit_priv.h"

/* RAM code-page bitmap (1 bit per 4 KB page). mmu.c reads it through the
 * const alias g_jit_code_bitmap (declared in mmu.h) when filling D-TLB write
 * entries; this file owns the writable pointer. NULL until the JIT starts. */
static u8 *g_code_bitmap;
static u64 g_code_bitmap_bits;

static inline int bitmap_pfn(Machine *m, u64 pa_page, u64 *pfn) {
    if (pa_page < m->ram_base || pa_page - m->ram_base >= m->ram_size)
        return 0;
    *pfn = (pa_page - m->ram_base) >> 12;
    return 1;
}

static void bitmap_set(Machine *m, u64 pa_page) {
    u64 pfn;
    if (g_code_bitmap && bitmap_pfn(m, pa_page, &pfn))
        g_code_bitmap[pfn >> 3] |= (u8)(1u << (pfn & 7));
}

static void bitmap_clear(Machine *m, u64 pa_page) {
    u64 pfn;
    if (g_code_bitmap && bitmap_pfn(m, pa_page, &pfn))
        g_code_bitmap[pfn >> 3] &= (u8)~(1u << (pfn & 7));
}

static int bitmap_test(Machine *m, u64 pa_page) {
    u64 pfn;
    if (g_code_bitmap && bitmap_pfn(m, pa_page, &pfn))
        return (g_code_bitmap[pfn >> 3] >> (pfn & 7)) & 1;
    return 0;
}

static int phys_attach(CPU *c, JitEnv *env) {
    (void)env;
    if (g_code_bitmap) return 0;
    g_code_bitmap_bits = c->m->ram_size >> 12;
    g_code_bitmap = calloc(1, (g_code_bitmap_bits >> 3) + 1);
    if (!g_code_bitmap) return -1;
    g_jit_code_bitmap = g_code_bitmap;       /* mmu.c starts honoring it */
    return 0;
}

/* First translation on a physical page: mark it and strip the W bit from
 * any D-TLB entry already pointing at it, so future stores take the slow
 * path and hit the invalidation hook. (The host page pointer identifies the
 * physical page across all VA aliases.) */
static void phys_translated(CPU *c, const JBlock *b) {
    if (bitmap_test(c->m, b->page)) return;
    bitmap_set(c->m, b->page);
    DTlbEnt *dt = c->eng.dtlb;
    for (u64 i = 0; i <= c->eng.dtlb_mask; i++)
        if ((dt[i].pte & ~0xfffULL) == (u64)(uintptr_t)b->host_page)
            dt[i].pte &= ~(u64)DTLB_W;
}

/* IC IVAU, Xt: with store-tracking the real invalidation already happened at
 * the store (mem_write slow path, DMA and flash hooks), so translations for
 * the line's page are near-always already gone and the code bitmap is clear.
 * Belt-and-braces: resolve Xt without faulting (the insn is a no-op in
 * sysreg.c and never faults -- stay bug-compatible) and drop that one page's
 * surviving blocks. jit_invalidate_phys_range is bitmap-gated and in-helper-
 * safe, so this must never escalate to a full flush. */
static void phys_ic_ivau(CPU *c, u64 va) {
    u64 pa;
    if (mmu_probe_pa(c, va, &pa)) jit_invalidate_phys_range(c->m, pa, 1);
}

/* No resync: nothing raises the flag here -- one CPU, and every change of
 * guest memory reaches the JIT through the hooks above, on the CPU's own
 * thread. No descriptor to keep from a guest either. */
const JitCoherence jit_phys_coherence = {
    .attach     = phys_attach,
    .translated = phys_translated,
    .ic_ivau    = phys_ic_ivau,
};

void jit_invalidate_phys_range(Machine *m, u64 pa, u64 len) {
    if (!jit_on() || !len) return;
    CPU *c = &m->cpu;
    u64 first = pa & ~(GUEST_PAGE_SIZE - 1);
    u64 npages = ((pa + len - 1 - first) >> 12) + 1;
    for (u64 i = 0; i < npages; i++) {
        u64 pg = first + (i << 12);
        /* RAM pages are bitmap-guarded; non-RAM (flash) pages are rare
         * writes -- walk their (near-always empty) page chain directly. */
        u64 pfn;
        if (bitmap_pfn(m, pg, &pfn) && !bitmap_test(m, pg)) continue;
        jit_drop_page(c, pg);
        bitmap_clear(m, pg);
    }
}

/* PSCI warm reboot: drop everything (RAM persists but the OS is going away;
 * the fresh kernel would miss on ctx/host-page anyway -- this reclaims the
 * cache and resets the thrash guard). Runs from the run loop between steps,
 * never mid-block. */
void jit_reset(Machine *m) {
    jit_flush(&m->cpu);
    if (g_code_bitmap)
        memset(g_code_bitmap, 0, (size_t)(g_code_bitmap_bits >> 3) + 1);
}
