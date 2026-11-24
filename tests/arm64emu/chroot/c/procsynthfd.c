/* A synthesized /proc file through its descriptor: fstat gives the file's own
 * attributes (S_IRUGO, 0400 for environ and auxv, root's for a file of no
 * process), and its fd link names the file, as d_path does -- not the
 * anonymous memory the emulator serves it from (0777, /memfd:...). Not
 * cmdline: qemu-user, the oracle on a foreign host, serves that one from a
 * memfd of its own and has the same two defects. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
    char own[64];
    snprintf(own, sizeof own, "/proc/%d/status", (int)getpid());
    const char *files[] = {"/proc/version", "/proc/loadavg", "/proc/uptime",
                           "/proc/self/status", "/proc/self/environ", own};
    for (unsigned i = 0; i < sizeof files / sizeof *files; i++) {
        struct stat by_name, by_fd;
        int fd = open(files[i], O_RDONLY);
        if (fd < 0 || stat(files[i], &by_name) || fstat(fd, &by_fd)) {
            printf("%s: unavailable\n", files[i]);
            if (fd >= 0) close(fd);
            continue;
        }
        char lnk[64], tgt[256];
        snprintf(lnk, sizeof lnk, "/proc/self/fd/%d", fd);
        ssize_t n = readlink(lnk, tgt, sizeof tgt - 1);
        tgt[n > 0 ? n : 0] = 0;
        const char *want = strncmp(files[i], "/proc/self/", 11) ? files[i] : NULL;
        char wbuf[64];
        if (!want) {
            snprintf(wbuf, sizeof wbuf, "/proc/%d/%s", (int)getpid(), files[i] + 11);
            want = wbuf;
        }
        printf("%s: mode=%o same_mode=%d same_owner=%d link_names_it=%d\n",
               i == 5 ? "/proc/<pid>/status" : files[i], by_fd.st_mode & 07777,
               by_fd.st_mode == by_name.st_mode,
               by_fd.st_uid == by_name.st_uid && by_fd.st_gid == by_name.st_gid,
               !strcmp(tgt, want));
        close(fd);
    }
    return 0;
}
