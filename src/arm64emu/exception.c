/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* Exception entry: every exception is taken to EL1 (we model EL0/EL1).
 * Return (ERET, cpu_unpack_spsr) is lib/arm64's cpu.c. */
#include "machine.h"
#include "esr.h"
#include <stdio.h>

void exception_vector(CPU *c, ExcKind kind, u64 esr, u64 far, u64 ret_addr) {
    unsigned tel = 1;            /* route to EL1 */
    if (c->el > tel) tel = c->el;

    u32 spsr = cpu_pack_spsr(c);
    bool from_lower = (c->el < tel);

    u64 off;
    if (from_lower) {
        off = 0x400;            /* lower EL, AArch64 */
    } else {
        off = c->sp_sel ? 0x200 : 0x000;  /* current EL with SP_ELx / SP_EL0 */
    }
    off += (u64)kind * 0x80;

    c->spsr[tel] = spsr;
    c->elr[tel]  = ret_addr;
    c->esr[tel]  = esr;
    c->far[tel]  = far;

    c->el = tel;
    c->sp_sel = 1;
    cpu_refresh_el_state(c);                /* FPEN=0b01 stops trapping at EL1;
                                             * the D-TLB tag follows the EL */
    c->daif |= PS_D | PS_A | PS_I | PS_F;   /* mask on entry */
    c->pc = (c->vbar[tel] & ~0x7ffULL) + off;
    c->halted = false;
    /* Taking an exception clears the local exclusive monitor. Without this, an
     * IRQ landing between a kernel LDXR and its STXR would leave the monitor
     * valid, letting the resumed STXR spuriously succeed and silently drop the
     * concurrent update -> intermittent lock/memory corruption in userspace. */
    c->excl_valid = false;

    if (g_trace || g_dbg)
        fprintf(stderr, "[exc] kind=%d -> EL%u vec=0x%llx esr=0x%llx far=0x%llx elr=0x%llx\n",
                kind, tel, (unsigned long long)c->pc, (unsigned long long)esr,
                (unsigned long long)far, (unsigned long long)ret_addr);
}
