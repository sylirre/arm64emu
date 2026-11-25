/* /proc names spelled with a leading zero: procfs reads a numbered entry with
 * name_to_int(), which refuses one, so "/proc/0<pid>" and "/proc/self/fd/05"
 * do not exist, whatever process <pid> and descriptor 5 are. Every name is
 * asked the ways the emulator answers for itself: a synthesized file, a magic
 * link read and followed, an exec through an fd link. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void say(const char *what, long r) {
    printf("%s: %s\n", what, r < 0 ? strerror(errno) : "ok");
}

int main(void) {
    int low = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    int fd = fcntl(low, F_DUPFD_CLOEXEC, 20);   /* two digits: 020 is not 0..9 */
    int pid = getpid();
    char p[128], b[256];
    struct stat st;
    static const char *const pidfiles[] = {"status", "stat", "cmdline", "maps",
                                           "environ", "personality", "comm"};
    for (int zero = 0; zero < 2; zero++) {
        const char *z = zero ? "0" : "";
        for (unsigned i = 0; i < sizeof pidfiles / sizeof *pidfiles; i++) {
            snprintf(p, sizeof p, "/proc/%s%d/%s", z, pid, pidfiles[i]);
            int f = open(p, O_RDONLY | O_CLOEXEC);
            printf("%s", zero ? "0<pid> " : "<pid> ");
            say(pidfiles[i], f);
            if (f >= 0) close(f);
        }
        snprintf(p, sizeof p, "/proc/%s%d/exe", z, pid);
        say("exe readlink", readlink(p, b, sizeof b) > 0 ? 0 : -1);
        say("exe stat", stat(p, &st));
        snprintf(p, sizeof p, "/proc/%s%d/cwd", z, pid);
        say("cwd stat", stat(p, &st));
        snprintf(p, sizeof p, "/proc/%s%d", z, pid);
        say("directory", stat(p, &st));
        snprintf(p, sizeof p, "/proc/self/fd/%s%d", z, fd);
        say("fd stat", stat(p, &st));
        snprintf(p, sizeof p, "/dev/fd/%s%d", z, fd);
        say("/dev/fd stat", stat(p, &st));
        snprintf(p, sizeof p, "/proc/self/task/%s%d/status", z, pid);
        say("task status", access(p, F_OK));
    }
    char *av[] = {p, NULL}, *ev[] = {NULL};
    snprintf(p, sizeof p, "/proc/self/fd/0%d", fd);
    execve(p, av, ev);
    say("exec fd", -1);
    snprintf(p, sizeof p, "/proc/0%d/exe", pid);
    execve(p, av, ev);
    say("exec exe", -1);
    return 0;
}
