/* A recvmsg whose msg_name is there and whose msg_namelen is 0: the name has
 * nowhere to go and its length is still reported (move_addr_to_user), for a
 * datagram of either family; a NULL msg_name leaves msg_namelen as it was.
 * recvmmsg likewise. Self-checking: qemu-user writes the name all the same and
 * a 0 for the NULL one. The expected block is a kernel's. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static void one(const char *what, int s, int tx, const struct sockaddr *to,
                socklen_t tolen, void *name, socklen_t nlen) {
    char buf[8], keep[8];
    memset(keep, 0xaa, sizeof keep);
    if (sendto(tx, "x", 1, 0, to, tolen) != 1) perror("sendto");
    struct iovec iov = {buf, sizeof buf};
    struct msghdr mh = {.msg_name = name, .msg_namelen = nlen, .msg_iov = &iov, .msg_iovlen = 1};
    if (name) memcpy(name, keep, sizeof keep);
    ssize_t r = recvmsg(s, &mh, 0);
    printf("%s: r=%zd namelen=%d untouched=%d\n", what, r, (int)mh.msg_namelen,
           name ? !memcmp(name, keep, sizeof keep) : -1);
}

int main(void) {
    char name[128];
    int a = socket(AF_INET, SOCK_DGRAM, 0), b = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in in = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t l = sizeof in;
    bind(a, (struct sockaddr *)&in, sizeof in);
    getsockname(a, (struct sockaddr *)&in, &l);
    struct sockaddr_in src = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    bind(b, (struct sockaddr *)&src, sizeof src);
    one("inet 0", a, b, (struct sockaddr *)&in, sizeof in, name, 0);
    one("inet null", a, b, (struct sockaddr *)&in, sizeof in, NULL, 16);
    one("inet 16", a, b, (struct sockaddr *)&in, sizeof in, name, 16);

    int u = socket(AF_UNIX, SOCK_DGRAM, 0), v = socket(AF_UNIX, SOCK_DGRAM, 0);
    struct sockaddr_un ua = {.sun_family = AF_UNIX};
    unlink("/tmp/recvname0_u"); unlink("/tmp/recvname0_v");
    strcpy(ua.sun_path, "/tmp/recvname0_v");
    bind(v, (struct sockaddr *)&ua, sizeof(sa_family_t) + strlen(ua.sun_path) + 1);
    strcpy(ua.sun_path, "/tmp/recvname0_u");
    socklen_t ul = sizeof(sa_family_t) + strlen(ua.sun_path) + 1;
    bind(u, (struct sockaddr *)&ua, ul);
    one("unix 0", u, v, (struct sockaddr *)&ua, ul, name, 0);
    int w = socket(AF_UNIX, SOCK_DGRAM, 0);   /* unbound: no name to report */
    one("unix unnamed 0", u, w, (struct sockaddr *)&ua, ul, name, 0);

    char mb[8];
    struct iovec mi = {mb, sizeof mb};
    struct mmsghdr mm = {.msg_hdr = {.msg_name = name, .msg_namelen = 0, .msg_iov = &mi, .msg_iovlen = 1}};
    if (sendto(v, "y", 1, 0, (struct sockaddr *)&ua, ul) != 1) perror("sendto");
    int r = recvmmsg(u, &mm, 1, 0, NULL);
    printf("mmsg 0: r=%d namelen=%d\n", r, (int)mm.msg_hdr.msg_namelen);
    unlink("/tmp/recvname0_u"); unlink("/tmp/recvname0_v");
    return 0;
}
