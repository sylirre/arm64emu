/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* The entry of every signal handler chroot-ng installs (sigsys.c,
 * cng_sig_entry): the host context the monitor's code runs in, made on the
 * thread the signal interrupted, whatever that thread's own was.
 *
 * void cng_hentry(int sig, siginfo_t *si, void *uc)
 *   The kernel enters it with the three arguments, sp at its frame, and x30 at
 *   the restorer. TPIDR_EL0 is the one register of the interrupted context the
 *   frame does not carry: it is saved here, the host's (cng_host_tp) put in its
 *   place, and the saved one put back on the way out -- by then, whatever the
 *   C side has left in the save slot, which is how an emulated execve hands
 *   the new program none and a tracer's NT_ARM_TLS write takes effect
 *   (sigsys.c, struct cng_tps). Everything else of the interrupted context is
 *   the frame's, sigreturn's to restore: the general registers, x18 among
 *   them, and the FP/SIMD ones, which the C side is free to spend.
 *
 * The slot is a struct cng_tps {tp, prev} on this stack, handed to
 * cng_handler_run as its fourth argument. */
    .text
    .balign 16
    .global cng_hentry
    .type cng_hentry, %function
cng_hentry:
    hint    #34                     /* bti c: entered as if by blr */
    stp     x29, x30, [sp, #-32]!
    mov     x29, sp
    mrs     x9, tpidr_el0
    stp     x9, xzr, [sp, #16]      /* {tp, prev} */
    adrp    x10, cng_host_tp
    ldr     x10, [x10, :lo12:cng_host_tp]
    msr     tpidr_el0, x10
    add     x3, sp, #16
    bl      cng_handler_run
    ldr     x9, [sp, #16]
    msr     tpidr_el0, x9
    ldp     x29, x30, [sp], #32
    ret
    .size cng_hentry, . - cng_hentry

    .section .note.GNU-stack,"",%progbits
