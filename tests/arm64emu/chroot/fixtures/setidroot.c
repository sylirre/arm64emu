/* --setuid-root / --setgid-root: a set-id executable is shown as owned by
 * root (uid 0 for set-uid, group 0 for set-gid with group execute -- the bit
 * alone is the old mandatory-locking mark, not set-gid), and executing it
 * raises the effective id to 0. Without the flags the same files show, and
 * raise to, the fake identity that owns them, since every rootfs file is the
 * invoking user's on the host.
 *
 * Run from a rootfs /tmp under a fake identity: copies itself with each mode,
 * prints the owner a stat shows, then executes the copy, which prints the
 * ids it came up with. Self-checking: the ids are the fake identity's. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int copy_self(const char *self, const char *dst, mode_t mode) {
    int in = open(self, O_RDONLY), out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0700);
    if (in < 0 || out < 0) return -1;
    char buf[65536];
    ssize_t n;
    while ((n = read(in, buf, sizeof buf)) > 0)
        if (write(out, buf, (size_t)n) != n) return -1;
    close(in);
    close(out);
    return chmod(dst, mode);   /* after the writes: a write clears S_ISUID */
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "child")) {
        printf(" euid=%u egid=%u\n", (unsigned)geteuid(), (unsigned)getegid());
        return 0;
    }
    static const struct { const char *name; mode_t mode; } k[] = {
        { "setuid", 04755 }, { "setgid", 02755 }, { "setgid-no-gx", 02745 },
        { "plain", 0755 },
    };
    for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++) {
        char dst[64];
        snprintf(dst, sizeof dst, "/tmp/ci_sr_%s", k[i].name);
        struct stat st;
        if (copy_self(argv[0], dst, k[i].mode) < 0 || stat(dst, &st) < 0) {
            printf("%s: setup failed\n", k[i].name);
            continue;
        }
        printf("%s uid=%u gid=%u", k[i].name, (unsigned)st.st_uid, (unsigned)st.st_gid);
        fflush(stdout);
        pid_t p = fork();
        if (p == 0) {
            execl(dst, dst, "child", (char *)0);
            _exit(127);
        }
        int ws;
        waitpid(p, &ws, 0);
        unlink(dst);
    }
    printf("done\n");
    return 0;
}
