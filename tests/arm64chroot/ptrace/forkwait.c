/* Self-checking: a followed fork's child is its tracer's tracee by the time
 * the tracer hears of the fork. gdb waits for a new child's first stop by the
 * pid PTRACE_GETEVENTMSG gave it (waitpid(pid, __WALL)); the kernel attaches
 * the child at clone time, so that wait finds it. The emulator's child claimed
 * its registry link only once it ran, and a tracer quicker than it was told
 * ECHILD -- the pid was nobody's tracee yet, and no child of the tracer's.
 * Many rounds, ATTACH- and SEIZE-flavored, since it is a race. */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PTRACE_SEIZE
#define PTRACE_SEIZE 0x4206
#endif
#ifndef PTRACE_EVENT_STOP
#define PTRACE_EVENT_STOP 128
#endif
#ifndef __WALL
#define __WALL 0x40000000
#endif

static int round_(int n, int seize) {
    int go[2];
    if (pipe(go)) return 1;
    pid_t a = fork();
    if (a == 0) {
        char b;
        if (read(go[0], &b, 1) != 1) _exit(2);
        pid_t g = fork();
        if (g == 0) _exit(9);
        int st;
        waitpid(g, &st, 0);
        _exit(7);
    }
    int st;
    if (seize) {
        if (ptrace(PTRACE_SEIZE, a, 0, (void *)PTRACE_O_TRACEFORK)) return printf("FAIL: seize\n"), 1;
    } else {
        if (ptrace(PTRACE_ATTACH, a, 0, 0)) return printf("FAIL: attach\n"), 1;
        if (waitpid(a, &st, __WALL) != a || !WIFSTOPPED(st)) return printf("FAIL: attach stop\n"), 1;
        ptrace(PTRACE_SETOPTIONS, a, 0, (void *)PTRACE_O_TRACEFORK);
        ptrace(PTRACE_CONT, a, 0, 0);
    }
    if (write(go[1], "g", 1) != 1) return 1;
    if (waitpid(a, &st, __WALL) != a ||
        (st >> 8) != (SIGTRAP | (PTRACE_EVENT_FORK << 8))) {
        printf("FAIL round %d: fork event %#x\n", n, st);
        return 1;
    }
    unsigned long msg = 0;
    ptrace(PTRACE_GETEVENTMSG, a, 0, &msg);
    pid_t g = (pid_t)msg;
    /* At once, by its pid, as gdb does. */
    pid_t w = waitpid(g, &st, __WALL);
    if (w != g) {
        printf("FAIL round %d: wait for the new child: %d errno %d\n", n, (int)w, errno);
        return 1;
    }
    int want = seize ? (SIGTRAP | (PTRACE_EVENT_STOP << 8)) : SIGSTOP;
    if (!WIFSTOPPED(st) || (st >> 8) != want) {
        printf("FAIL round %d: its first stop %#x\n", n, st);
        return 1;
    }
    ptrace(PTRACE_CONT, g, 0, 0);
    ptrace(PTRACE_CONT, a, 0, 0);
    int ga = 0, gg = 0;
    while (!ga || !gg) {
        w = waitpid(-1, &st, __WALL);
        if (w < 0) break;
        if (WIFEXITED(st) || WIFSIGNALED(st)) {
            if (w == a) ga = 1;
            if (w == g) gg = 1;
            continue;
        }
        ptrace(PTRACE_CONT, w, 0, 0);
    }
    close(go[0]); close(go[1]);
    if (!ga || !gg) { printf("FAIL round %d: exits %d %d\n", n, ga, gg); return 1; }
    return 0;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    for (int i = 0; i < 20; i++)
        if (round_(i, i & 1)) return 1;
    printf("OK\n");
    return 0;
}
