/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* M36 test: SEV and SEVL set the event register in every tier.
 *
 * A WFE with the event register set completes at once and clears it; SEVL
 * (and SEV) set it. The predecoded tier classified both as NOPs, so a
 * SEVL; WFE pair -- the idiom Linux's smp_cond_load and spinlock slow paths
 * start with -- waited there for a wake source the reference interpreter
 * never needed, and here, with no interrupt ever raised, forever. Each pair
 * below must run straight through; the last WFE after the event has been
 * consumed is not executed (it would wait). x0=0 on success. */

.text
.global _start
_start:
    movz    x0, #1
    sevl
    wfe                                 /* consumes the event */
    movz    x0, #2
    sev
    wfe
    movz    x0, #3
    sevl
    nop
    wfe                                 /* the event survives other insns */
    movz    x0, #4
    sevl
    sevl
    wfe                                 /* one event, however many SEVLs */
    movz    x0, #0
    hlt     #0
