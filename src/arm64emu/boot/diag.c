/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* Guest-kernel diagnostics of the full system, on the step driver's hooks
 * (cpu.h, cpu_step_hook/cpu_svc_hook): the mm-syscall tracer (ARM64EMU_SYS)
 * and the musl heap tracker (ARM64EMU_HEAP). Both look at EL0 through the
 * guest kernel's eyes, so only the interpreter runs them (g_debug_hooks). */
#include "cpu.h"
#include "diag.h"
#include "mmu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int g_systrace = 0;  /* env ARM64EMU_SYS: trace mm syscalls (mmap/brk/munmap/...) (debug) */

/* One-shot record of an in-flight mm syscall, logged with its result when EL0
 * resumes. Lets us see musl's heap/mmap layout around the /init corruption. */
static struct { int active; u64 num, a0, a1, a2, a3, ic; } g_sysp;
static const char *sysname(u64 n) {
    switch (n) {
        case 214: return "brk";    case 215: return "munmap"; case 216: return "mremap";
        case 220: return "clone";  case 221: return "execve"; case 222: return "mmap";
        case 226: return "mprotect"; default: return "?";
    }
}
/* Dump a NUL-terminated user string via non-faulting peek (for execve tracing). */
static void dump_ustr(CPU *c, u64 va) {
    if (!va) { fprintf(stderr, "(null)"); return; }
    u64 b; if (!mem_peek(c, va, 1, &b)) { fprintf(stderr, "<UNMAPPED 0x%llx>", (unsigned long long)va); return; }
    fprintf(stderr, "\"");
    for (int i = 0; i < 80; i++) {
        if (!mem_peek(c, va + i, 1, &b)) { fprintf(stderr, "<FAULT@+%d>", i); break; }
        if (!b) break;
        fputc((b >= 32 && b < 127) ? (int)b : '.', stderr);
    }
    fprintf(stderr, "\"");
}

/* Dump a NULL-terminated user pointer array (argv/envp), flagging bad pointers. */
static void dump_uptrarr(CPU *c, const char *tag, u64 arr) {
    fprintf(stderr, "  %s@0x%llx:\n", tag, (unsigned long long)arr);
    for (int i = 0; i < 40; i++) {
        u64 p;
        if (!mem_peek(c, arr + (u64)i * 8, 8, &p)) { fprintf(stderr, "   [%d] <array UNMAPPED>\n", i); break; }
        if (!p) break;
        u64 b; bool mapped = mem_peek(c, p, 1, &b);
        fprintf(stderr, "   [%d] 0x%llx %s ", i, (unsigned long long)p, mapped ? "->" : "-> *** UNMAPPED ***");
        if (mapped) dump_ustr(c, p);
        fprintf(stderr, "\n");
    }
}

static void systrace_svc(CPU *c) {
    if (c->el != 0) return;
    u64 n = c->x[8];
    if (n != 214 && n != 215 && n != 216 && n != 220 && n != 221 && n != 222 && n != 226)
        return;
    g_sysp.active = 1; g_sysp.num = n;
    g_sysp.a0 = c->x[0]; g_sysp.a1 = c->x[1]; g_sysp.a2 = c->x[2]; g_sysp.a3 = c->x[3];
    g_sysp.ic = c->icount;
    if (n == 221) {                          /* execve: dump path/argv/envp now */
        fprintf(stderr, "[execve] ic=%llu ttbr0=0x%llx path=",
                (unsigned long long)c->icount, (unsigned long long)c->ttbr0[1]);
        dump_ustr(c, c->x[0]); fprintf(stderr, "\n");
        dump_uptrarr(c, "argv", c->x[1]);
        dump_uptrarr(c, "envp", c->x[2]);
    }
}

