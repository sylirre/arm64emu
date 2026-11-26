/* Every way to name what an exec runs, and what an exec takes with it (M6).
 *
 * usage: execvar           run the cases, each in a child of its own
 *        execvar show ...  the program the cases run: its arguments and state
 *
 * Runs in a directory of its own making under /, whose files it writes itself:
 * scripts whose interpreter is this program by its guest path. Prints one line
 * per case: what the program that came out of the exec saw, or the errno, or the
 * signal. The answers are a kernel's.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void put(const char *p, const char *text, mode_t m) {
    unlink(p);
    int fd = open(p, O_CREAT | O_WRONLY | O_TRUNC, 0755);
    if (fd < 0 || write(fd, text, strlen(text)) < 0)
        exit(9);
    close(fd);
    chmod(p, m);
}

static int show(int argc, char **argv) {
    char e[256];
    printf("argv");
    for (int i = 0; i < argc; i++)
        printf(" %s", argv[i]);
    ssize_t n = readlink("/proc/self/exe", e, sizeof e - 1);
    e[n > 0 ? n : 0] = 0;
    printf(" exe=%s", e);
    const char *f = getenv("FDS");
    if (f) { /* "<kept> <closed>" */
        int a = atoi(f), b = atoi(strchr(f, ' ') + 1);
        printf(" kept=%d closed=%d", fcntl(a, F_GETFD) >= 0,
               fcntl(b, F_GETFD) < 0 && errno == EBADF);
    }
    if (getenv("TIMERS")) {
        int gone = 0, want = atoi(getenv("TIMERS"));
        for (int i = 0; i < want; i++) {
            struct itimerspec it;
            gone += syscall(SYS_timer_gettime, i, &it) < 0 && errno == EINVAL;
        }
        printf(" timers_gone=%d", gone == want);
    }
    if (getenv("MASK")) {
        unsigned long m = 0;
        syscall(SYS_rt_sigprocmask, SIG_BLOCK, 0, &m, 8);
        struct sigaction hup, usr;
        sigaction(SIGHUP, 0, &hup);
        sigaction(SIGUSR1, 0, &usr);
        printf(" blocked=%d ignored=%d handler_reset=%d", (int)((m >> (SIGUSR2 - 1)) & 1),
               hup.sa_handler == SIG_IGN, usr.sa_handler == SIG_DFL);
    }
    printf("\n");
    return 42;
}

/* Run `body` in a child: its line is the exec'd program's, or the errno. */
#define CASE(name, ...)                                                        \
    do {                                                                       \
        printf("%s: ", name);                                                  \
        fflush(stdout);                                                        \
        pid_t k_ = fork();                                                     \
        if (k_ == 0) {                                                         \
            errno = 0;                                                         \
            __VA_ARGS__;                                                       \
            printf("errno %d\n", errno);                                       \
            fflush(stdout);                                                    \
            _exit(1);                                                          \
        }                                                                      \
        int st_;                                                               \
        waitpid(k_, &st_, 0);                                                  \
        if (WIFSIGNALED(st_))                                                  \
            printf("signal %d\n", WTERMSIG(st_));                              \
    } while (0)

static void on_usr1(int s) { (void)s; }

