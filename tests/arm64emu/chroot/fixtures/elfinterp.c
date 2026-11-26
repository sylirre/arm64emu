/* A PT_INTERP that does not make sense, exec'd (elf.c elf_header_check): a
 * length outside [2, PATH_MAX] and a string with no NUL at its end are ENOEXEC,
 * one past the end of the file EIO -- load_elf_binary's order, answered while
 * the caller is still there to read it. The cases are this program's own file
 * with a note header turned into PT_INTERP. Self-checking: qemu-user re-runs
 * itself for an exec, and its own loader answers. */
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

static void one(const char *name, unsigned long off, unsigned long sz, int fill) {
    unsigned char *b = malloc(len);
    memcpy(b, img, len);
    Elf64_Ehdr *eh = (Elf64_Ehdr *)b;
    Elf64_Phdr *ph = (Elf64_Phdr *)(b + eh->e_phoff), *x = 0;
    for (int i = 0; i < eh->e_phnum && !x; i++)
        if (ph[i].p_type == PT_NOTE || ph[i].p_type == PT_GNU_STACK)
            x = &ph[i];
    x->p_type = PT_INTERP;
    x->p_offset = off;
    x->p_filesz = x->p_memsz = sz;
    if (fill)
        memset(b + off, 'x', sz);
    char p[] = "/tmp/elfinterp.x";
    unlink(p);
    int fd = open(p, O_CREAT | O_WRONLY, 0755);
    if (write(fd, b, len) != (ssize_t)len)
        exit(9);
    close(fd);
    pid_t k = fork();
    if (k == 0) {
        char *av[] = {p, 0}, *ev[] = {0};
        execve(p, av, ev);
        _exit(errno);
    }
    int st;
    waitpid(k, &st, 0);
    printf("%s: %s\n", name, WIFSIGNALED(st) ? "signal" : strerror(WEXITSTATUS(st)));
    unlink(p);
    free(b);
}

int main(void) {
    int fd = open("/proc/self/exe", O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st))
        return 1;
    len = (size_t)st.st_size;
    img = malloc(len);
    if (read(fd, img, len) != (ssize_t)len)
        return 1;
    one("past the end", len + 64, 16, 0);
    one("too long", 64, 8192, 0);
    one("too short", 64, 1, 0);
    one("no NUL", 64, 16, 1);
    return 0;
}
