/* A bound AF_UNIX pathname reads back as the name the guest bound.
 *
 * The kernel keeps sun_path exactly as bind() was handed it, and every reader
 * of the address gets that string: getsockname of the socket, getpeername of a
 * connected peer, the source address of a datagram, accept's peer. The
 * emulator binds through the directory it pinned ("/proc/<pid>/fd/<n>/<name>")
 * and used to hand that spelling to all of them. Relative names stay relative,
 * as the kernel keeps them; a reader in another process gets them too. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static void show(const char *what, const struct sockaddr_un *u, socklen_t l,
                 const char *want) {
    size_t off = offsetof(struct sockaddr_un, sun_path);
    if (l <= off) { printf("%s: unnamed\n", what); return; }
    int same = !strcmp(u->sun_path, want) && l == off + strlen(want) + 1;
    printf("%s: %s len_ok=%d\n", what, same ? "as bound" : "OTHER", (int)(l == off + strlen(u->sun_path) + 1));
    if (!same) printf("  got [%s]\n", u->sun_path[0] == '/' ? "absolute" : u->sun_path);
}

static int mk(int type, const char *name) {
    int s = socket(AF_UNIX, type, 0);
    struct sockaddr_un u = {AF_UNIX, {0}};
    strcpy(u.sun_path, name);
    if (bind(s, (struct sockaddr *)&u, sizeof u) != 0) { perror("bind"); exit(1); }
    return s;
}

int main(void) {
    char dir[] = "/tmp/sunnameXXXXXX";
    if (!mkdtemp(dir) || chdir(dir) != 0) return 1;
    char abs_srv[200], abs_cli[200];
    snprintf(abs_srv, sizeof abs_srv, "%s/srv.sock", dir);
    snprintf(abs_cli, sizeof abs_cli, "%s/cli.sock", dir);
    struct sockaddr_un u;
    socklen_t l;

    /* A stream server, by its absolute name. */
    int srv = mk(SOCK_STREAM, abs_srv);
    listen(srv, 4);
    l = sizeof u; getsockname(srv, (struct sockaddr *)&u, &l);
    show("getsockname", &u, l, abs_srv);
    int c = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un to = {AF_UNIX, {0}};
    strcpy(to.sun_path, abs_srv);
    connect(c, (struct sockaddr *)&to, sizeof to);
    l = sizeof u; getpeername(c, (struct sockaddr *)&u, &l);
    show("getpeername", &u, l, abs_srv);
    int a = accept(srv, NULL, NULL);
    l = sizeof u; getsockname(a, (struct sockaddr *)&u, &l);
    show("accepted getsockname", &u, l, abs_srv);
    /* ...and from another process, which has no record of the bind. */
    pid_t k = fork();
    if (k == 0) {
        int c2 = socket(AF_UNIX, SOCK_STREAM, 0);
        connect(c2, (struct sockaddr *)&to, sizeof to);
        l = sizeof u; getpeername(c2, (struct sockaddr *)&u, &l);
        show("child getpeername", &u, l, abs_srv);
        fflush(stdout);
        _exit(0);
    }
    waitpid(k, NULL, 0);

    /* Datagrams: the source address is the sender's bound name. */
    int ds = mk(SOCK_DGRAM, "dsrv.sock");          /* relative, kept so */
    int dc = mk(SOCK_DGRAM, abs_cli);
    l = sizeof u; getsockname(ds, (struct sockaddr *)&u, &l);
    show("relative getsockname", &u, l, "dsrv.sock");
    struct sockaddr_un dto = {AF_UNIX, {0}};
    strcpy(dto.sun_path, "dsrv.sock");
    sendto(dc, "x", 1, 0, (struct sockaddr *)&dto, sizeof dto);
    char b;
    l = sizeof u;
    recvfrom(ds, &b, 1, 0, (struct sockaddr *)&u, &l);
    show("recvfrom source", &u, l, abs_cli);
    /* A short buffer: cut at the caller's size, the full length reported. */
    struct sockaddr_un sh;
    memset(&sh, 0x55, sizeof sh);
    l = offsetof(struct sockaddr_un, sun_path) + 3;
    getsockname(ds, (struct sockaddr *)&sh, &l);
    printf("short: len=%d bytes=%.3s next_untouched=%d\n", (int)l, sh.sun_path,
           ((unsigned char *)&sh)[offsetof(struct sockaddr_un, sun_path) + 3] == 0x55);

    unlink(abs_srv); unlink(abs_cli); unlink("dsrv.sock");
    if (chdir("/")) return 1;
    rmdir(dir);
    return 0;
}
