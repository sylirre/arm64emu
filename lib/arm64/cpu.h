/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* AArch64 CPU state and core interpreter interface, for both of arm64emu's
 * modes. A field only one of them uses costs the other nothing it would
 * notice: most sit in padding. */
#ifndef A64_CPU_H
#define A64_CPU_H

#include "types.h"

struct Machine;
struct CpuOps;

/* PSTATE condition flag bit positions (matching SPSR_ELx / NZCV layout). */
#define PS_N (1u << 31)
#define PS_Z (1u << 30)
#define PS_C (1u << 29)
#define PS_V (1u << 28)

/* DAIF mask bits (as in PSTATE/SPSR bits [9:6]). */
#define PS_D (1u << 9)
#define PS_A (1u << 8)
#define PS_I (1u << 7)
#define PS_F (1u << 6)

/* Exception entry kinds (offsets within a vector group are computed from these). */
typedef enum {
    EXC_SYNC = 0,
    EXC_IRQ  = 1,
    EXC_FIQ  = 2,
    EXC_SERROR = 3,
} ExcKind;

/* Guest memory access kinds (mem.h). */
typedef enum { ACC_READ, ACC_WRITE, ACC_EXEC } AccType;

/* CPU.host_fzmode before the host FPCR has been synced: no FZ/FZ16 value. */
#define HOST_FZMODE_UNKNOWN 0xffffffffu

/* Result of executing a single instruction. */
typedef enum {
    STEP_OK = 0,
    STEP_HALT,        /* fatal: unimplemented / stop the machine */
} StepResult;

/* ---- the CPU's translation caches ----------------------------------------
 * What every guest access and fetch goes through before the slow paths of
 * guest memory (mem.h, which also states the probe ABI); part of the CPU
 * (c->eng), so reaching them costs no thread-local access. The program sets
 * them up for each CPU it runs and keeps the D-TLB's table itself.
 *
 * DTlbEnt: tag = VA page | the CPU's tagctx; pte = host page | DTLB_R/W/X. */
typedef struct { u64 tag; u64 pte; } DTlbEnt;

/* The host page of the code page last fetched from. Only the page
 * *translation* is cached, never decoded bytes: the instruction word is read
 * live, so self-modifying code in the page is seen at once. Whatever changes
 * a translation empties it: tlb_flush_all, which every SCTLR_EL1 write runs,
 * and cpu_reset. */
typedef struct {
    u64  page;      /* VA page base of the cached translation */
    u64  pa_page;   /* its physical page, where the mode has one (JIT block
                     * identity in a full system) */
    u8  *host;      /* host pointer to the page; NULL = invalid */
} FetchCache;

typedef struct CpuEngine {
    DTlbEnt *dtlb;          /* the D-TLB: dtlb_mask + 1 entries */
    u64 dtlb_mask;
    /* The low bits current tags carry: genbits | SCTLR_EL1.M << 1 | EL0, the
     * EL0 bit the effective privilege (an LDTR/STTR at EL1 is EL0's). Kept,
     * not computed per access: cpu_refresh_dtlb_ctx at every change of what
     * it depends on. */
    u64 tagctx;
    u64 genbits;            /* the program's flush generation, as tags carry it
                             * (bits 11:2); 0 where it keeps none */
    /* Where entries can go stale behind this CPU's back -- another thread
     * changing the mappings they were filled from -- the program names the
     * generation counter it bumps for that, and `gen` is the one the entries
     * reflect: a hit is taken only while the two agree, and the slow path
     * empties the table when they do not. NULL where nothing but this CPU
     * invalidates its own entries. */
    const unsigned long *gen_src;
    unsigned long gen;
    /* Moves wherever what a VA holds as code can change without anyone
     * telling the JIT: a full system's TLB maintenance (the program's
     * tlb_flush_all and tlb_flush_page). What caches code by VA beyond the
     * D-TLB and the fetch caches -- the JIT's jump cache -- empties itself
     * when it moves. User mode tells the JIT of every change instead, and
     * leaves it be. */
    u32 code_epoch;
    /* One fetch cache per privilege, so the EL0<->EL1 ping-pong of every
     * syscall/IRQ/ERET keeps both code pages cached: fetch[0] is the current
     * EL's, the one every fetch probes, and fetch[1] the other's.
     * cpu_refresh_el_state swaps them when the EL crosses between EL0 and the
     * rest; fetch_el0 says whose fetch[0] is. */
    FetchCache fetch[2];
    u8 fetch_el0;
    /* The mode the program runs this CPU in, read by the library wherever
     * the two differ (an architectural state decides the rest: the EL, the
     * reset values sysreg_init gives the EL1 configuration).
     * sys: a full system -- the guest runs at EL1 as well as EL0, behind the
     *      stage-1 MMU, and owns the EL1 configuration. 0 is user mode: the
     *      guest only ever runs at EL0, under the configuration Linux gives a
     *      process, which nothing it can execute changes; the program plays
     *      its kernel.
     * smp: guest threads run guest code at the same time on host threads,
     *      over shared memory: LSE atomics, exclusives and LDAR/STLR must be
     *      host atomic accesses, and DMB/DSB host fences. 0 is one CPU
     *      executed by one host thread in program order, where a plain
     *      read-modify-write is atomic and a barrier orders nothing more. */
    u8 sys, smp;
    /* The rest of the mode: what the library asks of the program that runs
     * this CPU (CpuOps, below), set up with the two above. */
    const struct CpuOps *ops;
    /* The engines' own state, allocated by their first run: the predecoded
     * tier's decode cache (pdrun.h) and the JIT's code cache and tables
     * (jit.h). They belong to the host thread running the CPU, like
     * everything here: a CPU copied to start another thread must not take
     * them along. */
    struct PDEnt *pd;
    struct JitEnv *jit;
} CpuEngine;

