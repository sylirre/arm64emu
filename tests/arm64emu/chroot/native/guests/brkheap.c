/* The program break, by the kernel's rules (mm/mmap.c, brk): every line names
 * what was asked and what the kernel answers, so the expected block in the
 * milestone is the kernel's output and a run under the native tier is compared with
 * it line for line. Raw syscalls throughout, and nothing that allocates
 * once the rows have begun: the libc's sbrk keeps a cached break of its own,
 * and a malloc that grew its heap from there would move the break under the
 * rows (glibc's main arena does, in a dynamic program).
 *
 *   brkheap            the rows
 *   brkheap exec ADDR  (internal) the program an exec started: is ADDR, a
 *                      page of the old program's heap, still mapped? */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned long P;

static unsigned long brk_(unsigned long a) { return (unsigned long)syscall(SYS_brk, a); }
static unsigned long pa(unsigned long a) { return (a + P - 1) & ~(P - 1); }

static int mapped(unsigned long a) {
    unsigned char v;
    return mincore((void *)(a & ~(P - 1)), P, &v) == 0;
}

/* /proc/self/maps, read whole into a buffer of our own: no stdio stream,
 * which would allocate. */
static char maps[1 << 16];

static int read_maps(void) {
    int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    long n = 0, r;
    if (fd < 0)
        return -1;
    while (n < (long)sizeof maps - 1 && (r = read(fd, maps + n, sizeof maps - 1 - n)) > 0)
        n += r;
    close(fd);
    maps[n] = '\0';
    return 0;
}

/* The next [heap] line's range, from *pos on. */
static int next_heap(char **pos, unsigned long *lo, unsigned long *hi) {
    for (char *l = *pos; *l;) {
        char *e = strchr(l, '\n');
        if (!e)
            e = l + strlen(l);
        char save = *e;
        *e = '\0';
        int is = strstr(l, "[heap]") && sscanf(l, "%lx-%lx", lo, hi) == 2;
        *e = save;
        l = *e ? e + 1 : e;
        if (is) {
            *pos = l;
            return 1;
        }
    }
    return 0;
}

/* The [heap] lines of /proc/self/maps: whether there are any, whether they run
 * end to end, and whether the last one ends at the page-aligned break. */
static void heap_lines(const char *what, unsigned long cur) {
    if (read_maps() < 0) {
        printf("%s: no maps\n", what);
        return;
    }
    int n = 0, contiguous = 1;
    unsigned long lo, hi, prev = 0;
    for (char *pos = maps; next_heap(&pos, &lo, &hi); n++) {
        if (n && lo != prev)
            contiguous = 0;
        prev = hi;
    }
    printf("%s: %s, %s, %s\n", what, n ? "named" : "none",
           contiguous ? "end to end" : "apart",
           prev == pa(cur) ? "ending at the break" : "not at the break");
}

static int child_exec(unsigned long old) {
    /* The new program's own heap may have come to lie over the page, which
     * is a page of the new program's and no longer the old one's. */
    int theirs = 0;
    unsigned long lo, hi;
    if (read_maps() == 0)
        for (char *pos = maps; next_heap(&pos, &lo, &hi);)
            if (old >= lo && old < hi)
                theirs = 1;
    printf("exec: the old heap %s\n",
           !mapped(old) || theirs ? "is gone" : "is still mapped");
    return 0;
}

