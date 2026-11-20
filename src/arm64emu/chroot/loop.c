/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* The interpreter run loop: step the CPU, dispatch pending exceptions
 * (recorded by exception.c) to the syscall layer or to signal delivery. */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "machine.h"
#include "esr.h"
#include "guest_abi.h"
#include "pdrun.h"
#include "jit.h"
#include "ptrace.h"
#include "sysreg.h"
#include "cli.h"

/* Generic-timer count for CNTVCT_EL0/CNTPCT_EL0 reads (the CPU's ops,
 * CpuOps.gt_count): host monotonic clock scaled to the advertised 24 MHz
 * counter frequency. */
u64 host_gt_count(CPU *c, bool virt) {
    (void)c; (void)virt;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    u64 ns = (u64)ts.tv_sec * 1000000000ULL + (u64)ts.tv_nsec;
    return ns * 3 / 125;   /* ns -> 24 MHz ticks */
}

/* EL0's view of the ID registers, as an arm64 kernel presents it. An MRS of
 * the op0 3, op1 0, CRn 0 space is UNDEFINED at EL0, and instead of the
 * SIGILL the kernel answers it itself (cpufeature.c, emulate_sys_reg): CRm 0
 * for MIDR_EL1 (the CPU's), MPIDR_EL1 (0x80000000, whatever the CPU) and
 * REVIDR_EL1 (0); CRm 2..7 for the feature registers, each field the kernel
 * shows userspace passed through and every other one read as its safe value
 * -- a CPU's EL2/EL3, its debug and memory-system details and its AArch32
 * support are nothing a program is told. The table is the kernel's
 * (FTR_VISIBLE fields; safe_val of each FTR_HIDDEN one) for every AArch64
 * register it tracks; an untracked slot reads 0, and so does an AArch32
 * register (CRm 2 and 3) of a CPU with no AArch32 at EL0, which the kernel
 * never reads. The rest of the space, CRm 1 included, stays SIGILL. */
static const struct { u8 crm, op2; u64 visible, hidden; } id_el0_view[] = {
    { 4, 0, 0x000f000f00ff0000ULL, 0x0000000000000011ULL },  /* ID_AA64PFR0: EL1, EL0 AArch64 only */
    { 4, 1, 0x0000f0000f000fffULL, 0 },                      /* ID_AA64PFR1 */
    { 4, 2, 0x0000000f00000ff0ULL, 0 },                      /* ID_AA64PFR2 */
    { 4, 4, 0x0fffff0f0ffff0ffULL, 0 },                      /* ID_AA64ZFR0 */
    { 4, 5, 0xbff1ffff73810001ULL, 0 },                      /* ID_AA64SMFR0 */
    { 4, 7, 0x00000000fc008003ULL, 0 },                      /* ID_AA64FPFR0 */
    { 5, 0, 0,                     0x0000000000000006ULL },  /* ID_AA64DFR0: DebugVer v8.0 */
    { 6, 0, 0xf0fffffff0fffff0ULL, 0 },                      /* ID_AA64ISAR0 */
    { 6, 1, 0xf0fff0ffffffffffULL, 0 },                      /* ID_AA64ISAR1 */
    { 6, 2, 0x0fff000000ffffffULL, 0 },                      /* ID_AA64ISAR2 */
    { 6, 3, 0x00000000f00f00f0ULL, 0 },                      /* ID_AA64ISAR3 */
    { 7, 0, 0xf000000000000000ULL, 0x00000111ff000000ULL },  /* ID_AA64MMFR0: TGran4/64 "none", TGran*_2 "as stage 1" */
    { 7, 1, 0x0000f00000000000ULL, 0 },                      /* ID_AA64MMFR1 */
    { 7, 2, 0x0000000f00000000ULL, 0 },                      /* ID_AA64MMFR2 */
    { 7, 3, 0x00000000000f0000ULL, 0 },                      /* ID_AA64MMFR3 */
};

/* do_el0_undef's try_emulate_mrs: whether the UNDEFINED instruction at pc is
 * such an MRS, answered (Rt written, pc past it) if so. */
