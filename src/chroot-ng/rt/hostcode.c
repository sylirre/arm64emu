/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* The host's own code: every executable segment of every object the dynamic
 * linker has loaded, less the vDSO (monitor/seccomp.c has what for). A unit of
 * its own, since <link.h> brings the libc's <elf.h>, whose names the loader's
 * cng/elf.h spells too. */
#define _GNU_SOURCE
#include <link.h>

#include "cng/rt.h"

struct hc_ctx {
    unsigned long (*out)[2];
    int max, n;
    unsigned long vdso;
};

static int hc_cb(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size;
    struct hc_ctx *c = data;
    for (int i = 0; i < info->dlpi_phnum && c->n < c->max; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if (ph->p_type != PT_LOAD || !(ph->p_flags & PF_X))
            continue;
        unsigned long lo = info->dlpi_addr + ph->p_vaddr;
        if (c->vdso && lo >= c->vdso && lo < c->vdso + (1UL << 20))
            continue;
        c->out[c->n][0] = lo;
        c->out[c->n][1] = lo + ph->p_memsz;
        c->n++;
    }
    return 0;
}

int cng_host_code(unsigned long (*out)[2], int max, unsigned long vdso) {
    struct hc_ctx c = {out, max, 0, vdso};
    dl_iterate_phdr(hc_cb, &c);
    return c.n;
}
