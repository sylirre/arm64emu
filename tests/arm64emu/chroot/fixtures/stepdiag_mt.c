/* The step diagnostics with guest threads and processes (run_tests.sh, "the
 * step diagnostics"). spin() is a loop of two instructions that each retire
 * exactly ITER times a call, far hotter than anything else here, so the
 * profile's top counts are known:
 *
 *   threads T ITER   T threads and the main one spin, then print "done":
 *                    (T + 1) * ITER, counted atomically across them; with
 *                    --max-insn, the first thread to reach it ends the
 *                    process (one report, SIGKILL, no "done")
 *   exec ITER        spin, fork; the child spins and execs "threads 0 ITER";
 *                    the parent waits for it. Each of the four programs --
 *                    the parent, the child before and after its execve --
 *                    is profiled on its own: ITER each, never 2 * ITER,
 *                    which a profile inherited across the fork or kept
 *                    across the execve would show (the same binary, so the
 *                    same PCs). */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned long iter;

/* Out of line, so that every caller runs the same two PCs. */
__attribute__((noinline)) static void *spin(void *arg) {
    unsigned long n = iter;
    __asm__ volatile("1: subs %0, %0, #1\n\tb.ne 1b" : "+r"(n) : : "cc");
    return arg;
}

int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "threads")) {
        int t = atoi(argv[2]);
        pthread_t th[16];
        iter = strtoul(argv[3], NULL, 0);
        if (t < 0 || t > 16 || !iter) return 2;
        for (int i = 0; i < t; i++) pthread_create(&th[i], NULL, spin, NULL);
        spin(NULL);
        for (int i = 0; i < t; i++) pthread_join(th[i], NULL);
        puts("done");
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "exec")) {
        iter = strtoul(argv[2], NULL, 0);
        if (!iter) return 2;
        spin(NULL);
        fflush(stdout);
        pid_t p = fork();
        if (p == 0) {
            spin(NULL);
            execl(argv[0], argv[0], "threads", "0", argv[2], (char *)NULL);
            _exit(127);
        }
        int st;
        if (p < 0 || waitpid(p, &st, 0) != p) return 1;
        printf("child %d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
        return 0;
    }
    return 2;
}
