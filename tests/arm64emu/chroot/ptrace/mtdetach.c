/* Self-checking test: a group stop outlives its tracer's detach, and no
 * thread runs on between the trap it was detached in and the stop that
 * follows -- the kernel's answers, row by row.
 *
 * Both threads of a process are SEIZEd and trap into a group stop; the tracer
 * detaches them, the main thread first and then the other, and the other way
 * round. Detached, each thread stops again as it leaves its trap
 * (__ptrace_unlink re-arms its part in the stop), so the process stays
 * stopped -- its real parent is told -- until a SIGCONT, and the second
 * thread, which never makes a system call it could stop in but sched_yield,
 * moves its counter not once from its trap to the SIGCONT.
 *
 * The emulator hands a group stop over to the host as the last traced thread
 * leaves: a host SIGSTOP. Sent to the process, it was taken by the thread the
 * host picked -- the main one, parked in the emulator's stop -- and the last
 * thread to leave, the counting one, ran on in the guest until the main one
 * had made the stop. mtjobctl.c saw it one time in some hundreds, its counter
 * asleep 2 ms between steps; this one counts all the time.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static char got[4096];
static size_t gotn;
/* Every row goes here, to be compared with the kernel's at the end. */
static void out(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(got + gotn, sizeof got - gotn, fmt, ap);
    va_end(ap);
    if (n > 0 && gotn + (size_t)n < sizeof got) gotn += (size_t)n;
}
static void nap(int ms) { struct timespec t = { ms / 1000, (ms % 1000) * 1000000L }; while (nanosleep(&t, &t) && errno == EINTR) ; }
static volatile unsigned long *ctr;   /* shared page: the count, the counting tid */
static void *counter(void *a) { (void)a; ctr[1] = syscall(SYS_gettid); for (;;) { ctr[0]++; sched_yield(); } return NULL; }
int main(void) {
    ctr = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    for (int round = 0; round < 6; round++) {
        int other_first = round & 1;   /* 0: main detached first, the counter last */
        out("--- %s detached first\n", other_first ? "counter" : "main");
        ctr[0] = ctr[1] = 0;
        pid_t k = fork();
        if (k == 0) { pthread_t t; pthread_create(&t, NULL, counter, NULL); for (;;) nap(5); }
        while (!ctr[1]) nap(5);
        pid_t tid = (pid_t)ctr[1];
        ptrace(PTRACE_SEIZE, k, 0, 0);
        ptrace(PTRACE_SEIZE, tid, 0, 0);
        nap(20);
        kill(k, SIGSTOP);
        int s;
        pid_t w = waitpid(-1, &s, __WALL);
        out("%-24s stop sig=%d ev=%d\n", "sigstop", WIFSTOPPED(s) ? WSTOPSIG(s) : -1, s >> 16);
        ptrace(PTRACE_CONT, w, 0, SIGSTOP);
        int traps = 0;
        for (int i = 0; i < 2; i++) {
            w = waitpid(-1, &s, __WALL);
            traps += w > 0 && WIFSTOPPED(s) && WSTOPSIG(s) == SIGSTOP && (s >> 16) == PTRACE_EVENT_STOP;
        }
        out("%-24s %d\n", "group-stop traps", traps);
        unsigned long c0 = ctr[0];   /* both in their traps: frozen */
        if (other_first) {
            ptrace(PTRACE_DETACH, tid, 0, 0);
            ptrace(PTRACE_DETACH, k, 0, 0);
        } else {
            ptrace(PTRACE_DETACH, k, 0, 0);
            ptrace(PTRACE_DETACH, tid, 0, 0);
        }
        for (int i = 0; i < 200 && (w = waitpid(k, &s, WUNTRACED | WNOHANG)) == 0; i++)
            nap(10);
        out("%-24s %s\n", "after detach", w == k && WIFSTOPPED(s) ? "stopped (parent told)" : w == 0 ? "no report" : "?");
        nap(200);
        out("%-24s %s\n", "counter since the trap", ctr[0] == c0 ? "still" : "moving");
        kill(k, SIGCONT);
        nap(100);
        c0 = ctr[0]; nap(100);
        out("%-24s %s\n", "counter after SIGCONT", ctr[0] == c0 ? "still" : "moving");
        kill(k, SIGKILL);
        waitpid(k, &s, 0);
    }
    /* The kernel's answers, taken natively with this same program. */
    static const char one[][64] = {
        "sigstop                  stop sig=19 ev=0\n",
        "group-stop traps         2\n",
        "after detach             stopped (parent told)\n",
        "counter since the trap   still\n",
        "counter after SIGCONT    moving\n",
    };
    static char want[4096];
    size_t wn = 0;
    for (int round = 0; round < 6; round++) {
        wn += (size_t)snprintf(want + wn, sizeof want - wn, "--- %s detached first\n",
                               (round & 1) ? "counter" : "main");
        for (size_t i = 0; i < sizeof one / sizeof one[0]; i++)
            wn += (size_t)snprintf(want + wn, sizeof want - wn, "%s", one[i]);
    }
    if (strcmp(got, want) == 0) { printf("OK\n"); return 0; }
    const char *g = got, *x = want;
    while (*g && *g == *x) { g++; x++; }
    while (g > got && g[-1] != '\n') { g--; x--; }
    printf("FAIL:\n  got:  %.*s\n  want: %.*s\n", (int)strcspn(g, "\n"), g,
           (int)strcspn(x, "\n"), x);
    return 1;
}