/* ---- musl mallocng double-allocation detector (env ARM64EMU_HEAP=1) ----------------
 * The line-438 execve EFAULT is heap corruption: in the /init shell, ash's
 * string-stack output block aliases the live `environ` array (both base
 * 0xffff9c9ca180), so ash's lexer writes "in\0" over environ[0]. This tracker
 * pinpoints the allocation that hands out the live environ block.
 *
 * musl's public malloc/free/realloc are 4-byte position-independent branch
 * wrappers; ALL user allocations funnel through them (calloc/realloc call
 * malloc@plt). We match them by (pc & 0xfff) + the exact instruction word, so no
 * load base is needed, and we compute each call's musl base = pc - file_offset.
 * Allocations are tracked PER PROCESS, keyed by TTBR0 table base (so re-exec'd
 * children and the unrelated first EL0 process do not confuse one another).
 * We fire when an allocation returns either (A) the address __environ currently
 * points at (the live environ array handed back out), or (B) a base already live
 * in the same process (a plain double-alloc). __environ is at musl file offset
 * 0xc2008 (from the .so dynsym).                                                */
int g_heaptrack = 0;
#define HT_MALLOC_OFF   0x834
#define HT_MALLOC_INS   0x14000584u
#define HT_FREE_OFF     0x73c
#define HT_FREE_INS     0x14000271u
#define HT_REALLOC_OFF  0x9a4
#define HT_REALLOC_INS  0x17fffef4u
#define HT_MALLOC_FOFF  0x28834
#define HT_FREE_FOFF    0x2873c
#define HT_REALLOC_FOFF 0x2a9a4
#define HT_ENVIRON_FOFF 0xc2008

#define HT_BUCKETS (1u << 20)
#define HT_NODES   (1 << 21)     /* signed: compared against int ht_node_top */
static int *ht_bucket;                 /* head node index, or -1 */
static struct htnode { u64 ttbr, base, size, site; int next; } *ht_node;
static int ht_free_head = -1, ht_node_top = 0;
static long ht_live = 0;
/* one outstanding top-level alloc, timed for its return value */
static struct { int active, is_realloc; u64 old, size, lr, sp, callpc, ic, ttbr, musl; } ht_call;
static int ht_fires = 0;
static long ht_nmalloc = 0, ht_nfree = 0, ht_nrealloc = 0;
u64 g_heap_at = 0;          /* env ARM64EMU_HEAP_AT: log every alloc/free touching this VA */
/* TTBR0 carries the ASID in the high bits; key on the table base only so an
 * ASID change for the same process does not split it into two identities. */
static inline u64 ht_pgbase(u64 t) { return t & 0x0000FFFFFFFFF000ULL; }
void heaptrack_report(void) {
    fprintf(stderr, "[HEAP] report: malloc=%ld free=%ld realloc=%ld live=%ld nodes=%d fires=%d\n",
            ht_nmalloc, ht_nfree, ht_nrealloc, ht_live, ht_node_top, ht_fires);
}

