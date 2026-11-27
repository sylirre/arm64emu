/* What a fork's child keeps of the native tier's per-thread scratch stacks
 * (monitor/sigsys.c): the one the handler making the fork stands on, the
 * forking thread's, which it takes over under its own tid at the same index;
 * the parent's other threads are not in the child, and their stacks are
 * given back (cng_sig_fork_child). The child used to keep a copy of every
 * one, its own under the parent's tid, and look up a fresh slot besides --
 * where the handler's note of the frame it returned the child to was not: a
 * tracer's attach carried by SIGSYS that landed on that frame took the
 * fork's svc behind it for a trap it had displaced, and made the fork again
 * (m18's forkafter, one run in eight on a slow device). That race cannot be
 * held open from outside; what the child keeps can be counted. A scratch
 * stack is a read-write anonymous mapping of 256 KiB between inaccessible
 * ones, and nothing else here is.
 *
 * Prints how many the parent holds -- the main thread and four others, each
 * past a trap (an open, which every filter of ours traps), so each holds a
 * slot -- and how many a fork the main thread makes keeps, then one a
 * thread that is not the main one makes. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define SCR_SZ (256UL * 1024)

static int count_stacks(void) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f)
        return -1;
    unsigned long pg = (unsigned long)sysconf(_SC_PAGESIZE);
    unsigned long plo = 0, phi = 0, lo, hi, ino;
    char perms[8] = "", pperms[8] = "", line[512];
    int n = 0, cand = 0; /* the previous line was a stack after a guard */
    while (fgets(line, sizeof line, f)) {
        char path[256] = "";
        if (sscanf(line, "%lx-%lx %7s %*s %*s %lu %255s", &lo, &hi, perms, &ino,
                   path) < 4)
            continue;
        /* A guard can be merged with a neighbour's: one inaccessible
         * mapping, a page or more. */
        if (cand && lo == phi && hi - lo >= pg && !strcmp(perms, "---p"))
            n++;
        /* Before 4.5 the kernel names a mapping some thread's SP is in
         * "[stack:TID]": the one the handler reading this stands on. */
        int anon = !ino && (!path[0] || !strncmp(path, "[stack", 6));
        cand = hi - lo == SCR_SZ && !strcmp(perms, "rw-p") && anon && plo &&
               lo == phi && phi - plo >= pg && !strcmp(pperms, "---p");
        plo = lo;
        phi = hi;
        strcpy(pperms, perms);
    }
    fclose(f);
    return n;
}

static void trap_once(void) {
    int fd = open("/", O_RDONLY);
    if (fd >= 0)
        close(fd);
}

static int fork_count(const char *what) {
    pid_t c = fork();
    if (c == 0) {
        printf("%s kept %d stack(s)\n", what, count_stacks());
        fflush(stdout);
        _exit(0);
    }
    int st;
    return c > 0 && waitpid(c, &st, 0) == c && WIFEXITED(st) ? 0 : 1;
}

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static int g_ready, g_go, g_done;

static void *thr(void *a) {
    int forker = a != 0;
    trap_once();
    pthread_mutex_lock(&g_mu);
    g_ready++;
    pthread_cond_broadcast(&g_cv);
    while (forker && !g_go)
        pthread_cond_wait(&g_cv, &g_mu);
    pthread_mutex_unlock(&g_mu);
    if (forker) {
        fork_count("a fork of another thread");
        pthread_mutex_lock(&g_mu);
        g_done = 1;
        pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_mu);
    }
    for (;;)
        pause();
    return 0;
}

int main(void) {
    setvbuf(stdout, 0, _IOLBF, 0);
    trap_once();
    pthread_t t;
    for (int i = 0; i < 4; i++)
        if (pthread_create(&t, 0, thr, i == 0 ? (void *)1 : 0) != 0)
            return 1;
    pthread_mutex_lock(&g_mu);
    while (g_ready < 4)
        pthread_cond_wait(&g_cv, &g_mu);
    pthread_mutex_unlock(&g_mu);
    printf("the parent holds %d stack(s)\n", count_stacks());
    fork_count("a fork of the main thread");
    pthread_mutex_lock(&g_mu);
    g_go = 1;
    pthread_cond_broadcast(&g_cv);
    while (!g_done)
        pthread_cond_wait(&g_cv, &g_mu);
    pthread_mutex_unlock(&g_mu);
    return 0;
}
