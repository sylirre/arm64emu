/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* Linux-user exception seam. The core calls exception_take() as it would on
 * a full system, but here nothing ever vectors to EL1: the exception is
 * recorded, the CPU asks for attention, and the run loop (loop.c) dispatches
 * it -- SVC to the syscall layer, aborts/undef/BRK to guest signal delivery.
 * The CPU stays at EL0 for the lifetime of the process. */
#include "machine.h"

__thread ThreadState g_tls;

void exception_take(CPU *c, ExcKind kind, u64 esr, u64 far, u64 ret_addr) {
    (void)kind;
    g_tls.pend_exc.valid = true;
    g_tls.pend_exc.esr = esr;
    g_tls.pend_exc.far = far;
    /* SVC passes the next-instruction address (continue there after the
     * syscall); faults pass the faulting instruction (Linux re-executes it if
     * a handler repairs the situation). */
    c->pc = ret_addr;
    c->excl_valid = false;
    cpu_attn(c);                /* the run loop dispatches it */
}