static inline unsigned ht_hash(u64 ttbr, u64 base) {
    return (unsigned)((((base >> 4) ^ (ttbr >> 12)) * 0x9E3779B97F4A7C15ULL) >> 44) & (HT_BUCKETS - 1);
}
static int ht_find(u64 ttbr, u64 base, u64 *size, u64 *site) {
    for (int n = ht_bucket[ht_hash(ttbr, base)]; n >= 0; n = ht_node[n].next)
        if (ht_node[n].base == base && ht_node[n].ttbr == ttbr) {
            if (size) *size = ht_node[n].size;
            if (site) *site = ht_node[n].site;
            return 1;
        }
    return 0;
}
static void ht_insert(u64 ttbr, u64 base, u64 size, u64 site) {
    int n = ht_free_head;
    if (n >= 0) ht_free_head = ht_node[n].next;
    else if (ht_node_top < HT_NODES) n = ht_node_top++;
    else { fprintf(stderr, "[HEAP] node pool exhausted (live=%ld)\n", ht_live); g_heaptrack = 0; return; }
    ht_node[n].ttbr = ttbr; ht_node[n].base = base; ht_node[n].size = size; ht_node[n].site = site;
    unsigned b = ht_hash(ttbr, base);
    ht_node[n].next = ht_bucket[b]; ht_bucket[b] = n; ht_live++;
}
static void ht_remove(u64 ttbr, u64 base) {
    unsigned b = ht_hash(ttbr, base);
    int prev = -1;
    for (int n = ht_bucket[b]; n >= 0; prev = n, n = ht_node[n].next)
        if (ht_node[n].base == base && ht_node[n].ttbr == ttbr) {
            if (prev < 0) ht_bucket[b] = ht_node[n].next; else ht_node[prev].next = ht_node[n].next;
            ht_node[n].base = 0;
            ht_node[n].next = ht_free_head; ht_free_head = n; ht_live--;
            return;
        }
}
/* Which live block of the current process (if any) contains `va`? (write-watch) */
void heaptrack_query(CPU *c, u64 va) {
    if (!g_heaptrack) return;
    u64 ttbr = ht_pgbase(c->ttbr0[1]);
    for (int n = 0; n < ht_node_top; n++) {
        u64 b = ht_node[n].base;
        if (b && ht_node[n].ttbr == ttbr && va >= b && va < b + ht_node[n].size) {
            fprintf(stderr, "[HEAP] va 0x%llx is INSIDE live block base=0x%llx size=%llu (+0x%llx) site=0x%llx\n",
                    (unsigned long long)va, (unsigned long long)b, (unsigned long long)ht_node[n].size,
                    (unsigned long long)(va - b), (unsigned long long)ht_node[n].site);
            return;
        }
    }
    fprintf(stderr, "[HEAP] va 0x%llx is NOT inside any live tracked block of this proc (live=%ld)\n",
            (unsigned long long)va, ht_live);
}
static u64 ht_read_environ(CPU *c, u64 musl) {     /* value of __environ for a process */
    u64 v;
    if (!mem_peek(c, musl + HT_ENVIRON_FOFF, 8, &v)) return 0;
    return v;
}
static void ht_dump_words(CPU *c, u64 va, int n) {
    for (int i = 0; i < n; i++) {
        u64 w;
        if (!mem_peek(c, va + (u64)i * 8, 8, &w)) { fprintf(stderr, "   [+%d] <unmapped>\n", i * 8); break; }
        fprintf(stderr, "   [+%-3d] 0x%016llx\n", i * 8, (unsigned long long)w);
    }
}
static void heaptrack_step(CPU *c, u32 insn) {
    if (c->el != 0) return;
    u64 ttbr = ht_pgbase(c->ttbr0[1]);
    /* capture the return value of a timed top-level alloc */
    if (ht_call.active) {
        if (c->pc == ht_call.lr && ttbr == ht_call.ttbr && *cpu_cur_sp(c) >= ht_call.sp) {
            u64 ret = c->x[0];
            if (ht_call.is_realloc && ret) ht_remove(ht_call.ttbr, ht_call.old);
            if (ret) {
                u64 env = ht_read_environ(c, ht_call.musl);
                u64 vsz = 0, vsite = 0;
                int hit_env = (env && ret == env);
                int hit_live = ht_find(ht_call.ttbr, ret, &vsz, &vsite);
                if ((hit_env || hit_live) && ht_fires < 30) {
                    ht_fires++;
                    fprintf(stderr,
                        "\n[HEAP] *** DOUBLE-ALLOC #%d *** %s returned %s\n"
                        "   ret=0x%llx size=%llu callsite=0x%llx ic=%llu ttbr=0x%llx musl=0x%llx\n"
                        "   __environ=0x%llx (ret==environ? %s)  prior-live-block? %s (size=%llu site=0x%llx)\n",
                        ht_fires, ht_call.is_realloc ? "realloc" : "malloc",
                        hit_env ? "the LIVE __environ array" : "an already-live block",
                        (unsigned long long)ret, (unsigned long long)ht_call.size,
                        (unsigned long long)ht_call.callpc, (unsigned long long)ht_call.ic,
                        (unsigned long long)ht_call.ttbr, (unsigned long long)ht_call.musl,
                        (unsigned long long)env, hit_env ? "YES" : "no",
                        hit_live ? "YES" : "no", (unsigned long long)vsz, (unsigned long long)vsite);
                    ht_dump_words(c, ret, 6);
                    ring_dump();
                }
                if (g_heap_at && ret <= g_heap_at && g_heap_at < ret + (ht_call.size ? ht_call.size : 1))
                    fprintf(stderr, "[AT] %s ret=0x%llx size=%llu (AT +0x%llx) callsite=0x%llx ic=%llu ttbr=0x%llx __environ=0x%llx\n",
                            ht_call.is_realloc ? "realloc" : "malloc", (unsigned long long)ret,
                            (unsigned long long)ht_call.size, (unsigned long long)(g_heap_at - ret),
                            (unsigned long long)ht_call.callpc, (unsigned long long)ht_call.ic,
                            (unsigned long long)ht_call.ttbr, (unsigned long long)env);
                if (hit_live) ht_remove(ht_call.ttbr, ret);
                ht_insert(ht_call.ttbr, ret, ht_call.size, ht_call.callpc);
            }
            ht_call.active = 0;
        }
        return;                                /* ignore hooks nested inside the timed call */
    }
    u32 off = (u32)(c->pc & 0xfff);
    if (insn == HT_MALLOC_INS && off == HT_MALLOC_OFF) {
        ht_nmalloc++;
        ht_call.active = 1; ht_call.is_realloc = 0; ht_call.old = 0; ht_call.size = c->x[0];
        ht_call.lr = c->x[30]; ht_call.sp = *cpu_cur_sp(c); ht_call.callpc = c->x[30];
        ht_call.ic = c->icount; ht_call.ttbr = ttbr; ht_call.musl = c->pc - HT_MALLOC_FOFF;
    } else if (insn == HT_REALLOC_INS && off == HT_REALLOC_OFF) {
        ht_nrealloc++;
        ht_call.active = 1; ht_call.is_realloc = 1; ht_call.old = c->x[0]; ht_call.size = c->x[1];
        ht_call.lr = c->x[30]; ht_call.sp = *cpu_cur_sp(c); ht_call.callpc = c->x[30];
        ht_call.ic = c->icount; ht_call.ttbr = ttbr; ht_call.musl = c->pc - HT_REALLOC_FOFF;
    } else if (insn == HT_FREE_INS && off == HT_FREE_OFF) {
        ht_nfree++;
        if (c->x[0]) {
            u64 env = ht_read_environ(c, c->pc - HT_FREE_FOFF);
            if (c->x[0] == env)
                fprintf(stderr, "[HEAP] free() of the LIVE __environ array 0x%llx ic=%llu callsite=0x%llx ttbr=0x%llx\n",
                        (unsigned long long)c->x[0], (unsigned long long)c->icount,
                        (unsigned long long)c->x[30], (unsigned long long)ttbr);
            if (g_heap_at) {
                u64 fsz = 0;
                int known = ht_find(ttbr, c->x[0], &fsz, 0);
                if (c->x[0] == g_heap_at || (known && c->x[0] <= g_heap_at && g_heap_at < c->x[0] + (fsz ? fsz : 1)))
                    fprintf(stderr, "[AT] free   ptr=0x%llx size=%llu (AT +0x%llx) callsite=0x%llx ic=%llu ttbr=0x%llx __environ=0x%llx\n",
                            (unsigned long long)c->x[0], (unsigned long long)fsz,
                            (unsigned long long)(g_heap_at - c->x[0]), (unsigned long long)c->x[30],
                            (unsigned long long)c->icount, (unsigned long long)ttbr, (unsigned long long)env);
            }
            ht_remove(ttbr, c->x[0]);
        }
    }
}
static void heaptrack_init(void) {
    ht_bucket = malloc(HT_BUCKETS * sizeof(int));
    ht_node = malloc(HT_NODES * sizeof *ht_node);
    for (unsigned i = 0; i < HT_BUCKETS; i++) ht_bucket[i] = -1;
}

/* The step hook: report the mm syscall in flight once EL0 resumes after it,
 * and run the heap tracker. */
static void diag_step(CPU *c, u32 insn) {
    if (g_systrace && g_sysp.active && c->el == 0) {     /* mm syscall returned */
        fprintf(stderr, "[sys] %-7s(0x%llx, 0x%llx, 0x%llx, 0x%llx) = 0x%llx  ic=%llu\n",
                sysname(g_sysp.num), (unsigned long long)g_sysp.a0,
                (unsigned long long)g_sysp.a1, (unsigned long long)g_sysp.a2,
                (unsigned long long)g_sysp.a3, (unsigned long long)c->x[0],
                (unsigned long long)g_sysp.ic);
        g_sysp.active = 0;
    }
    if (g_heaptrack) heaptrack_step(c, insn);
}

bool diag_install(void) {
    if (!g_systrace && !g_heaptrack) return false;
    if (g_heaptrack) heaptrack_init();
    if (g_systrace) cpu_svc_hook = systrace_svc;
    cpu_step_hook = diag_step;
    return true;
}
