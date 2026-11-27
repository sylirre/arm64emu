/* The kernel identity a guest is told (M17-14): uname(2), whose faked fields
 * must be NUL-padded to the end (a shorter identity written over a longer one
 * left the host's tail past the terminator), and /proc/version, by name and
 * against a /proc dirfd, which must repeat the same release. Prints
 *   uname: rc=.. sys=.. node=.. rel=.. ver=.. mach=.. tail_dirty=N
 *   version: <first line of /proc/version>
 *   version_at: <the same, read through openat(dirfd("/proc"), "version")> */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

static void show(const char *tag, int dfd, const char *name) {
    int fd = openat(dfd, name, O_RDONLY);
    char b[512] = {0};
    if (fd >= 0) {
        long n = read(fd, b, sizeof b - 1);
        if (n > 0)
            b[n] = 0;
        close(fd);
    }
    b[strcspn(b, "\n")] = 0;
    printf("%s: %s\n", tag, fd >= 0 ? b : "<unopenable>");
}

int main(void) {
    char u[6][65];
    memset(u, 0x5a, sizeof u);
    long r = syscall(SYS_uname, u);
    int dirty = 0;
    for (int f = 0; f < 5; f++) {
        if (f == 1)
            continue; /* nodename is deliberately the host's */
        for (size_t k = strnlen(u[f], 65) + 1; k < 65; k++)
            dirty += u[f][k] != 0;
    }
    printf("uname: rc=%ld sys=%s node=%s rel=%s ver=%s mach=%s tail_dirty=%d\n", r,
           u[0], u[1], u[2], u[3], u[4], dirty);
    show("version", AT_FDCWD, "/proc/version");
    int d = open("/proc", O_RDONLY | O_DIRECTORY);
    show("version_at", d, "version");
    return 0;
}
