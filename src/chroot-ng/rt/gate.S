/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* The syscall gate: the only `svc #0` of the monitor's (the libc it is linked
 * with has its own, which run before the filter and never after: nothing of the
 * monitor calls into the libc).
 *
 * long cng_syscall6(long a0,a1,a2,a3,a4,a5, long nr)
 *   AAPCS64: a0..a5 arrive in x0..x5, nr in x6, result returned in x0.
 *
 * The seccomp filter installed later (SIGSYS milestone) allows any syscall
 * whose instruction pointer lies in [__cng_gate_start, __cng_gate_end); that
 * is how the SIGSYS handler re-issues translated syscalls without trapping
 * itself.  Keep this region tiny and self-contained.
 *
 * The instruction pointer the filter sees is the one AFTER the svc, so a svc
 * is in the gate only if the instruction that follows it is too: __cng_gate_end
 * must never directly follow a svc that is meant to be allowed.
 *
 * The filter compares the pointer's high word for equality and its low word
 * against the range (seccomp.c), which is exact only for a gate that does not
 * cross a 4 GiB boundary; wherever the loader puts the image, one that starts
 * 256-aligned and fits in 256 bytes cannot (the end of the file asserts it).
 */
    .section .text.gate,"ax",%progbits
    .balign 256
    .global __cng_gate_start
    .hidden __cng_gate_start
__cng_gate_start:

    .global cng_syscall6
    .type cng_syscall6, %function
cng_syscall6:
    mov     x8, x6
    svc     #0
    ret
    .size cng_syscall6, . - cng_syscall6

/* long cng_syscall6_on(long a0,a1,a2,a3,a4,a5, long nr, unsigned long sp)
 *   The same, with the svc made on the stack `sp` (in x7), for a call a
 *   dispatch makes from guest context on the guest's behalf (dispatch.c,
 *   reissue_svc): a signal delivered while it blocks lands there. x9 keeps
 *   ours across the svc, which preserves it, as does any handler's sigreturn.
 */
    .global cng_syscall6_on
    .type cng_syscall6_on, %function
cng_syscall6_on:
    mov     x9, sp
    mov     sp, x7
    mov     x8, x6
    svc     #0
    mov     sp, x9
    ret
    .size cng_syscall6_on, . - cng_syscall6_on

/* The kickable wait (ptrace.c, cng_pt_wait_native): a guest-context blocking
 * wait4 or waitid, made as the guest issued it — unless a ptrace kick has been
 * taken since the caller read *kicks as `seen`.
 *
 * long cng_wait_unless(long a0, a1, a2, a3, a4, long nr,
 *                      const unsigned *kicks, unsigned seen)
 *   a0..a4 in x0..x4, nr in x5 (the svc's sixth argument, which neither call
 *   reads), kicks in x6, seen in w7.
 *
 * The compare covers a kick taken before it; one taken from the compare up to
 * and including the svc — where the value read is already stale, or a restart
 * of the svc is pending — has its handler resume here at the bail instead
 * (cng_pt_kick_taken); and one that interrupts the svc has its -EINTR replaced
 * the same way. The bail's -512 is CNG_WAIT_KICKED: ERESTARTSYS, which no wait
 * ever returns to user space. */
    .global cng_wait_unless
    .type cng_wait_unless, %function
cng_wait_unless:
    mov     x8, x5
    .global cng_wait_unless_check
cng_wait_unless_check:
    ldr     w9, [x6]
    cmp     w9, w7
    b.ne    cng_wait_unless_bail
    .global cng_wait_unless_svc
cng_wait_unless_svc:
    svc     #0
    ret
    .global cng_wait_unless_bail
cng_wait_unless_bail:
    mov     x0, #-512
    ret
    .size cng_wait_unless, . - cng_wait_unless

/* The two points a tracee's held-back signals are let go at (ptrace.c,
 * cng_pt_signal_frame): rt_sigprocmask(SIG_UNBLOCK, set, 0, 8), and a signal
 * it delivers lands on the instruction after the svc, which tells the
 * handler where it is.
 *
 * void cng_pt_deliver(const unsigned long *set): a dispatch from guest
 *   context is over, and the guest's registers are the record its handler
 *   resumes the thread with (cng_pt_dispatch_end).
 * void cng_pt_unhold(const unsigned long *set): the guest's own call is about
 *   to be made, which a signal pending then is to cut short (reissue_svc). */
    .global cng_pt_deliver
    .type cng_pt_deliver, %function
cng_pt_deliver:
    mov     x1, x0
    mov     x0, #1                  /* SIG_UNBLOCK */
    mov     x2, #0
    mov     x3, #8
    mov     x8, #135                /* __NR_rt_sigprocmask */
    svc     #0
    .global cng_pt_deliver_ret
cng_pt_deliver_ret:
    ret
    .size cng_pt_deliver, . - cng_pt_deliver

    .global cng_pt_unhold
    .type cng_pt_unhold, %function
cng_pt_unhold:
    mov     x1, x0
    mov     x0, #1                  /* SIG_UNBLOCK */
    mov     x2, #0
    mov     x3, #8
    mov     x8, #135                /* __NR_rt_sigprocmask */
    svc     #0
    .global cng_pt_unhold_ret
cng_pt_unhold_ret:
    ret
    .size cng_pt_unhold, . - cng_pt_unhold

/* The bounce (sigsys.c): a syscall the SIGSYS handler must not run itself —
 * a wait that installs a signal mask of its own for its duration — is run from
 * here instead, in the guest's own context, with the guest's own registers.
 * The handler rewrites the trap frame so that sigreturn resumes at
 * cng_bounce_wait with x0..x5 and x8 as the guest issued them (a mask pointer
 * redirected to a copy with SIGSYS taken out); the svc is inside the gate, and
 * so is the mov after it, where the filter sees its instruction pointer, so
 * the filter allows it. The svc after __cng_gate_end is outside the gate and
 * traps: that is the completion, which the handler recognizes by its address
 * (cng_bounce_svc_end) and answers by putting the guest's registers back and
 * resuming it after its own svc with the wait's result. The number in x8 there
 * only has to be one every filter of ours traps; uname is. Nothing runs past
 * the completion, hence the brk. */
    .global cng_bounce_wait
    .type cng_bounce_wait, %function
cng_bounce_wait:
    svc     #0
    mov     x8, #160                /* __NR_uname */

    .global __cng_gate_end
    .hidden __cng_gate_end
__cng_gate_end:
    .if (__cng_gate_end - __cng_gate_start) > 256
    .error "the gate is longer than the 256 bytes it is aligned to (see the top)"
    .endif
    svc     #0
    .global cng_bounce_svc_end
cng_bounce_svc_end:
    brk     #0
    .size cng_bounce_wait, . - cng_bounce_wait

/* The same bounce for a call whose dispatch has to run from guest context too
 * (sigsys.c, BOUNCE_DISP): one with a path or a socket address to translate,
 * where it is not the guest's own svc that blocks but the one the dispatcher
 * makes. The handler resumes the thread here with sp at the record; the C side
 * runs the dispatch on the record as its register file, and the completion
 * is the trap above. Past
 * the gate's end, which this has no svc to be inside of. */
    .global cng_bounce_dispatch
    .type cng_bounce_dispatch, %function
cng_bounce_dispatch:
    mov     x0, sp                  /* the record */
    bl      cng_bounce_dispatch_c
    mov     x8, #160                /* __NR_uname, as above */
    b       __cng_gate_end
    .size cng_bounce_dispatch, . - cng_bounce_dispatch
