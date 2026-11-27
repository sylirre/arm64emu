/* Does this host let a process make anonymous memory executable? Map a page
 * read-write, write `mov w0, #7; ret` into it, make it read-execute and call
 * it. Exits 7 when the code ran, 1 when the mprotect was refused (Android's
 * execmem denial, PR_SET_MDWE). Run with no arm64emu in the way: it is the
 * host's answer the suite wants (lib.sh, cng_probe_caps). */
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void) {
    long pg = sysconf(_SC_PAGESIZE);
    uint32_t *p = mmap(0, (size_t)pg, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return 1;
    p[0] = 0x528000e0; /* mov w0, #7 */
    p[1] = 0xd65f03c0; /* ret */
    __builtin___clear_cache((char *)p, (char *)(p + 2));
    if (mprotect(p, (size_t)pg, PROT_READ | PROT_EXEC) != 0)
        return 1;
    return ((int (*)(void))(void *)p)();
}
