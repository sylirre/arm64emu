/* Read one directory to the end with getdents64 calls of a given size and
 * print every name: "dents: <name> <name> ...", then "dents: batches=N".
 *
 * usage: getdents DIR BUFSZ
 *
 * The entries that exist only as the monitor's overlays (a -b mount point, the
 * /dev nodes) have no dirent of their own and are spliced into the kernel's
 * records, so a small BUFSZ is what asks whether they survive a directory
 * whose real entries fill the caller's buffer -- musl's readdir reads 2 KiB at
 * a time where glibc's reads 32 KiB -- and whether a buffer that has room for
 * the overlay but not for one kernel record makes the injection repeat. A
 * read the kernel refuses (EINVAL: no record fits) ends the listing, as it
 * ends a libc's readdir. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 3)
        return 2;
    long bufsz = strtol(argv[2], NULL, 10);
    if (bufsz < 1 || bufsz > 65536)
        bufsz = 256;
    int fd = open(argv[1], O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        perror("open");
        return 1;
    }
    static char buf[65536];
    int batches = 0;
    printf("dents:");
    for (;;) {
        long n = syscall(SYS_getdents64, fd, buf, bufsz);
        if (n <= 0 || ++batches > 4096)
            break;
        for (long o = 0; o + 19 <= n;) {
            unsigned short rl;
            memcpy(&rl, buf + o + 16, 2);
            if (rl == 0 || o + rl > n)
                break;
            printf(" %s", buf + o + 19);
            o += rl;
        }
    }
    printf("\ndents: batches=%d\n", batches);
    close(fd);
    return 0;
}
