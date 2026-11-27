/* Programs whose headers do not make sense, exec'd (M3).
 *
 * Each case is this program's own file with a field of its headers changed. A
 * loader that sizes a mapping, a read or a copy from one has to judge it first:
 * the process that execs it must either live to read an errno or be killed as a
 * kernel kills it, and nothing else may come of it — least of all a write outside
 * what the object reserves, which in this one is the monitor's own memory.
 *
 * The exec is made in a child. Prints "<case>: errno N", "signal N" or "ran" per
 * case, and "parent alive" at the end. Not here: a p_offset that does not share
 * its p_vaddr's page offset, which the kernel cannot map and the native tier's copying
 * loader takes as it stands (how a 4 KiB-aligned program runs on a 16 KiB-page
 * kernel) -- the program then runs on what the header says, an answer of its
 * own and not the loader's.
 */
#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned char *img;
static size_t len;

/* The `n`th program header of a type in the copy `b`. */
static Elf64_Phdr *ph_of(unsigned char *b, unsigned type, int n) {
    Elf64_Ehdr *eh = (Elf64_Ehdr *)b;
    Elf64_Phdr *ph = (Elf64_Phdr *)(b + eh->e_phoff);
    for (int i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == type && n-- == 0)
            return &ph[i];
    return 0;
}

static void run(const char *name, unsigned char *b, size_t n) {
    const char *p = "/elfbad.x";
    unlink(p);
    int fd = open(p, O_CREAT | O_WRONLY | O_TRUNC, 0755);
    if (fd < 0 || write(fd, b, n) != (ssize_t)n)
        exit(9);
    close(fd);
    pid_t k = fork();
    if (k == 0) {
        /* A program that runs on what its headers say and hangs on it is
         * killed by the alarm, which the exec keeps: "signal 14". */
        alarm(30);
        char *av[] = {"x", "ran", 0}, *ev[] = {0};
        execve(p, av, ev);
        _exit(errno);
    }
    int st;
    waitpid(k, &st, 0);
    if (WIFSIGNALED(st))
        printf("%s: signal %d\n", name, WTERMSIG(st));
    else if (WEXITSTATUS(st) == 200)
        printf("%s: ran\n", name);
    else
        printf("%s: errno %d\n", name, WEXITSTATUS(st));
}

#define BAD(name, ...)                                                         \
    do {                                                                       \
        unsigned char *b = malloc(len);                                        \
        memcpy(b, img, len);                                                   \
        Elf64_Ehdr *eh = (Elf64_Ehdr *)b;                                      \
        Elf64_Phdr *l0 = ph_of(b, PT_LOAD, 0), *l1 = ph_of(b, PT_LOAD, 1);     \
        size_t n = len;                                                        \
        (void)eh; (void)l0; (void)l1;                                          \
        __VA_ARGS__;                                                           \
        run(name, b, n);                                                       \
        free(b);                                                               \
    } while (0)

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "ran"))
        return 200;
    setvbuf(stdout, 0, _IOLBF, 0);
    int fd = open("/proc/self/exe", O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0)
        return 9;
    len = (size_t)st.st_size;
    img = malloc(len);
    if (read(fd, img, len) != (ssize_t)len)
        return 9;
    (void)argv;

    BAD("unchanged", (void)0);
    BAD("no headers", eh->e_phnum = 0);
    BAD("odd header size", eh->e_phentsize = 40);
    BAD("headers past the end", eh->e_phoff = len + 4096);
    BAD("wrong machine", eh->e_machine = 62);
    BAD("a relocatable", eh->e_type = ET_REL);
    BAD("cut in the header", n = 40);
    BAD("no segment", /* every one: lld links four, ld.bfd two or more */
        for (Elf64_Phdr *l; (l = ph_of(b, PT_LOAD, 0));) l->p_type = PT_NULL);
    BAD("file part past the memory part", l1->p_memsz = l1->p_filesz / 2);
    BAD("file part past the end", l1->p_filesz = l1->p_memsz = len * 64);
    BAD("a segment that wraps", l1->p_vaddr = 0xffffffffffff0000ull; l1->p_memsz = 0x20000);
    BAD("a span over all addresses", l1->p_vaddr = 0x7fffffff00000000ull);
    BAD("an end that rounds up to 0", l1->p_vaddr = 0; l1->p_memsz = 0xfffffffffffff001ull);
    BAD("an interpreter past the end",
        Elf64_Phdr *x = ph_of(b, PT_NOTE, 0) ? ph_of(b, PT_NOTE, 0) : ph_of(b, PT_GNU_STACK, 0);
        x->p_type = PT_INTERP; x->p_offset = len + 64; x->p_filesz = x->p_memsz = 16);
    BAD("an interpreter too long",
        Elf64_Phdr *x = ph_of(b, PT_NOTE, 0) ? ph_of(b, PT_NOTE, 0) : ph_of(b, PT_GNU_STACK, 0);
        x->p_type = PT_INTERP; x->p_offset = 64; x->p_filesz = x->p_memsz = 8192);
    BAD("an interpreter too short",
        Elf64_Phdr *x = ph_of(b, PT_NOTE, 0) ? ph_of(b, PT_NOTE, 0) : ph_of(b, PT_GNU_STACK, 0);
        x->p_type = PT_INTERP; x->p_offset = 64; x->p_filesz = x->p_memsz = 1);
    BAD("an interpreter with no end",
        Elf64_Phdr *x = ph_of(b, PT_NOTE, 0) ? ph_of(b, PT_NOTE, 0) : ph_of(b, PT_GNU_STACK, 0);
        x->p_type = PT_INTERP; x->p_offset = 64; x->p_filesz = x->p_memsz = 16;
        memset(b + 64, 'x', 16));
    unlink("/elfbad.x");
    printf("parent alive\n");
    return 0;
}