static bool emulate_id_mrs(CPU *c) {
    u32 insn;
    if (!mem_ifetch(c, c->pc, &insn) || (insn & 0xfff00000u) != 0xd5300000u)
        return false;                                  /* not an MRS of op0 2/3 */
    unsigned op0 = 2 | ((insn >> 19) & 1), op1 = (insn >> 16) & 7;
    unsigned crn = (insn >> 12) & 15, crm = (insn >> 8) & 15, op2 = (insn >> 5) & 7;
    if (op0 != 3 || op1 != 0 || crn != 0 || crm == 1 || crm > 7) return false;
    u64 v = 0;
    if (crm == 0) {
        if (op2 == 0) v = sysreg_id_read(c, 0, 0);     /* MIDR_EL1 */
        else if (op2 == 5) v = 1ULL << 31;             /* MPIDR_EL1 */
        else if (op2 != 6) return false;               /* REVIDR_EL1 reads 0 */
    } else {
        for (size_t i = 0; i < sizeof id_el0_view / sizeof *id_el0_view; i++)
            if (id_el0_view[i].crm == crm && id_el0_view[i].op2 == op2) {
                v = (sysreg_id_read(c, crm, op2) & id_el0_view[i].visible) |
                    id_el0_view[i].hidden;
                break;
            }
    }
    set_x(c, insn & 31, v);
    c->pc += 4;
    return true;
}

/* The fault record the frames to come read (thread.h), set where a kernel
 * sets it. fault_note_abort is arch/arm64/mm/fault.c set_thread_esr, for a
 * data or instruction abort, an alignment fault among them: the address
 * untagged, the ESR as the CPU gave it -- except for an address no user page
 * can have (is_ttbr0_addr: past TASK_SIZE, or in the kernel's half), which
 * is reported as a level-0 translation fault keeping only EC, IL, CM and
 * WnR, as user space is never to learn about kernel mappings. */
static void fault_note_abort(u64 far, u64 esr) {
    u64 addr = a64_untag(far);
    g_tls.fault_address = addr;
    if (addr >= GUEST_TASK_SIZE) {
        const u64 ec_il = 0x3fULL << 26 | 1u << 25;
        if ((esr >> 26) == EC_DABORT_LOWER)
            esr = (esr & (ec_il | 1u << 8 /*CM*/ | 1u << 6 /*WnR*/)) | FSC_TRANS_L0;
        else
            esr = (esr & ec_il) | FSC_TRANS_L0;
    }
    g_tls.fault_code = esr;
}

/* ...and fault_note arch/arm64/kernel/traps.c arm64_notify_die, for an SP or
 * PC alignment fault and everything the kernel answers as an undefined
 * instruction (`esr` 0 then, which takes the record away): no address. */
static void fault_note(u64 esr) {
    g_tls.fault_address = 0;
    g_tls.fault_code = esr;
}

/* Fatal guest fault with no guest handler (M1..M4): report, restore the host
 * default disposition and re-raise so the parent sees the real termination
 * status (guest handler delivery arrives in M5). */
void force_sig_fault(CPU *c, int sig, int code, u64 addr) {
    (void)code;
    /* Include the faulting instruction word when the PC is still fetchable
     * (always true for SIGILL/undefined, where it names the missing opcode).
     * mem_ifetch is non-faulting: it reports failure instead of aborting. */
    u32 insn = 0;
    if (mem_ifetch(c, c->pc, &insn))
        fprintf(stderr,
                "arm64emu: guest fatal signal %d at pc=0x%llx addr=0x%llx insn=0x%08x\n",
                sig, (unsigned long long)c->pc, (unsigned long long)addr, insn);
    else
        fprintf(stderr,
                "arm64emu: guest fatal signal %d at pc=0x%llx addr=0x%llx\n",
                sig, (unsigned long long)c->pc, (unsigned long long)addr);
    if (c->m->strace) cpu_dump(c);
    /* Report the WIFSIGNALED death to our tracer, drop the /proc slot, restore the
     * host default and re-raise so the real parent sees the status. Shared with
     * the async default-fatal delivery path (sig_deliver_pending). */
    guest_terminate_by_signal(c, sig);
}