typedef struct CPU {
    /* General purpose. x[31] is reserved; use reg helpers for XZR/SP semantics. */
    u64 x[31];
    u64 pc;

    /* PSTATE */
    u32 nzcv;         /* uses PS_N/Z/C/V bit positions */
    u32 daif;         /* uses PS_D/A/I/F bit positions */
    u8  el;           /* current exception level 0..3 (we implement 0,1; minimal 2) */
    u8  sp_sel;       /* SPSel: 0 => SP_EL0, 1 => SP_ELx */
    u8  sp_chk;       /* SCTLR_EL1.SA/SA0 checks SP alignment at this EL
                         (cpu_refresh_el_state) */
    /* The FZ/FZ16 bits the host FPCR currently mirrors (exec_fpsimd.c,
     * fpcr_host_sync; AArch64 hosts only), HOST_FZMODE_UNKNOWN where the host
     * thread's FPCR has not been synced since this CPU started or was reset.
     * Host-thread state, kept here because a CPU runs on one host thread. */
    u32 host_fzmode;

    /* Banked stack pointers SP_EL0..SP_EL3 */
    u64 sp_el[4];

    /* SIMD/FP */
    V128 v[32];
    u32 fpcr, fpsr;
    bool fp_trapped;  /* CPACR_EL1.FPEN traps FP/SIMD at this EL (cpu_refresh_el_state) */
    u8 ldst_unpriv;   /* transient: current data access is LDTR/STTR (EL0 view);
                         read by a full system's MMU */
    /* The FP core's lazy state (exec_fpsimd.c): FPCR.FZ/FZ16 latched for the
     * instruction in flight, and the FPSR flags raised in software that
     * fpsr_sync has yet to fold into fpsr. */
    u8  fz, fz16;
    u32 fpexc;

    /* Banked system registers (index by EL where meaningful) */
    u64 sctlr[4];
    u64 ttbr0[4];
    u64 ttbr1[4];
    u64 tcr[4];
    u64 mair[4];
    u64 amair[4];
    u64 vbar[4];
    u64 esr[4];
    u64 far[4];
    u64 elr[4];
    u64 spsr[4];
    u64 tpidr[4];     /* TPIDR_ELx */
    u64 tpidrro_el0;
    u64 contextidr_el1;
    u64 cpacr_el1;
    u64 mdscr_el1;
    u64 par_el1;

    /* Identification / affinity */
    u64 mpidr;

    /* Generic timer */
    u64 cntfrq;
    u64 cntp_ctl, cntp_cval, cntp_tval_base;
    u64 cntv_ctl, cntv_cval;
    u64 cntkctl_el1;
    s64 cntvoff;
    u64 cntpct_base;   /* ARM64EMU_RTCLOCK mode: host ns at which the counter == 0 */
    u64 timer_skip;    /* deterministic mode: virtual ticks fast-forwarded over WFI idle */

    /* Exclusive monitor. On one CPU it matches on the address; where guest
     * threads run concurrently (eng.smp) excl_val/excl_val2 hold the values
     * LDXR/LDXP loaded and STXR/STXP do a real host compare-and-swap
     * (SMP-correct on weakly ordered hosts), not just an address match. excl_valid is u32, not bool, so the JITs' 32-bit
     * CPU-struct accesses to it are well-formed. */
    u32  excl_valid;
    u64  excl_addr;
    u64  excl_size;
    u64  excl_val, excl_val2;

    /* Interrupt input lines (driven by the GIC). */
    bool irq_line;
    bool fiq_line;

    /* Run control */
    bool halted;       /* WFI/WFE: waiting for an event */
    bool stop;         /* machine should terminate */
    bool reset;        /* PSCI SYSTEM_RESET: warm-reboot the machine, don't exit */
    bool event;        /* WFE event register: set by SEV/SEVL, consumed by WFE */
    /* Attention: the program's run loop has something to act on before more
     * guest code runs. The engines poll this one byte where they would
     * otherwise test every reason for it (cpu_attn below has the contract). */
    u8   attn;
    u64  icount;       /* retired instruction count */
    u64  cur_insn_pc;  /* address of the instruction currently executing */

    struct Machine *m;
    /* Its mode and translation caches (above), kept by cpu_reset. Part of
     * the CPU rather than thread-local storage, which where it is emulated
     * (Bionic) costs a call per access; set up by the program for each CPU
     * it runs, before the CPU's first reset. */
    CpuEngine eng;
} CPU;

