/* Ioctls beside the interface-query band, after one inside it.
 *
 * The filter traps SIOCGIFNAME..SIOCGIFMAP (0x8910..0x8970) and cng_nl_ioctl
 * answers them from the interface enumeration, taking the argument for an
 * ifreq. That is the whole of what it is for -- but a trapped `svc` is patched
 * by the lazy rewriter, one site serves every request its syscall is asked,
 * and under -R every ioctl reaches the dispatcher anyway. Requests that were
 * never the band's then met the ifreq copy: a NULL argument, or one that ends
 * a mapping, was EFAULT where the host answers (a terminal's TIOCSCTTY lost
 * its controlling terminal that way on Android, under -R, once anything had
 * asked for the interface list).
 *
 * Prints one line per case, 0 or -errno, so the expected text is a kernel's
 * wherever the first one is answered:
 *   siocgifconf   a band request, to have the site trapped and patched;
 *   fioclex       FIOCLEX, no argument at all: the close-on-exec bit is on;
 *   fionclex      FIONCLEX, likewise: off again;
 *   fionread_edge FIONREAD into the last word of a mapping with an unmapped
 *                 page after it, on a pipe holding three bytes.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#define T_FIONCLEX 0x5450UL
#define T_FIOCLEX  0x5451UL
#define T_FIONREAD 0x541bUL
#define T_SIOCGIFCONF 0x8912UL

static int cloexec(int fd) {
    int f = fcntl(fd, F_GETFD);
    return f < 0 ? -errno : !!(f & FD_CLOEXEC);
}

int main(void) {
    int sk = socket(AF_INET, SOCK_DGRAM, 0);
    char buf[512];
    struct {
        int len, pad;
        char *buf;
    } ifc = {sizeof buf, 0, buf};
    errno = 0;
    int r = sk < 0 ? -1 : ioctl(sk, T_SIOCGIFCONF, &ifc);
    printf("siocgifconf=%d\n", r < 0 ? -errno : 0);

    int p[2];
    if (pipe(p))
        return 1;
    errno = 0;
    r = ioctl(p[0], T_FIOCLEX, 0);
    printf("fioclex=%d cloexec=%d\n", r < 0 ? -errno : 0, cloexec(p[0]));
    errno = 0;
    r = ioctl(p[0], T_FIONCLEX, 0);
    printf("fionclex=%d cloexec=%d\n", r < 0 ? -errno : 0, cloexec(p[0]));

    long pg = sysconf(_SC_PAGESIZE);
    char *m = mmap(0, (size_t)pg * 2, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED || mprotect(m + pg, (size_t)pg, PROT_NONE))
        return 1;
    int *edge = (int *)(m + pg - sizeof(int));
    if (write(p[1], "abc", 3) != 3)
        return 1;
    *edge = -1;
    errno = 0;
    r = ioctl(p[0], T_FIONREAD, edge);
    printf("fionread_edge=%d n=%d\n", r < 0 ? -errno : 0, *edge);
    return 0;
}
