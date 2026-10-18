/* First links of one file, made at once (M10, l2s).
 *
 * The emulation's first link of a file renames it into the store and leaves a
 * symlink to it in its place. Made unlocked, two of them at once both took
 * the file for one of its own, and the later renamed the earlier's symlink
 * over the data: the name became a link to itself, the contents gone. Or a
 * linker found the symlink, or no name at all between the rename and the
 * symlink, where it had seen the file, and failed. A reader that stats the
 * name meanwhile saw it missing too — busybox `ln` is one, it stats its
 * source first — where the kernel never shows a name being linked as gone.
 *
 * Each round makes DIR/a afresh, then releases LINKERS children at once, each
 * linking a to a name of its own, and one reader that stats and reads a until
 * they are done. With real hardlinks every counter is 0.
 *
 *   l2srace DIR ROUNDS
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define LINKERS 8
#define PAYLOAD "payload\n"

static int put(const char *p, const char *s) {
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    int ok = write(fd, s, strlen(s)) == (ssize_t)strlen(s);
    close(fd);
    return ok ? 0 : -1;
}

/* 1 if p reads exactly PAYLOAD. */
static int intact(const char *p) {
    char b[64];
    int fd = open(p, O_RDONLY);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, b, sizeof b);
    close(fd);
    return n == (ssize_t)strlen(PAYLOAD) && !memcmp(b, PAYLOAD, (size_t)n);
}

int main(int argc, char **argv) {
    if (argc < 3 || chdir(argv[1]) != 0) {
        fprintf(stderr, "usage: l2srace DIR ROUNDS\n");
        return 2;
    }
    int rounds = atoi(argv[2]);
    volatile int *done = mmap(0, 4096, PROT_READ | PROT_WRITE,
                              MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (done == MAP_FAILED)
        return 2;
    int linkfail = 0, miss = 0, lost = 0, nlinkbad = 0, inobad = 0;
    char nm[16];
    for (int r = 0; r < rounds; r++) {
        unlink("a");
        for (int i = 0; i < LINKERS; i++) {
            snprintf(nm, sizeof nm, "l%d", i);
            unlink(nm);
        }
        if (put("a", PAYLOAD) != 0)
            return 2;
        *done = 0;
        int go[2];
        if (pipe(go) != 0)
            return 2;
        pid_t kids[LINKERS], rd;
        for (int i = 0; i < LINKERS; i++) {
            kids[i] = fork();
            if (kids[i] == 0) {
                char c;
                close(go[1]);
                if (read(go[0], &c, 1) < 0) /* released by the close below */
                    _exit(2);
                snprintf(nm, sizeof nm, "l%d", i);
                _exit(link("a", nm) == 0 ? 0 : 1);
            }
        }
        rd = fork();
        if (rd == 0) {
            char c;
            struct stat st;
            int m = 0;
            close(go[1]);
            if (read(go[0], &c, 1) < 0)
                _exit(255);
            while (!*done)
                if ((stat("a", &st) != 0 || !intact("a")) && m < 255)
                    m++;
            _exit(m);
        }
        close(go[0]);
        close(go[1]);
        int ws;
        for (int i = 0; i < LINKERS; i++)
            if (waitpid(kids[i], &ws, 0) != kids[i] || !WIFEXITED(ws) ||
                WEXITSTATUS(ws))
                linkfail++;
        *done = 1;
        if (waitpid(rd, &ws, 0) == rd && WIFEXITED(ws))
            miss += WEXITSTATUS(ws);
        struct stat sa, sl;
        if (!intact("a") || stat("a", &sa) != 0) {
            lost++;
            continue;
        }
        if (sa.st_nlink != LINKERS + 1)
            nlinkbad++;
        for (int i = 0; i < LINKERS; i++) {
            snprintf(nm, sizeof nm, "l%d", i);
            if (stat(nm, &sl) != 0 || sl.st_ino != sa.st_ino || !intact(nm)) {
                inobad++;
                break;
            }
        }
    }
    printf("linkfail=%d miss=%d lost=%d nlinkbad=%d inobad=%d\n", linkfail,
           miss, lost, nlinkbad, inobad);
    return 0;
}