/* Does the run loop have to act for this thread before any more guest code
 * runs: a signal to deliver, a tracer's kick to serve (ptrace_service_kick),
 * an execve's call-out to answer (stop_gen)? All three raise g_sig_npend and,
 * with it, the CPU's attention (sig_npend_raise), the engines' way out of
 * guest code; this is the question behind the flag, which a signal the guest
 * blocks leaves set. The SVC dispatch below asks it before running a call.
 *
 * `at` is where a frame for the signal would say the thread was: none is
 * delivered on the rt_sigreturn trampoline (sig_deliver_pending), so a
 * signal is not due there, and the trampoline gets to run. */
int emu_callout_due(CPU *c, u64 at) {
    if (!g_sig_npend) return 0;
    return g_ptrace_kick ||
           __atomic_load_n(&c->m->stop_gen, __ATOMIC_ACQUIRE) != g_tls.stop_gen ||
           (!sig_on_trampoline(c->m, at) && sig_pending_deliverable(c->m));
}

/* ARM64EMU_PROF's ranking for the program this process runs, which ends here
 * -- an execve replaces it, or the process ends -- headed by whose it is; the
 * next program's starts empty. One per program, not per process: across an
 * execve the old image's PCs would mean nothing in the new one's. */
void diag_prof_flush(struct Machine *m) {
    if (!g_prof) return;
    char title[sizeof m->exec_path + 32];
    snprintf(title, sizeof title, "pid %d %s", (int)getpid(), m->exec_path);
    prof_dump(title);
    prof_reset();
}

/* What the diagnostics have to say as a fatal signal ends the process: the
 * dying thread's ring (unless the coverage finder printed it as it stopped
 * the CPU), the program's profile, and the JIT statistics, which the exit
 * path flushes but a signal death would lose. */
void diag_death(CPU *c) {
    if (!c->diag_stop) ring_dump(c);
    diag_prof_flush(c->m);
    jit_stats_flush();
}

/* This thread reached --max-insn, or a step diagnostic stopped its CPU
 * (ARM64EMU_COV's first foreign PC): the run ends here, and the process with
 * it, as a SIGKILL would end it -- its parent must not take a program cut
 * short for one that succeeded. The fatal-signal teardown prints the ring
 * and the profile (diag_death). Once per process: a sibling that reaches its
 * own limit meanwhile waits for the kill rather than report a second time. */
static _Noreturn void diag_cut(CPU *c) {
    static int cut;
    if (__atomic_exchange_n(&cut, 1, __ATOMIC_ACQ_REL))
        for (;;) pause();
    if (!c->diag_stop) {
        fprintf(stderr, "[maxinsn reached at icount=%llu]\n",
                (unsigned long long)c->icount);
        cpu_dump(c);
    }
    guest_terminate_by_signal(c, SIGKILL);
}

