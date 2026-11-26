/* The /proc/<pid> links the emulator supplies a target for -- exe, cwd, root -- asked
 * about as links.
 *
 * Followed, they lead where the guest's own program, directory and root are.
 * Not followed they are symlinks of procfs like any other: lstat says so, an
 * O_PATH|O_NOFOLLOW descriptor is of the link and reads back the target, an
 * open that refuses a link refuses it, and nothing removes or renames it -- least
 * of all the file it leads to.
 *
 * The descriptor's read-back is the one that used to name the emulator's own
 * binary. Self-checking: qemu-user follows /proc/self/exe whatever the flags say.
 * The expected block is a kernel's.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

#define E(r) ((r) < 0 ? errno : 0)

int main(int argc, char **argv) {
    char own[64];
    snprintf(own, sizeof own, "/proc/%d", (int)getpid());
    const char *dirs[] = {"/proc/self", "/proc/thread-self", own};
    static const char *const names[] = {"exe", "cwd", "root"};
    for (int d = 0; d < 3; d++) {
        for (int i = 0; i < 3; i++) {
            char p[128], t[512], t2[512];
            struct stat l, s, f;
            snprintf(p, sizeof p, "%s/%s", dirs[d], names[i]);
            int rl = lstat(p, &l), rs = stat(p, &s);
            printf("%s %s: lstat=%d link=%d stat=%d kind=%d", d == 2 ? "/proc/<pid>" : dirs[d],
                   names[i], E(rl), rl == 0 && S_ISLNK(l.st_mode), E(rs),
                   rs == 0 && (i == 0 ? S_ISREG(s.st_mode) : S_ISDIR(s.st_mode)));
            errno = 0;
            printf(" nofollow=%d", E(open(p, O_RDONLY | O_NOFOLLOW)));
            ssize_t n = readlink(p, t, sizeof t);
            int fd = open(p, O_PATH | O_NOFOLLOW);
            int fr = fstat(fd, &f);
            ssize_t m = readlinkat(fd, "", t2, sizeof t2);
            printf(" opath=%d link=%d same_target=%d", fd >= 0 ? 0 : errno,
                   fr == 0 && S_ISLNK(f.st_mode), n > 0 && n == m && !memcmp(t, t2, n));
            close(fd);
            /* By directory descriptor, as ls -l asks. */
            int dfd = open(dirs[d], O_RDONLY | O_DIRECTORY);
            rl = fstatat(dfd, names[i], &l, AT_SYMLINK_NOFOLLOW);
            printf(" at=%d link=%d", E(rl), rl == 0 && S_ISLNK(l.st_mode));
            errno = 0;
            int u = unlinkat(dfd, names[i], 0);
            printf(" unlinkat=%d", E(u));
            close(dfd);
            errno = 0;
            printf(" unlink=%d", E(unlink(p)));
            errno = 0;
            printf(" rename=%d", E(rename(p, "/proclink.x")) != 0);
            /* A trailing slash follows it. */
            char ps[140];
            snprintf(ps, sizeof ps, "%s/", p);
            errno = 0;
            rl = lstat(ps, &l);
            printf(" slash=%d dir=%d\n", E(rl), rl == 0 && S_ISDIR(l.st_mode));
        }
    }
    /* The program is still there. */
    printf("program=%d\n", access(argv[0], F_OK));
    (void)argc;
    return 0;
}
