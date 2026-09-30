/* Self-checking: the tracer is a thread, not a process. The thread that
 * SEIZEs a task is its tracer: another thread of the same process is told
 * ESRCH for a request (ptrace_check_attach), though it may wait for the
 * tracee's stops (do_wait walks every thread's tracees); TracerPid names the
 * tracing thread; and when that thread exits, the tracee is released
 * (exit_ptrace) while its process lives on -- untraced, and free to be traced
 * again. The emulator kept the tracer per process: any thread's request
 * worked, TracerPid named the pid, and a tracer thread's exit released
 * nothing. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef PTRACE_SEIZE
#define PTRACE_SEIZE 0x4206
#endif
#ifndef PTRACE_INTERRUPT
#define PTRACE_INTERRUPT 0x4207
#endif
#ifndef __WALL
#define __WALL 0x40000000
#endif

static pid_t kid;
static int to_b[2], to_main[2];

static void nap(int ms) {
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&t, &t) && errno == EINTR) ;
}
static void say(int fd, char c) { if (write(fd, &c, 1) != 1) _exit(3); }
static char hear(int fd) { char c = 0; if (read(fd, &c, 1) != 1) _exit(3); return c; }

static int tracer_pid(pid_t p) {
    char path[64], line[256];
    snprintf(path, sizeof path, "/proc/%d/status", (int)p);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int v = -1;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "TracerPid: %d", &v) == 1) break;
    fclose(f);
    return v;
}

static volatile int b_tid, b_fail;
static void *thread_b(void *a) {
    (void)a;
    b_tid = (int)syscall(SYS_gettid);
    if (ptrace(PTRACE_SEIZE, kid, 0, 0)) b_fail = 1;
    say(to_main[1], 's');                  /* seized */
    hear(to_b[0]);                          /* main has asked, and failed */
    if (ptrace(PTRACE_INTERRUPT, kid, 0, 0)) b_fail = 2;
    int st;
    if (waitpid(kid, &st, __WALL) != kid || !WIFSTOPPED(st)) b_fail = 3;
    say(to_main[1], 't');                  /* in a trap */
    hear(to_b[0]);                          /* main has tried to resume it */
    if (ptrace(PTRACE_CONT, kid, 0, 0)) b_fail = 4;
    if (ptrace(PTRACE_INTERRUPT, kid, 0, 0)) b_fail = 5;
    say(to_main[1], 'i');                  /* a stop for main to wait for */
    hear(to_b[0]);
    if (ptrace(PTRACE_CONT, kid, 0, 0)) b_fail = 6;
    return NULL;                            /* ...and exits, its tracee running */
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (pipe(to_b) || pipe(to_main)) return 1;
    kid = fork();
    if (kid == 0) { for (;;) pause(); }

    pthread_t b;
    pthread_create(&b, NULL, thread_b, NULL);
    hear(to_main[0]);
    if (b_fail) { printf("FAIL: seize from thread (%d)\n", b_fail); return 1; }
    if (tracer_pid(kid) != b_tid) {
        printf("FAIL: TracerPid %d, want the thread %d\n", tracer_pid(kid), b_tid);
        return 1;
    }
    errno = 0;
    if (ptrace(PTRACE_INTERRUPT, kid, 0, 0) != -1 || errno != ESRCH) {
        printf("FAIL: interrupt from another thread: errno %d\n", errno);
        return 1;
    }
    say(to_b[1], 'g');
    hear(to_main[0]);
    if (b_fail) { printf("FAIL: tracer thread (%d)\n", b_fail); return 1; }
    errno = 0;
    if (ptrace(PTRACE_CONT, kid, 0, 0) != -1 || errno != ESRCH) {
        printf("FAIL: cont from another thread: errno %d\n", errno);
        return 1;
    }
    say(to_b[1], 'g');
    hear(to_main[0]);
    /* The wait is the whole thread group's. */
    int st;
    if (waitpid(kid, &st, __WALL) != kid || !WIFSTOPPED(st) || (st >> 16) == 0) {
        printf("FAIL: another thread's wait: %#x\n", st);
        return 1;
    }
    say(to_b[1], 'g');
    pthread_join(b, NULL);
    if (b_fail) { printf("FAIL: tracer thread (%d)\n", b_fail); return 1; }

    /* The tracer thread is gone: the tracee is nobody's. */
    int tp = -1;
    for (int i = 0; i < 200 && (tp = tracer_pid(kid)) != 0; i++) nap(10);
    if (tp != 0) { printf("FAIL: TracerPid %d after the tracer thread\n", tp); return 1; }
    errno = 0;
    if (ptrace(PTRACE_INTERRUPT, kid, 0, 0) != -1 || errno != ESRCH) {
        printf("FAIL: still traced after the tracer thread: errno %d\n", errno);
        return 1;
    }
    nap(100);
    if (ptrace(PTRACE_SEIZE, kid, 0, 0)) { printf("FAIL: seize again: %d\n", errno); return 1; }
    if (tracer_pid(kid) != getpid()) {
        printf("FAIL: TracerPid %d, want %d\n", tracer_pid(kid), (int)getpid());
        return 1;
    }
    kill(kid, SIGKILL);
    waitpid(kid, &st, __WALL);
    printf("OK\n");
    return 0;
}
