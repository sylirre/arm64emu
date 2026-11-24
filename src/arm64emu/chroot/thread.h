/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* Per-thread emulation state. With CLONE_VM guest threads, each guest thread
 * runs on its own host thread with its own CPU and the fields below; the
 * Machine (address space, fd table, signal dispositions) is shared. These live
 * in thread-local storage so the copied core's `c->m->...` accesses that must
 * be per-thread are routed here instead. */
#ifndef A64_THREAD_H
#define A64_THREAD_H

#include "types.h"

typedef struct {
    bool valid;
    u64  esr, far;
} PendExc;

typedef struct {
    PendExc pend_exc;         /* recorded by exception.c, dispatched by loop.c */
    /* The kernel's thread.fault_address and thread.fault_code: what every
     * signal frame this thread is given reads -- sigcontext.fault_address, and
     * an esr_context record while fault_code is not 0 (signal.c,
     * deliver_to_handler). Set only by the exceptions that set them in a
     * kernel (loop.c, fault_note), never cleared by a delivery: a BRK, a kill,
     * a seccomp trap still show the last fault's. Inherited by a new thread
     * (GThread, thread_entry) and by a fork child, kept by an execve -- the
     * exec'ing thread's, whichever thread then runs the image (Machine,
     * dethread_fault_address/_code). */
    u64 fault_address, fault_code;
    /* An rt_sigreturn refused its frame (signal.c, sig_return): the SIGSEGV
     * arm64_notify_segfault raises is due once the call has returned its 0
     * (syscall_dispatch, sig_badframe_deliver) -- its si_code and address. */
    u8  badframe;
    int badframe_code;
    u64 badframe_addr;
    /* The bits of the kernel's regs->pstate this CPU does not have (DIT,
     * TCO, BTYPE, SSBS, SS): what a tracer's SETREGSET or an rt_sigreturn
     * wrote there, which a kernel keeps as written until the thread returns
     * to user space -- a GETREGSET and a signal frame see them meanwhile --
     * and the hardware then never had. Cleared as guest code runs (loop.c);
     * NZCV is the CPU's own (signal.c, el0_pstate). */
    u32 pstate_extra;
    u64 clear_child_tid;      /* CLONE_CHILD_CLEARTID address (futex on exit) */
    u64 robust_head;          /* set_robust_list head, echoed by get_robust_list */
    /* Guest blocked-signal set: a per-thread attribute (POSIX), inherited
     * from the creator on clone. A process-wide set loses signals when
     * threads block/restore concurrently -- musl wraps every raise() and
     * pthread_exit() in block-all/restore, so a thread could exit with its
     * own raise()d signal still deferred by a sibling's block. */
    u64 sigmask;
    u64 saved_sigmask;        /* rt_sigsuspend: mask to record in the frame */
    int have_saved_sigmask;
    /* Copies of the Machine's call-out counters (machine.h). stop_gen is
     * compared once per run-loop iteration -- a mismatch means "go to the
     * safepoint and find out why"; image_gen says which loaded program this
     * thread belongs to, and a stale one means an execve replaced it. */
    u32 stop_gen;
    u32 image_gen;
    /* The group stop (Machine.jc_gseq) this thread has taken part in: one
     * later than this is still to be joined (signal.c, "group stop"). */
    u32 jc_seen;
    /* The arm64 tagged-address ABI (PR_SET_TAGGED_ADDR_CTRL): a thread's own,
     * inherited by a clone and a fork, cleared by execve. With it, a pointer
     * a syscall dereferences may carry a tag in its top byte; without it,
     * one that does is EFAULT, as the kernel's access_ok answers. The check
     * applies while `uaccess` says a syscall handler of this thread is
     * running (syscall_dispatch) -- not to the emulator's own accesses, nor
     * to a tracer's, which the kernel's GUP untags whatever the setting. */
    u32 tagged_addr_ctrl;
    u8  uaccess;
    /* The access this thread is making grows no stack (mem.c, as_stack_grow):
     * it stands for the kernel's GUP -- process_vm_readv/writev, a shared
     * futex's key -- which has not expanded a stack since 6.1.37, or it is
     * the emulator looking at guest memory for itself (the --strace-full
     * decoder), which no kernel does at all. Everything else faults a stack
     * down as the kernel's page-fault handler does: the guest's own
     * accesses, a syscall's copies, a tracer's PEEK and POKE
     * (access_remote_vm expands the stack itself). */
    u8  nogrow;
    /* mem_reachable answered "reachable" for a page in a stack's hole since
     * this syscall's handler began: the transfer being measured has bytes a
     * stack is to grow over, which xfer_begin (sys_file.c) has to know before
     * the host moves anything. */
    u8  xfer_hole;
    /* Syscall-restart bookkeeping (SA_RESTART on EINTR, and the emulator's own
     * internal wakeups -- syscall_restart_internal, src/arm64emu/chroot/syscall.c).
     * sc_orig_x0 is the kernel's regs->orig_x0, x0 as the SVC found it, before
     * a tracer's entry stop or the -ENOSYS of a call of -1: what seccomp
     * judges as args[0], what RET_TRAP puts back, what a restart the kernel
     * shows starts from. sc_restart_x0 is the x0 a restart is made with: the
     * call's own (an interruption of the emulator's, which no kernel would
     * have made, is invisible), until a stop or a handler -- the kernel's
     * do_signal -- has shown the restart, from orig_x0 (syscall.c,
     * syscall_restart_view). sc_rcode is what the kernel's call came back with
     * when this one came back EINTR (sc_ret_eintr): -ERESTARTSYS and the
     * other three restart codes, or a plain -EINTR (signal.c,
     * sc_restart_code); sc_icount the instruction count as the call
     * returned, which tells a stop on the way back to user space from one
     * after guest code has run. */
    u64 sc_svc_pc, sc_orig_x0, sc_restart_x0, sc_nr, sc_icount;
    s64 sc_rcode;
    int sc_ret_eintr;
    int sc_shown;             /* the restart has been shown (sc_restart_x0) */
    int sc_restarted;         /* this dispatch re-runs the SVC we rewound to */
    u64 sc_wait_t0;           /* CLOCK_MONOTONIC ns this attempt began to wait
                               * (syscall_wait_begin); 0 = not a timed wait */
    u64 sc_waited_ns;         /* how long earlier attempts at this same call
                               * already waited, so its timeout does not start
                               * over every time we restart it */
    int tid;                  /* thread id (main thread tid == pid) */
    /* Alternate signal stack: per-thread, as POSIX sigaltstack(2) is a
     * thread-local attribute. Storing it process-wide corrupts delivery under
     * multithreading -- each Go M registers its own gsignal stack, so a shared
     * field routes one thread's signal frame onto another thread's stack. */
    u64 sig_altstack_sp, sig_altstack_size;
    /* personality(2): a task attribute, so a thread's own. Inherited by a
     * clone from its creator and by a fork child from the forking thread,
     * kept across execve less what the exec clears (sys_personality). */
    u32 personality;
    /* prctl(PR_SET_NO_NEW_PRIVS) as this thread asked for it: a task flag
     * (PFA_NO_NEW_PRIVS), so a sibling setting it leaves this one as it was.
     * Inherited by a clone from its creator and by a fork child from the
     * forking thread, kept across execve; the process's view of it for
     * /proc is its main thread's (Machine.no_new_privs). */
    u8  no_new_privs;
    u8  pers_pub;             /* this thread's value is published in the
                               * broker for other processes' /proc views --
                               * it differs from its process's base value
                               * (proctab.c, "personality") */
    u32 sig_altstack_flags;   /* the kernel's sas_ss_flags: the ss_flags word
                               * as sigaltstack was given it (SS_AUTODISARM,
                               * or a SS_ONSTACK/SS_DISABLE the caller passed),
                               * SS_DISABLE after a reset; written raw into a
                               * frame's uc_stack, and only its SS_AUTODISARM
                               * bit reported back by sigaltstack */
} ThreadState;

extern __thread ThreadState g_tls;

#endif /* A64_THREAD_H */