int main(int argc, char **argv) {
    const char *b = strrchr(argv[0], '/');
    b = b ? b + 1 : argv[0];
    if (argc > 1 && (!strcmp(argv[1], "show") || !strcmp(argv[1], "sheb")))
        return show(argc, argv);
    setvbuf(stdout, 0, _IOLBF, 0);
    const char *self = argv[0]; /* the guest path the harness gave */
    char line[512], p[64];
    mkdir("/xv", 0755);
    snprintf(line, sizeof line, "#!%s sheb\n", self);
    put("/xv/s1", line, 0755);
    for (int i = 2; i <= 8; i++) { /* s2 runs s1, ... */
        snprintf(line, sizeof line, "#!/xv/s%d\n", i - 1);
        snprintf(p, sizeof p, "/xv/s%d", i);
        put(p, line, 0755);
    }
    snprintf(line, sizeof line, "#!%s sheb\n", self);
    put("/xv/noexec", line, 0644);
    put("/xv/empty", "", 0755);
    unlink("/xv/link");
    if (symlink(self, "/xv/link") != 0)
        return 9;
    char *ev[] = {0};
    char *as[] = {"s", "AA", 0}, *aw[] = {"w", "show", "BB", 0};

    CASE("plain", execve(self, aw, ev));
    CASE("symlink", execve("/xv/link", aw, ev));
    CASE("shebang", execve("/xv/s1", as, ev));
    CASE("shebang 4 deep", execve("/xv/s4", as, ev));
    /* How deep a chain may go is not one number across kernels (5.15 runs a
     * level more than 6.x does); every one runs four and refuses eight. */
    CASE("shebang 8 deep", execve("/xv/s8", as, ev));
    CASE("a script that is not executable", execve("/xv/noexec", as, ev));
    CASE("empty file", execve("/xv/empty", as, ev));
    CASE("missing", execve("/xv/none", as, ev));
    CASE("a directory", execve("/xv", as, ev));
    CASE("relative", { if (chdir("/xv") == 0) execve("link", aw, ev); });

    /* Through a descriptor. */
    CASE("fd path, program", {
        int fd = open(self, O_RDONLY);
        snprintf(p, sizeof p, "/proc/self/fd/%d", fd);
        execve(p, aw, ev);
    });
    CASE("fd path, script", {
        int fd = fcntl(open("/xv/s1", O_RDONLY), F_DUPFD, 77);
        snprintf(p, sizeof p, "/proc/self/fd/%d", fd);
        execve(p, as, ev);
    });
    CASE("fd path, no descriptor", execve("/proc/self/fd/4294967299", as, ev));
    CASE("O_PATH fd of an unreadable program", {
        char *copy = "/xv/unreadable";
        int in = open(self, O_RDONLY), out = open(copy, O_CREAT | O_WRONLY | O_TRUNC, 0755);
        char buf[65536];
        ssize_t n;
        while ((n = read(in, buf, sizeof buf)) > 0)
            if (write(out, buf, (size_t)n) != n)
                _exit(9);
        close(out);
        out = open(copy, O_RDONLY | O_CLOEXEC);
        fchmod(out, 0111);
        snprintf(p, sizeof p, "/proc/self/fd/%d", out);
        execve(p, aw, ev);
    });

    /* execveat. */
#define XAT(d, n, a, f) syscall(SYS_execveat, d, n, a, ev, f)
    CASE("execveat relative", { int d = open("/xv", O_RDONLY | O_DIRECTORY);
                                XAT(d, "link", aw, 0); });
    CASE("execveat above the root", { int d = open("/xv", O_RDONLY | O_DIRECTORY);
                                      XAT(d, "../../../../xv/link", aw, 0); });
    CASE("execveat AT_EMPTY_PATH", { int d = open(self, O_RDONLY); XAT(d, "", aw, AT_EMPTY_PATH); });
    CASE("execveat nofollow, a link", XAT(AT_FDCWD, "/xv/link", aw, AT_SYMLINK_NOFOLLOW));
    CASE("execveat nofollow, a file", XAT(AT_FDCWD, self, aw, AT_SYMLINK_NOFOLLOW));
    CASE("execveat unknown flag", XAT(AT_FDCWD, self, aw, 0x8000));
    CASE("execveat empty name", XAT(AT_FDCWD, "", aw, 0));

    /* What goes with the image and what stays. */
    CASE("descriptors", {
        char f[32], *ef[] = {f, 0};
        int a = fcntl(0, F_DUPFD, 50), c = fcntl(0, F_DUPFD_CLOEXEC, 60);
        snprintf(f, sizeof f, "FDS=%d %d", a, c);
        execve(self, aw, ef);
    });
    CASE("timers", {
        char *ef[] = {"TIMERS=100", 0};
        for (int i = 0; i < 100; i++) {
            struct sigevent se = {.sigev_notify = SIGEV_NONE};
            timer_t t;
            if (timer_create(CLOCK_MONOTONIC, &se, &t) != 0)
                _exit(8);
        }
        execve(self, aw, ef);
    });
    CASE("signals", {
        char *ef[] = {"MASK=1", 0};
        sigset_t s;
        sigemptyset(&s);
        sigaddset(&s, SIGUSR2);
        sigprocmask(SIG_BLOCK, &s, 0);
        signal(SIGHUP, SIG_IGN);
        signal(SIGUSR1, on_usr1);
        execve(self, aw, ef);
    });
    (void)b;
    return 0;
}