/* ---- Register access helpers (XZR vs SP semantics) ---- */

/* Current selected stack pointer. */
static inline u64 *cpu_cur_sp(CPU *c) {
    return c->sp_sel ? &c->sp_el[c->el] : &c->sp_el[0];
}

/* Read GPR with reg==31 meaning XZR (zero). */
static inline u64 reg_x(CPU *c, unsigned n) {
    return (n == 31) ? 0 : c->x[n];
}
/* Write GPR with reg==31 meaning discard. */
static inline void set_x(CPU *c, unsigned n, u64 v) {
    if (n != 31) c->x[n] = v;
}
/* Read GPR with reg==31 meaning SP. */
static inline u64 reg_xsp(CPU *c, unsigned n) {
    return (n == 31) ? *cpu_cur_sp(c) : c->x[n];
}
static inline void set_xsp(CPU *c, unsigned n, u64 v) {
    if (n == 31) *cpu_cur_sp(c) = v; else c->x[n] = v;
}

/* 32/64-bit width helpers: result truncation when sf==0. */
static inline u64 reg_x_sz(CPU *c, unsigned n, bool is64) {
    u64 v = reg_x(c, n);
    return is64 ? v : (u32)v;
}
static inline void set_x_sz(CPU *c, unsigned n, bool is64, u64 v) {
    set_x(c, n, is64 ? v : (u32)v);
}

/* Build a PSTATE/SPSR word from current state. */
u32 cpu_pack_spsr(CPU *c);
void cpu_unpack_spsr(CPU *c, u32 spsr);

/* Recompute the D-TLB tag context (CpuEngine.tagctx) from the flush
 * generation, SCTLR_EL1.M, the EL and an LDTR/STTR in flight: called by
 * cpu_refresh_el_state, by tlb_flush_all (the generation; every SCTLR_EL1
 * write runs it) and around an unprivileged access (decode.c). */
static inline void cpu_refresh_dtlb_ctx(CPU *c) {
    c->eng.tagctx = c->eng.genbits | ((c->sctlr[1] & 1) ? 2u : 0u)
                  | ((c->el == 0 || c->ldst_unpriv) ? 1u : 0u);
}

/* Recompute what is cached of the current EL's configuration: the CPACR_EL1
 * FPEN trap predicate, which depends only on (cpacr, el), whether SCTLR_EL1
 * checks SP alignment there (SA0 at EL0, SA above), the D-TLB tag context,
 * and which of the two fetch caches is the current EL's. Refreshed at every
 * point the EL, CPACR or SCTLR can change: their MSRs, exception entry, ERET
 * (inside cpu_unpack_spsr), and reset -- in user mode only the last matters:
 * the guest never leaves EL0, where neither register is in reach, so they
 * stay as sysreg_init leaves them. FPEN: 0b11 no trap; 0b01 traps EL0 only;
 * 0b00/0b10 trap EL0 and EL1. EL2+ is outside CPACR's reach (CPTR_EL2/3 are
 * not modeled). */
static inline void cpu_refresh_el_state(CPU *c) {
    unsigned fpen = (unsigned)(c->cpacr_el1 >> 20) & 3;
    c->fp_trapped = (c->el == 0) ? (fpen != 3)
                  : (c->el == 1) ? (fpen == 0 || fpen == 2) : false;
    c->sp_chk = (u8)((c->sctlr[1] >> (c->el == 0 ? 4 : 3)) & 1);
    cpu_refresh_dtlb_ctx(c);
    u8 el0 = c->el == 0;
    if (el0 != c->eng.fetch_el0) {           /* the current EL's fetch cache */
        FetchCache t = c->eng.fetch[0];
        c->eng.fetch[0] = c->eng.fetch[1];
        c->eng.fetch[1] = t;
        c->eng.fetch_el0 = el0;
    }
}

