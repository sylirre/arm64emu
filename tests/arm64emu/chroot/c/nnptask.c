/* no_new_privs is a task's, not a process's (PFA_NO_NEW_PRIVS): a thread that
 * sets it leaves its siblings as they were, and a new task -- a thread, or a
 * fork child -- takes it from the task that made it. The emulator kept one
 * flag for the whole process. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

static int nnp(void) { return prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0); }

static void *reader(void *p) { *(int *)p = nnp(); return NULL; }

static int spawn_read(void) {
    int v = -1;
    pthread_t t;
    pthread_create(&t, NULL, reader, &v);
    pthread_join(t, NULL);
    return v;
}

static int fork_read(void) {
    pid_t k = fork();
    if (k == 0) _exit(nnp());
    int st = 0;
    waitpid(k, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static pthread_barrier_t b1, b2;
static int setter_after;

static void *setter(void *p) {
    (void)p;
    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    setter_after = nnp();
    pthread_barrier_wait(&b1);
    pthread_barrier_wait(&b2);
    return NULL;
}

int main(void) {
    printf("start=%d\n", nnp());
    pthread_barrier_init(&b1, NULL, 2);
    pthread_barrier_init(&b2, NULL, 2);
    pthread_t t;
    pthread_create(&t, NULL, setter, NULL);
    pthread_barrier_wait(&b1);
    printf("setter_after=%d\n", setter_after);
    printf("self_after_sibling_set=%d\n", nnp());
    printf("forked_by_unset=%d\n", fork_read());
    printf("spawned_by_unset=%d\n", spawn_read());
    pthread_barrier_wait(&b2);
    pthread_join(t, NULL);
    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    printf("self_after_set=%d forked=%d spawned=%d\n", nnp(), fork_read(), spawn_read());
    return 0;
}