int emu_loop(CPU *c) {
    for (;;) {
        if (UNLIKELY(c->stop)) return 0;
        /* Called out of guest code: an execve is dismantling this thread group,
         * or has already replaced the image this thread belongs to. One shared
         * load per iteration buys every thread a bounded response time, which
         * is what lets execve tear the address space down knowing nobody is
         * still walking it (guest_stop_point, sys_proc.c). */
        if (UNLIKELY(__atomic_load_n(&c->m->stop_gen, __ATOMIC_ACQUIRE) !=
                     g_tls.stop_gen)) {
            guest_stop_point(c);
            continue;   /* re-test c->stop, and the counter it just synced */
        }

        /* volatile: assigned inside the sigsetjmp bracket below, so a recovery
         * unwind must not leave it indeterminate. It stays 0 on that path,
         * which is right — an instruction that faulted did not single-step. */
        volatile int stepped = 0;
        /* Bus-error recovery bracket (mem.c). A file truncated from outside
         * this address space leaves PTEs pointing at host pages the kernel now
         * refuses; the handler records the guest abort and unwinds to here,
         * where the bus_setjmp returns nonzero and the engines are skipped so
         * the pending exception below is delivered. Cheap: once per run-loop
         * round trip, not per instruction, and savemask 0 keeps it a pure
         * register save (the handler unblocks SIGBUS itself before unwinding).
         * On arm32 Bionic bus_setjmp is our own save, not libc sigsetjmp,
         * which parks a mangled value in the live sp -- fatal under this
         * loop's async-signal traffic (mmu.h has the story). Deliberately
         * does NOT cover syscall dispatch further down: a handler may hold
         * locks that an unwind out to here would strand -- the syscall layer
         * brackets its own guest-memory touches instead (BUS_ARM_COPY). */
        if (bus_setjmp(&g_bus_jb) == 0) {
        as_bus_arm(c, BUS_ARM_ENGINE);
        if (UNLIKELY(g_debug_hooks)) {
            /* Full step: keeps every per-instruction debug facility
             * (trace/rtrace/prof/ring/cov/tpc) behaving exactly as before. */
            cpu_step(c);
        } else if (UNLIKELY(g_ptrace_singlestep)) {
            /* PTRACE_SINGLESTEP: exactly one instruction via the interpreter
             * (never a JIT/predecode chunk), then a SIGTRAP stop below. */
            cpu_step(c);
            stepped = 1;
        } else if (UNLIKELY(jit_on())) {
            /* -jit: run translated blocks; same return contract as pd_run.
             * Under --max-insn, to a block short of the limit and then step
             * by step, which stops at it exactly (jit_deadline). */
            u64 deadline = jit_deadline(c, ~0ULL, g_max_insn);
            if (LIKELY(deadline)) jit_run(c, deadline);
            else cpu_step(c);
        } else if (LIKELY(g_pd)) {
            /* Threaded fast path: executes instructions back-to-back through
             * the decode cache, returning when the CPU asks for attention.
             * No deadline but --max-insn's, at which it stops exactly:
             * nothing else here is paced by instruction count. */
            pd_run(c, g_max_insn ? g_max_insn : ~0ULL);
        } else {
            /* -nopd: single-step fast path -- cpu_step minus its stop and
             * halted checks (the top of this loop made the first; halted is
             * cleared below before the next step). */
            c->cur_insn_pc = c->pc;
            u32 insn;
            if (LIKELY(mem_ifetch(c, c->pc, &insn))) {
                c->pc += 4;
                exec_a64(c, insn);
                c->icount++;
            }
        }
        }
        as_bus_disarm();

        /* --max-insn, counted per thread (its icount, from the start of the
         * program it runs), or the coverage finder's stop: before the loop
         * acts on anything the last instruction left it, a syscall included,
         * so every engine stops in the same state. */
        if (UNLIKELY(g_max_insn) && c->icount >= g_max_insn) diag_cut(c);
        if (UNLIKELY(c->diag_stop)) diag_cut(c);

        /* What the CPU asked for attention for is looked at from here on, so
         * the request is dropped first: one raised while this iteration acts
         * (a signal caught after its look) brings the next engine straight
         * back here instead of being lost (cpu.h, "Run control"). */
        cpu_attn_clear(c);

        /* WFE/WFI at EL0: treat as yield. */
        if (UNLIKELY(c->halted)) c->halted = false;

        if (UNLIKELY(g_tls.pend_exc.valid)) {
            g_tls.pend_exc.valid = false;
            u64 esr = g_tls.pend_exc.esr;
            u64 far = g_tls.pend_exc.far;
            unsigned ec = (unsigned)(esr >> 26);
            switch (ec) {
                case EC_SVC64:
                    /* A signal that arrived while the engine was in guest code
                     * is delivered BEFORE the syscall runs, as a kernel delivers
                     * one that lands on a task in user mode: before its next
                     * instruction. The engines check the lever at their safe
                     * points -- every instruction for the interpreter, block
                     * entries for the JIT -- so one that landed after the last
                     * check and before the SVC is found here, with the syscall
                     * not yet dispatched. Dispatched first, a blocking syscall
                     * never came back: the host signal that carried the guest's
                     * had been consumed queuing it, and nothing was left to
                     * interrupt the wait -- glibc's setxid broadcast wedged a
                     * thread blocking on the setxid lock a few instructions
                     * after a sibling's SIGSETXID landed (tests/fixtures/
                     * sigsvc.c). So the SVC is stepped back over and left for
                     * after the handler: the frame is built at it, and the
                     * return re-executes it, exactly the kernel's order. Only
                     * for what WILL be delivered -- a blocked or ignored signal
                     * stays queued and the syscall proceeds -- and for the
                     * emulator's own call-outs, which need this thread at the
                     * loop boundary rather than in a wait: a tracer's kick
                     * (ptrace_service_kick) and execve's de_thread (stop_gen).
                     *
                     * A capture that lands after this check and before the
                     * host syscall is entered is the one neither this nor the
                     * EINTR can catch; the flag raised first is what makes
                     * such a capture arm the kick timer that will (signal.c,
                     * "the capture kick"). Raised before the check, so that a
                     * capture between the two is seen by the check or arms
                     * the timer -- never neither.
                     *
                     * A signal is not delivered at the rt_sigreturn
                     * trampoline's SVC: it waits for the context the sigreturn
                     * restores (sig_deliver_pending says why), and the frame
                     * built here would be at that SVC. */
                    g_sig_in_syscall = 1;
                    if (UNLIKELY(g_sig_npend) && emu_callout_due(c, c->pc - 4)) {
                        g_sig_in_syscall = 0;
                        c->pc -= 4;
                        break;
                    }
                    syscall_dispatch(c);
                    g_sig_in_syscall = 0;
                    break;
                case EC_DABORT_LOWER:
                case EC_DABORT_SAME:
                case EC_IABORT_LOWER:
                case EC_IABORT_SAME: {
                    unsigned fsc = esr & 0x3f;
                    if (fsc == FSC_EXTERNAL) {
                        /* A page past end-of-file of a file mapping (mem.c
                         * raise_dabort), data or instruction: the kernel's
                         * translation fault, which the fault handler answers
                         * VM_FAULT_SIGBUS -- SIGBUS/BUS_ADRERR, with that
                         * fault's ESR in the frame. */
                        fault_note_abort(far, (esr & ~0x3fULL) | FSC_TRANS_L3);
                        sig_deliver_fault(c, SIGBUS, 2 /*BUS_ADRERR*/, far);
                    } else if (fsc == FSC_ALIGN) {
                        /* An unaligned atomic (decode.c atomic_align_fault):
                         * do_alignment_fault's SIGBUS/BUS_ADRALN. It was
                         * delivered as SIGSEGV/SEGV_MAPERR. */
                        fault_note_abort(far, esr);
                        sig_deliver_fault(c, SIGBUS, 1 /*BUS_ADRALN*/, far);
                    } else {
                        /* A permission fault -- the page is mapped, just not
                         * readable, writable or executable as asked -- is
                         * SEGV_ACCERR, and only a page with no mapping at all
                         * SEGV_MAPERR (the kernel's do_page_fault answers
                         * VM_FAULT_BADACCESS for the first). */
                        fault_note_abort(far, esr);
                        sig_deliver_fault(c, SIGSEGV,
                                          (fsc >= FSC_PERM_L0 && fsc <= FSC_PERM_L3) ? 2 : 1,
                                          far);
                    }
                    break;
                }
                case EC_PC_ALIGN:                /* si_addr: the PC (FAR) */
                    fault_note(esr);
                    sig_deliver_fault(c, SIGBUS, 1 /*BUS_ADRALN*/, far);
                    break;
                case EC_SP_ALIGN:
                    /* FAR means nothing for this one: the kernel's el0_sp
                     * reports the SP itself (do_sp_pc_abort(regs->sp)), as
                     * every engine left it, checked before any writeback. */
                    fault_note(esr);
                    sig_deliver_fault(c, SIGBUS, 1 /*BUS_ADRALN*/, reg_xsp(c, 31));
                    break;
                case EC_BRK64:
                    /* send_user_sigtrap: the fault record stays the last
                     * fault's (fault_note). */
                    sig_deliver_fault(c, SIGTRAP, 1, c->pc);
                    break;
                case EC_MOP: {
                    /* FEAT_MOPS main/epilogue state mismatch (wrong option, or
                     * an epilogue facing >= a page). Play the kernel's
                     * do_el0_mops: put the registers back in prologue input
                     * format (Arm ARM rules CNTMJ/MWFQH, mirroring Linux
                     * arm64_mops_reset_regs) and restart at the prologue —
                     * P/M/E are architecturally consecutive for this reason.
                     * c->pc still points at the trapping M/E instruction. */
                    bool wrong_option = (esr >> 17) & 1, option_a = (esr >> 16) & 1;
                    unsigned dreg = (esr >> 10) & 0x1f, sreg = (esr >> 5) & 0x1f;
                    unsigned nreg = esr & 0x1f;
                    u64 dst = reg_x(c, dreg), src = reg_x(c, sreg);
                    u64 size = reg_x(c, nreg);
                    if ((esr >> 24) & 1) {                             /* SET* */
                        if (option_a ^ wrong_option) {
                            dst += size; size = 0 - size;
                        }
                    } else {                                           /* CPY* */
                        if (!(option_a ^ wrong_option)) {
                            /* Format is from Option B; N set = backward */
                            if (c->nzcv & PS_N) { dst -= size; src -= size; }
                        } else if (size >> 63) {
                            /* Format is from Option A; negative = forward */
                            dst += size; src += size; size = 0 - size;
                        }
                    }
                    set_x(c, dreg, dst);
                    set_x(c, sreg, src);
                    set_x(c, nreg, size);
                    c->pc -= ((esr >> 18) & 1) ? 8 : 4;
                    break;
                }
                case EC_UNKNOWN:
                case EC_MSR_MRS:
                    /* do_el0_undef (and do_el0_sys, which no hook of ours
                     * claims): force_signal_inject with no ESR, which takes
                     * the record away. */
                    if (esr >> 26 == EC_UNKNOWN && emulate_id_mrs(c)) break;
                    fault_note(0);
                    sig_deliver_fault(c, SIGILL, 1, c->pc);
                    break;
                default:
                    fault_note(esr);         /* bad_el0_sync */
                    sig_deliver_fault(c, SIGILL, 1, c->pc);
                    break;
            }
        }

        /* The boundary has been reached: a kick timer armed inside a syscall
         * handler (by a capture, or by one of the emulator's own call-outs)
         * has done its job, or was never needed. Disarmed before anything is
         * serviced: a stop taken below parks the thread, and a timer still
         * firing through it re-flagged the call it had interrupted as ours
         * to restart -- after the stop had settled that it answers EINTR.
         *
         * Everything from here to the single-step trap is near-always idle,
         * and the single-step engine (-nopd) comes through once per
         * instruction: so each flag is tested inline, and the three services
         * below sit behind one test of the two that call for them. */
        if (UNLIKELY(g_kick_armed)) sig_kick_timer_disarm();

        if (UNLIKELY(g_sig_npend | g_ptrace_kick)) {
            /* Something is due, and the thread may be sitting at an SVC we
             * rewound (syscall_restart_internal), not yet dispatched again:
             * that call is still in progress to the guest, and what is due
             * finds it interrupted, as it would inside the host syscall
             * (syscall.c). */
            if (g_sig_npend) syscall_unrewind(c);

            /* Adopt a pending PTRACE_ATTACH/SEIZE or service a
             * PTRACE_INTERRUPT (the kick signal set g_ptrace_kick and reused
             * g_sig_npend to exit the fast path above). Before the signals,
             * as the kernel's get_signal takes a ptrace trap before it
             * dequeues one: a tracer's ptrace(SEIZE) has returned by the time
             * its kill(2) sends the next signal, and a standard signal is
             * taken ahead of the real-time kick, so the two arrive together
             * and the signal must find the thread traced already. */
            if (g_ptrace_kick) ptrace_service_kick(c);

            /* Deliver any host-caught guest signal at this safe boundary. */
            if (g_sig_npend) sig_deliver_pending(c);
        }

        /* Our own control signal interrupted a host syscall to get this thread
         * here (the attach kick above, a tracee's wake of its tracer, execve's
         * de_thread call-out). The kernel resumes a syscall it stops a task in;
         * so must we, or the guest sees a wait that ended early for no reason it
         * can observe. After the delivery above, so a guest signal's own
         * disposition -- frame, or SA_RESTART rewind -- decides first. */
        if (UNLIKELY(g_sig_selfintr)) syscall_restart_internal(c);

        /* PTRACE_SINGLESTEP: trap after exactly one stepped instruction. Gated
         * on `stepped` so arming single-step from within a stop (which resumes
         * mid-iteration) does not trap before an instruction has run. */
        if (UNLIKELY(stepped) && g_ptrace_singlestep && !c->stop)
            ptrace_report_singlestep(c);
    }
}