/* Whether this CPU may ever have to check SP alignment (SCTLR_EL1.SA/SA0):
 * in a full system the guest kernel can turn either on at any time; in user
 * mode SA0 is what sysreg_init gave it, for good. The engines that decide
 * ahead of execution (the decode cache, the JIT) ask this whether an SP-based
 * access needs the check at all. */
static inline bool cpu_sp_chk_possible(const CPU *c) {
    return c->eng.sys || ((c->sctlr[1] >> 4) & 1);
}

/* Raise the EC 0x07 FP/SIMD-access trap (A64 ISS: CV=1, COND=0b1110).
 * sysreg.c. */
void cpu_fp_trap(CPU *c);

/* ---- Run control: the engines' contract with the program's run loop -----
 *
 * An engine (cpu_step, pd_run, jit_run) runs guest code until the CPU asks
 * for attention or, for the two that run more than one instruction, its
 * icount reaches the deadline the loop gave it. Whatever needs the loop
 * before more guest code runs raises attention: a stop or a halt (cpu_stop,
 * cpu_halt), an interrupt that became deliverable (cpu_irq_poll), and
 * whatever the program has of its own -- in user mode an exception it records
 * rather than vectors, or a signal it caught. The engines read the one byte
 * at each instruction boundary they check; the loop clears it before it
 * looks at the reasons, so one raised while it does is seen next time rather
 * than lost.
 *
 * Raised from wherever the reason arises: the CPU's own thread, a signal
 * handler on that thread, or another thread. Hence the relaxed atomics: one
 * plain byte access each, which the compiler may neither cache in a register
 * across an engine's loop nor drop. */
static inline void cpu_attn(CPU *c) {
    __atomic_store_n(&c->attn, 1, __ATOMIC_RELAXED);
}
static inline bool cpu_attn_pending(const CPU *c) {
    return __atomic_load_n(&c->attn, __ATOMIC_RELAXED) != 0;
}
static inline void cpu_attn_clear(CPU *c) {
    __atomic_store_n(&c->attn, 0, __ATOMIC_RELAXED);
}
static inline void cpu_stop(CPU *c) { c->stop = true; cpu_attn(c); }
static inline void cpu_halt(CPU *c) { c->halted = true; cpu_attn(c); }

/* An interrupt the CPU would take now: its line raised and PSTATE not masking
 * it. cpu_irq_poll asks for attention when there is one, and runs wherever
 * either side changes -- the interrupt controller driving a line, and an
 * instruction unmasking DAIF (MSR DAIF/DAIFClr, ERET). In user mode nothing
 * drives the lines, and it never fires. */
static inline bool cpu_irq_pending(const CPU *c) {
    return (c->irq_line && !(c->daif & PS_I)) ||
           (c->fiq_line && !(c->daif & PS_F));
}
static inline void cpu_irq_poll(CPU *c) {
    if (cpu_irq_pending(c)) cpu_attn(c);
}

/* ---- Core API ---- */
void cpu_reset(CPU *c, u64 entry, unsigned reset_el);
/* Fetch, decode and execute one instruction: nothing while the CPU is
 * stopped (STEP_HALT) or halted. Interrupts are the run loop's to deliver
 * between steps, cpu_take_interrupt. */
StepResult cpu_step(CPU *c);
/* Take the pending interrupt, FIQ before IRQ, if there is one to take now;
 * returns whether it did. */
bool cpu_take_interrupt(CPU *c);

/* ---- The program's side of the mode ----
 * What the library cannot do for itself it asks the program that runs the
 * CPU, through the table the program sets up each CPU with (c->eng.ops,
 * beside sys and smp): exception entry, the guest memory it models behind
 * the CPU's caches (mem.h), its timer and firmware, and the JIT's
 * coherence with that memory (jit.h). The library names no program
 * function, so one build of it serves a program that runs CPUs of either
 * mode. Entries marked optional may be NULL. */
