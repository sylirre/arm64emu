/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* The entry of every signal handler chroot-ng installs (sigsys.c,
 * cng_sig_entry): the host context the monitor's code runs in, made on the
 * thread the signal interrupted, whatever that thread's own was.
 *
 * void cng_hentry(int sig, siginfo_t *si, void *uc)
 *   The kernel enters it with the three arguments, sp at its frame, and x30 at
 *   the restorer. The host context is the thread's own (thread.c,
 *   cng_hentry_ctx: a guest thread of ours has its libc's TCB and shadow call
 *   stack; any other thread the main thread's), found first, on the
 *   interrupted context -- which the C side that finds it does not depend on.
 *   TPIDR_EL0 is the one register of the interrupted context the frame does
 *   not carry: it is saved here, the host's put in its place, and the saved
 *   one put back on the way out -- by then, whatever the C side has left in
 *   the save slot, which is how an emulated execve hands the new program none
 *   and a tracer's NT_ARM_TLS write takes effect (sigsys.c, struct cng_tps).
 *   x18 is the host's for the handler too, Bionic's shadow call stack, and
 *   comes back with the rest of the frame at sigreturn, as do the FP/SIMD
 *   registers, which the C side is free to spend.
 *
 * The slot is a struct cng_tps {tp, prev, host} on this stack, handed to
 * cng_handler_run as its fourth argument. */
    .text
    .balign 16
    .global cng_hentry
    .type cng_hentry, %function
cng_hentry:
    hint    #34                     /* bti c: entered as if by blr */
    stp     x29, x30, [sp, #-80]!
    mov     x29, sp
    stp     x0, x1, [sp, #16]
    str     x2, [sp, #32]
    bl      cng_hentry_ctx          /* -> {tp, x18} */
    ldp     x10, x11, [x0]
    mrs     x9, tpidr_el0
    stp     x9, xzr, [sp, #48]      /* {tp, prev} */
    str     x10, [sp, #64]          /* host */
    msr     tpidr_el0, x10
    mov     x18, x11
    ldp     x0, x1, [sp, #16]
    ldr     x2, [sp, #32]
    add     x3, sp, #48
    bl      cng_handler_run
    ldr     x9, [sp, #48]
    msr     tpidr_el0, x9
    ldp     x29, x30, [sp], #80
    ret
    .size cng_hentry, . - cng_hentry

/* A context to come back to, and the way back (thread.c: a guest thread's
 * exit unwinds to its start routine).
 *   long cng_ctx_save(struct cng_ctx *c): 0, and 1 when cng_ctx_jump(c) comes
 *     back to it -- with the callee-saved registers, sp, and x18 as they were.
 *   void cng_ctx_jump(const struct cng_ctx *c): never returns.
 * The context is 22 words: x19..x30, sp, x18, d8..d15. */
    .global cng_ctx_save
    .type cng_ctx_save, %function
cng_ctx_save:
    hint    #34
    stp     x19, x20, [x0, #0]
    stp     x21, x22, [x0, #16]
    stp     x23, x24, [x0, #32]
    stp     x25, x26, [x0, #48]
    stp     x27, x28, [x0, #64]
    stp     x29, x30, [x0, #80]
    mov     x9, sp
    stp     x9, x18, [x0, #96]
    stp     d8, d9, [x0, #112]
    stp     d10, d11, [x0, #128]
    stp     d12, d13, [x0, #144]
    stp     d14, d15, [x0, #160]
    mov     x0, #0
    ret
    .size cng_ctx_save, . - cng_ctx_save

    .global cng_ctx_jump
    .type cng_ctx_jump, %function
cng_ctx_jump:
    hint    #34
    ldp     x19, x20, [x0, #0]
    ldp     x21, x22, [x0, #16]
    ldp     x23, x24, [x0, #32]
    ldp     x25, x26, [x0, #48]
    ldp     x27, x28, [x0, #64]
    ldp     x29, x30, [x0, #80]
    ldp     x9, x18, [x0, #96]
    mov     sp, x9
    ldp     d8, d9, [x0, #112]
    ldp     d10, d11, [x0, #128]
    ldp     d12, d13, [x0, #144]
    ldp     d14, d15, [x0, #160]
    mov     x0, #1
    ret
    .size cng_ctx_jump, . - cng_ctx_jump

/* void cng_thread_enter(void *frame, unsigned long tp)
 *   Into the guest (thread.c): the guest's thread pointer in the register,
 *   and rt_sigreturn on the frame built for it, which sets everything else.
 *   The svc is the host's own code's, which the filters let through. */
    .global cng_thread_enter
    .type cng_thread_enter, %function
cng_thread_enter:
    hint    #34
    msr     tpidr_el0, x1
    mov     sp, x0
    mov     x8, #139                /* __NR_rt_sigreturn */
    svc     #0
    brk     #0
    .size cng_thread_enter, . - cng_thread_enter

    .section .note.GNU-stack,"",%progbits
