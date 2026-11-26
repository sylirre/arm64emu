/* What lies past the end of a file mapped executable (M23). A file mapping
 * longer than its file answers a fault on any page wholly past EOF with
 * SIGBUS, and the partial last page reads as zeroes past the file's bytes. The
 * monitor serves an executable file mapping the host refuses from an anonymous
 * copy, which used to read on into zeroes (`udf #0` for code) where the kernel
 * kills. Each page is touched from a child; prints what each child died of.
 *
 * usage: pasteof FILE   (FILE: under one page long) */
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *touch(volatile const char *p) {
    pid_t k = fork();
    if (k == 0) {
        _exit(*p == 0 ? 0 : 1);
    }
    int st = 0;
    waitpid(k, &st, 0);
    static char b[32];
    if (WIFSIGNALED(st))
        snprintf(b, sizeof b, "sig%d", WTERMSIG(st));
    else
        snprintf(b, sizeof b, "exit%d", WEXITSTATUS(st));
    return b;
}

int main(int argc, char **argv) {
    if (argc < 2)
        return 2;
    long pg = sysconf(_SC_PAGESIZE);
    int fd = open(argv[1], O_RDONLY);
    char *p = mmap(0, (size_t)(3 * pg), PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0);
    char *q = mmap(0, (size_t)pg, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, pg);
    if (p == MAP_FAILED || q == MAP_FAILED) {
        printf("map failed\n");
        return 1;
    }
    printf("page1-zeroes: %s\n", touch(p + pg - 1));
    printf("page2-past-eof: %s\n", touch(p + 2 * pg));
    printf("whole-mapping-past-eof: %s\n", touch(q));
    return 0;
}