int main(int argc, char **argv) {
    P = (unsigned long)sysconf(_SC_PAGESIZE);
    setvbuf(stdout, 0, _IONBF, 0);
    if (argc == 3 && !strcmp(argv[1], "exec"))
        return child_exec(strtoul(argv[2], 0, 16));

    unsigned long b = brk_(0), r;
    printf("brk(0) answers the break: %s\n", b && brk_(0) == b ? "yes" : "no");
    printf("below start_brk: %s\n", brk_(1) == b ? "unchanged" : "moved");

    r = brk_(b + 100);
    printf("grow by 100: %s\n", r == b + 100 ? "exactly as asked" : "other");
    memset((void *)b, 0x5a, 100);

    r = brk_(b + 3 * P + 5);
    printf("grow by three pages: %s, %s\n",
           r == b + 3 * P + 5 ? "exactly as asked" : "other",
           mapped(b + 3 * P + 4) ? "mapped to the break" : "not mapped");
    unsigned long mid = pa(b + 10);
    *(volatile char *)mid = (char)0xaa;

    r = brk_(b + 10);
    printf("shrink: %s, %s\n", r == b + 10 ? "exactly as asked" : "other",
           mid < pa(b + 3 * P + 5) && !mapped(mid) ? "pages given back"
                                                    : "pages kept");
    r = brk_(b + 2 * P);
    printf("grow back: %s, %s\n", r == b + 2 * P ? "exactly as asked" : "other",
           *(volatile char *)mid == 0 ? "zero-filled" : "old contents");
    printf("unaligned break kept: %s\n",
           brk_(0) == b + 2 * P ? "yes" : "no");

    /* A mapping above the break: growing into it, or up against it, is
     * refused; up to a page short of it is allowed. */
    unsigned long cur = brk_(0), q = pa(cur) + 16 * P;
    void *m = mmap((void *)q, P, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                   -1, 0);
    if (m != (void *)q) {
        printf("collision rows: no room\n");
    } else {
        printf("into a mapping: %s\n", brk_(q + 1) == cur ? "refused" : "allowed");
        printf("up against a mapping: %s\n",
               brk_(q - P + 1) == cur ? "refused" : "allowed");
        printf("a page short of a mapping: %s\n",
               brk_(q - P) == q - P ? "allowed" : "refused");
        printf("the mapping is untouched: %s\n", mapped(q) ? "yes" : "no");
        munmap((void *)q, P);
        brk_(cur);
    }
    cur = brk_(0);
    printf("past every address: %s\n",
           brk_(~0UL - 5) == cur && brk_(1UL << 62) == cur ? "refused" : "allowed");

    /* RLIMIT_DATA counts the heap: refused past it. A soft limit of 0 is
     * Valgrind's way of turning brk off, and refuses any growth. */
    struct rlimit old, lim;
    getrlimit(RLIMIT_DATA, &old);
    lim = old;
    lim.rlim_cur = 16 << 20;
    if (lim.rlim_max != RLIM_INFINITY && lim.rlim_max < lim.rlim_cur)
        lim.rlim_cur = lim.rlim_max;
    setrlimit(RLIMIT_DATA, &lim);
    printf("past RLIMIT_DATA: %s\n", brk_(cur + (64 << 20)) == cur ? "refused" : "allowed");
    lim.rlim_cur = 0;
    setrlimit(RLIMIT_DATA, &lim);
    printf("RLIMIT_DATA 0: %s\n", brk_(cur + P) == cur ? "refused" : "allowed");
    setrlimit(RLIMIT_DATA, &old);
    printf("limit lifted: %s\n", brk_(cur + P) == cur + P ? "allowed" : "refused");
    cur = brk_(0);
    heap_lines("maps [heap]", cur);

    /* A fork child's break is its own. */
    *(volatile char *)(pa(cur) - 1) = 1;
    pid_t pid = fork();
    if (pid == 0) {
        unsigned long c = brk_(0);
        int ok = brk_(c + 4 * P) == c + 4 * P;
        *(volatile char *)(pa(cur) - 1) = 2;
        memset((void *)c, 7, 4 * P);
        _exit(ok ? 0 : 1);
    }
    int st;
    waitpid(pid, &st, 0);
    printf("fork: the child %s, the parent's break %s, its heap %s\n",
           WIFEXITED(st) && WEXITSTATUS(st) == 0 ? "grew its own" : "could not grow",
           brk_(0) == cur ? "unchanged" : "moved",
           *(volatile char *)(pa(cur) - 1) == 1 ? "its own" : "written by the child");

    /* An exec leaves none of this heap behind. */
    char addr[32];
    snprintf(addr, sizeof addr, "%lx", pa(cur) - P);
    char *const av[] = {argv[0], "exec", addr, 0};
    execv("/proc/self/exe", av);
    printf("exec: %s\n", strerror(errno));
    return 1;
}
