/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* Virtual memory translation: the stage-1 MMU (EL1&0 regime, 4 KB granule)
 * behind the shared core's guest memory interface (lib/arm64/mem.h), whose
 * slow paths and cache maintenance mmu.c defines for this machine. */
#ifndef A64_MMU_H
#define A64_MMU_H

#include "cpu.h"
#include "mem.h"

/* Translate VA->PA for the current EL/state. On fault, raises the appropriate
 * synchronous exception (data/instruction abort) and returns false. */
bool mmu_translate(CPU *c, u64 va, AccType acc, u64 *pa_out);

/* Non-faulting VA->PA probe (no TLB fill, no abort, permissions ignored).
 * Used by the JIT's IC IVAU handling to find the physical page whose
 * translations to drop. */
bool mmu_probe_pa(CPU *c, u64 va, u64 *pa_out);

/* The CPU's translation caches (cpu.h's CpuEngine), which mmu_cpu_init sets
 * up for it -- machine_init calls it once; cpu_reset keeps them.
 *
 * The D-TLB holds RAM pages only: flash (CFI command state) and MMIO (side
 * effects) are never inserted, so they always take the bus path. Its tags
 * carry the low 10 bits of the TLB flush generation (g_tlb_gen), which makes
 * tlb_flush_all O(1): bumping the generation invalidates every entry without
 * touching the array. The slice aliases every 1024 flushes, and tlb_flush_all
 * empties the table then. The fetch cache is cleared by tlb_flush_all and
 * tlb_flush_page -- the invalidation contract the software TLB relies on. */
#define DTLB_BITS 12
#define DTLB_SIZE (1u << DTLB_BITS)
void mmu_cpu_init(CPU *c);
extern u32 g_tlb_gen;

/* Single-page invalidation for the VA-form TLBIs (VAE1/VAAE1/VALE1/VAALE1),
 * mem.h's tlb_flush_page: clears the page's TLB/D-TLB slots and the fetch
 * cache without bumping the flush generation, so every other cached
 * translation survives. Escalates to tlb_flush_all when va falls inside the
 * recorded large-page (block-mapped) range — the TLB fragments 2 MB/1 GB
 * blocks into 4 KB entries, and one block-VA TLBI must not leave sibling
 * fragments alive.
 *
 * Bumped by every tlb_flush_page: the JIT dispatcher purges its VA-keyed
 * indirect-branch jcache when this moves (its blocks/D-TLB need nothing —
 * blocks are PA-keyed and re-verified at dispatch, D-TLB slots are cleared
 * there). */
extern u32 g_tlbi_va_seq;

/* Pages holding JIT-translated code: the D-TLB refuses the W bit for them so
 * guest stores fall to the slow path, where the JIT invalidates the page's
 * blocks (self-modifying-code coherence). NULL until a JIT allocates it. */
extern const u8 *g_jit_code_bitmap;
void jit_invalidate_phys_range(u64 pa, u64 len);   /* defined in jit/jit.c */

#endif /* A64_MMU_H */
