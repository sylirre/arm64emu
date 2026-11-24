/* -F/--file-backed: the program's segments are mapped from its file
 * (MAP_PRIVATE) instead of copied, and nothing about the image may tell the
 * two apart but where its pages come from.
 *
 * Checks, on this static program's own writable PT_LOAD:
 *   data -- the initialized bytes are the file's;
 *   bss  -- a zero-initialized array reads zero;
 *   tail -- the page the file part ends in reads zero past it (elf.c zeroes
 *           it, as the copy leaves it and as the kernel's padzero does ahead
 *           of the bss) where the file has other bytes. Checked over the part
 *           of that page bss_blk occupies, which nothing writes before this
 *           runs -- the bss ahead of it is libc's -- and reported "vacuous"
 *           when the file has only zeros there, so a toolchain that lays the
 *           image out otherwise cannot pass the check by accident;
 *   off  -- /proc/self/maps names the segment's own file offset, as it does
 *           for a kernel's file mapping (omitted with the argument "nooff",
 *           for a copied image, whose offsets are its span's);
 * and the run writes over its data, which a second run must not see: the
 * mapping is private, so the write is the page's copy and never the file's.
 *
 * Self-checking. */
#define _GNU_SOURCE
#include <elf.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/auxv.h>
#include <unistd.h>

static volatile unsigned char data_blk[5000] = { [0] = 1, [4999] = 2 };
static volatile unsigned char bss_blk[20000];

int main(int argc, char **argv) {
    int want_off = !(argc > 1 && !strcmp(argv[1], "nooff"));
    int data = data_blk[0] == 1 && data_blk[4999] == 2;
    for (int i = 1; i < 4999; i++)
        if (data_blk[i]) data = 0;
    int bss = 1;
    for (size_t i = 0; i < sizeof bss_blk; i++)
        if (bss_blk[i]) bss = 0;

    /* The writable PT_LOAD holding data_blk (static, ET_EXEC: no load bias). */
    const Elf64_Phdr *ph = (const Elf64_Phdr *)getauxval(AT_PHDR);
    unsigned long phnum = getauxval(AT_PHNUM);
    uintptr_t a = (uintptr_t)&data_blk[0];
    const Elf64_Phdr *seg = NULL;
    for (unsigned long i = 0; i < phnum; i++)
        if (ph[i].p_type == PT_LOAD && a >= ph[i].p_vaddr &&
            a < ph[i].p_vaddr + ph[i].p_memsz)
            seg = &ph[i];
    if (!seg) { printf("no segment\n"); return 1; }
    const char *tail = "1";
    uintptr_t fend = seg->p_vaddr + seg->p_filesz;
    uintptr_t pend = (fend + 4095) & ~(uintptr_t)4095;
    uintptr_t b0 = (uintptr_t)&bss_blk[0], b1 = b0 + sizeof bss_blk;
    uintptr_t t0 = b0 > fend ? b0 : fend, t1 = b1 < pend ? b1 : pend;
    unsigned char fbuf[4096];
    int fd = open(argv[0], O_RDONLY);
    ssize_t n = t1 > t0 && fd >= 0
        ? pread(fd, fbuf, t1 - t0, (off_t)(seg->p_offset + (t0 - seg->p_vaddr)))
        : -1;
    if (fd >= 0) close(fd);
    int file_nonzero = 0;
    for (ssize_t i = 0; i < n; i++)
        if (fbuf[i]) file_nonzero = 1;
    if (n <= 0 || !file_nonzero) tail = "vacuous";
    for (uintptr_t p = t0; p < t1; p++)
        if (*(volatile unsigned char *)p) tail = "0";

    data_blk[0] = 0x55;   /* a second run must still read 1 */
    bss_blk[100] = 7;

    char off[16] = "";
    if (want_off) {
        unsigned long long want = seg->p_offset + (a - seg->p_vaddr), got = ~0ULL;
        FILE *f = fopen("/proc/self/maps", "r");
        char line[512];
        while (f && fgets(line, sizeof line, f)) {
            unsigned long long lo, hi, o;
            if (sscanf(line, "%llx-%llx %*s %llx", &lo, &hi, &o) == 3 &&
                a >= lo && a < hi) {
                got = o + (a - lo);
                break;
            }
        }
        if (f) fclose(f);
        snprintf(off, sizeof off, " off=%s", got == want ? "file" : "other");
    }
    printf("data=%d bss=%d tail=%s%s\n", data, bss, tail, off);
    return 0;
}