struct JitCoherence;
typedef struct CpuOps {
    /* Exception entry: a full system vectors to EL1 (ret_addr becomes
     * ELR_EL1); user mode records the exception for its run loop, which
     * plays the kernel's part. The library calls it exception_take. */
    void (*exception_take)(CPU *c, ExcKind kind, u64 esr, u64 far, u64 ret_addr);
    /* Guest memory behind the caches, and their maintenance: mem.h's slow
     * paths, tlb_flush_all and tlb_flush_page, under those names. at_s1 is
     * AT S1E1R/W, S1E0R/W: the PAR_EL1 value for va, never a fault. */
    bool (*read_slow)(CPU *c, u64 va, unsigned size, u64 *out);
    bool (*write_slow)(CPU *c, u64 va, unsigned size, u64 val);
    bool (*read128_slow)(CPU *c, u64 va, V128 *out);
    bool (*write128_slow)(CPU *c, u64 va, const V128 *val);
    bool (*ifetch_slow)(CPU *c, u64 va, u32 *insn_out);
    void *(*host_ptr_slow)(CPU *c, u64 va, unsigned size, AccType acc);
    void (*tlb_flush_all)(CPU *c);
    void (*tlb_flush_page)(CPU *c, u64 va);
    u64  (*at_s1)(CPU *c, u64 va, bool is_write, bool as_el0);
    /* The generic timer's count (CNTPCT, or CNTVCT with virt), and
     * (optional) the timer device's re-evaluation after a CNT* register
     * write, so its interrupt line tracks a reprogrammed compare value. */
    u64  (*gt_count)(CPU *c, bool virt);
    void (*timer_update)(CPU *c);
    /* (optional) An HVC or SMC at EL1 for the firmware the program plays
     * (PSCI): true if it was served, else the instruction is UNDEFINED. */
    bool (*smccc)(CPU *c, bool is_hvc);
    /* The JIT's coherence with the memory modelled (jit.h). */
    const struct JitCoherence *jit;
} CpuOps;

/* The library's calls through the CPU's ops that code inlined at many sites
 * makes are out of line (cpu.c), so those sites stay plain calls:
 * exception_take here, mem.h's slow paths and TLB maintenance (mem.h says
 * why). */
void exception_take(CPU *c, ExcKind kind, u64 esr, u64 far, u64 ret_addr);

/* Raise a synchronous exception (ELR = address of the faulting instruction). */
void cpu_raise_sync(CPU *c, u64 esr, u64 far);

/* Debug dump. */
void cpu_dump(CPU *c);

/* Decoder entry: execute one already-fetched instruction word. Defined in decode.c. */
void exec_a64(CPU *c, u32 insn);

/* Condition code evaluation (cond 0..15) using current NZCV. */
bool cond_holds(CPU *c, unsigned cond);

/* The step driver's diagnostics (cpu.c), set by the program from its command
 * line and environment; all off by default. Every per-instruction one runs
 * only in cpu_step, so the program also sets g_debug_hooks when any is on and
 * keeps the batching engines out of the way. */
extern int g_trace;        /* per-instruction trace */
extern int g_singlestep;   /* unused placeholder for parity with QEMU */
extern int g_dbg;          /* targeted device/IRQ debug */
extern int g_rtrace;       /* compact register trace for differential debugging */
extern int g_prof;         /* hot-PC profiler */
void prof_dump(void);      /* print the hottest PCs (no-op unless g_prof) */
extern u64 g_tpc;          /* dump CPU state when pc == this */
extern int g_ring;         /* ring-buffer recent steps */
void ring_dump(void);      /* print the recent-instruction ring buffer */
extern u64 g_watch;        /* log writes to [g_watch, g_watch+8) */
extern int g_debug_hooks;  /* OR of all per-step debug facilities (perf guard) */
extern int g_iabort_log;   /* log instruction aborts */
extern int g_rtclock;      /* drive the generic timer + RTC from the host clock
                              (default); 0 selects the deterministic
                              instruction-count clock (debug) */
extern u64 g_vawatch;      /* log writes whose range covers this VA */
void cov_load(const char *path);  /* load QEMU coverage PC set (divergence finder) */
/* The program's own per-instruction diagnostics, when it sets them:
 * cpu_step_hook runs in cpu_step for every instruction, before it executes
 * (with g_debug_hooks set), cpu_svc_hook before every SVC. */
extern void (*cpu_step_hook)(CPU *c, u32 insn);
extern void (*cpu_svc_hook)(CPU *c);

/* The library's environment knobs (the predecoded tier's PD_MAX, the JIT's
 * JIT_*) are named <prefix><name>, after the program running it: it sets
 * g_env_prefix ("ARM64EMU_", ...) before it starts a CPU. arm64_getenv is
 * getenv of such a name, NULL while no prefix is set. */
extern const char *g_env_prefix;
const char *arm64_getenv(const char *name);

#endif /* A64_CPU_H */
