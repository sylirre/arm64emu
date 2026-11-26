/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* The syscall gate: the only `svc #0` of the monitor's (the libc it is linked
 * with has its own, which run before the filter and never after: of the libc,
 * the monitor calls only mem/str, which make none).
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

/* The guest's FP/SIMD registers, saved to and loaded from a struct
 * user_fpsimd_state at \base: q0..q31, then fpsr and fpcr. x9 is spent. */
    .macro FPSAVE base
    stp     q0, q1, [\base, #0]
    stp     q2, q3, [\base, #32]
    stp     q4, q5, [\base, #64]
    stp     q6, q7, [\base, #96]
    stp     q8, q9, [\base, #128]
    stp     q10, q11, [\base, #160]
    stp     q12, q13, [\base, #192]
    stp     q14, q15, [\base, #224]
    stp     q16, q17, [\base, #256]
    stp     q18, q19, [\base, #288]
    stp     q20, q21, [\base, #320]
    stp     q22, q23, [\base, #352]
    stp     q24, q25, [\base, #384]
    stp     q26, q27, [\base, #416]
    stp     q28, q29, [\base, #448]
    stp     q30, q31, [\base, #480]
    mrs     x9, fpsr
    str     w9, [\base, #512]
    mrs     x9, fpcr
    str     w9, [\base, #516]
    str     xzr, [\base, #520]
    .endm
    .macro FPLOAD base
    ldp     q0, q1, [\base, #0]
    ldp     q2, q3, [\base, #32]
    ldp     q4, q5, [\base, #64]
    ldp     q6, q7, [\base, #96]
    ldp     q8, q9, [\base, #128]
    ldp     q10, q11, [\base, #160]
    ldp     q12, q13, [\base, #192]
    ldp     q14, q15, [\base, #224]
    ldp     q16, q17, [\base, #256]
    ldp     q18, q19, [\base, #288]
    ldp     q20, q21, [\base, #320]
    ldp     q22, q23, [\base, #352]
    ldp     q24, q25, [\base, #384]
    ldp     q26, q27, [\base, #416]
    ldp     q28, q29, [\base, #448]
    ldp     q30, q31, [\base, #480]
    ldr     w9, [\base, #512]
    msr     fpsr, x9
    ldr     w9, [\base, #516]
    msr     fpcr, x9
    .endm
#define FPSIMD_SZ 528 /* struct user_fpsimd_state */

/* The same bounce for a call whose dispatch has to run from guest context too
 * (sigsys.c, BOUNCE_DISP): one with a path or a socket address to translate,
 * where it is not the guest's own svc that blocks but the one the dispatcher
 * makes. The handler resumes the thread here with sp at the record, which says
 * where the dispatch is to run (sigsys.c, bounce_stack): the top of the
 * thread's scratch stack, whose busy flag is set here, or of a stack mapped
 * for the call, given back after it -- or nowhere, and the dispatch stays on
 * the guest's stack below the record.
 *
 * The C side spends the FP/SIMD registers, which are the guest's -- the kernel
 * keeps them across a syscall, and there is no signal frame here to keep them
 * in -- so they are saved first, on the stack switched to, before anything of
 * ours has run, and loaded again last: the completion trap's frame then holds
 * them for its sigreturn. In between they are where the dispatch hands them to
 * the guest's own call (cng_syscall_window) and to a ptrace stop.
 *
 * The first two instructions read the record and nothing else, which is what
 * cng_bounce_unwind takes as the dispatch not yet begun; from
 * cng_bounce_dispatch_done on, it is over. The completion is the trap above,
 * past the gate's end, which this has no svc to be inside of. */
    .text
    .balign 16
    .global cng_bounce_dispatch
    .type cng_bounce_dispatch, %function
cng_bounce_dispatch:
    mov     x0, sp                  /* the record */
    ldr     x9, [x0, #8]            /* ->stk */
    cbz     x9, 3f
    mov     x2, x0                  /* gsp: the guest's sp, at the record */
    mov     sp, x9
    ldr     x10, [x0, #16]          /* ->busyp */
    cbz     x10, 1f
    mov     w11, #1
    str     w11, [x10]
1:  sub     sp, sp, #(FPSIMD_SZ + 16)
    str     x0, [sp, #FPSIMD_SZ]
    FPSAVE  sp
    mov     x1, sp                  /* fpsave */
    bl      cng_bounce_dispatch_c   /* (record, fpsave, gsp) */
    FPLOAD  sp
    ldr     x0, [sp, #FPSIMD_SZ]
    mov     sp, x0
    ldr     x10, [x0, #16]
    cbz     x10, 2f
    str     wzr, [x10]
2:  ldr     x1, [x0, #32]           /* ->templen: a stack mapped for the call */
    cbz     x1, cng_bounce_dispatch_done
    ldr     x0, [x0, #24]           /* ->temp */
    mov     x6, #215                /* __NR_munmap, through the gate */
    bl      cng_syscall6
    b       cng_bounce_dispatch_done
3:  /* No stack of ours: in place, the save below the record. */
    sub     sp, sp, #(FPSIMD_SZ + 16)
    str     x0, [sp, #FPSIMD_SZ]
    FPSAVE  sp
    mov     x1, sp
    mov     x2, #0
    bl      cng_bounce_dispatch_c
    FPLOAD  sp
    ldr     x0, [sp, #FPSIMD_SZ]
    mov     sp, x0
    .global cng_bounce_dispatch_done
cng_bounce_dispatch_done:
    mov     x8, #160                /* __NR_uname, as above */
    b       __cng_gate_end
    .size cng_bounce_dispatch, . - cng_bounce_dispatch

/* long cng_syscall_window(const long *a, long nr, unsigned long sp,
 *                         void *fpsave)
 * The guest's call, from a dispatch bounced to guest context (dispatch.c,
 * reissue_svc): made through cng_syscall6_on, on the stack `sp` (0: the one
 * the caller is on), with the guest's FP/SIMD registers in place for it. A
 * handler of the guest's delivered while it blocks has them in its frame, and
 * whatever it leaves there is what is saved back. */
    .global cng_syscall_window
    .type cng_syscall_window, %function
cng_syscall_window:
    stp     x29, x30, [sp, #-32]!
    mov     x29, sp
    str     x3, [sp, #16]
    cbz     x3, 1f
    FPLOAD  x3
1:  cbnz    x2, 2f
    mov     x2, sp
2:  mov     x7, x2
    mov     x6, x1
    mov     x9, x0
    ldp     x2, x3, [x9, #16]
    ldp     x4, x5, [x9, #32]
    ldp     x0, x1, [x9]
    bl      cng_syscall6_on
    ldr     x3, [sp, #16]
    cbz     x3, 3f
    FPSAVE  x3
3:  ldp     x29, x30, [sp], #32
    ret
    .size cng_syscall_window, . - cng_syscall_window

/* void cng_pt_deliver_fp(const unsigned long *set, void *fpsave)
 * cng_pt_deliver, from a dispatch bounced to guest context: what it lets go
 * lands on the guest's registers -- the frame its handler puts back to the
 * guest's (ptrace.c, cng_pt_signal_frame) has the FP/SIMD half loaded here --
 * and what comes back is saved again, as for the window above. */
    .global cng_pt_deliver_fp
    .type cng_pt_deliver_fp, %function
cng_pt_deliver_fp:
    stp     x29, x30, [sp, #-32]!
    mov     x29, sp
    str     x1, [sp, #16]
    cbz     x1, 1f
    FPLOAD  x1
1:  bl      cng_pt_deliver
    ldr     x1, [sp, #16]
    cbz     x1, 2f
    FPSAVE  x1
2:  ldp     x29, x30, [sp], #32
    ret
    .size cng_pt_deliver_fp, . - cng_pt_deliver_fp

    .section .note.GNU-stack,"",%progbits
