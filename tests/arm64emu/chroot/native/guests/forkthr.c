/* Threads after an exec that keeps the host's thread memory: the native tier's exec
 * keeps the address space and gives back what the outgoing program mapped
 * (execve.c, exec_sweep), and what the host's libc made for a thread of ours
 * is not the program's -- bionic's thread record on the stack we gave it, its
 * TLS, shadow call stack and alternate signal stack (monitor/thread.c). The
 * shapes in which an exec lands on top of such memory still in use:
 *
 *   - a thread that is not the main one execs (the leader takes the exec
 *     over, as the kernel's de_thread has it);
 *   - a fork child of a threaded program execs: the other threads are not in
 *     the child, but bionic's list of threads still names them;
 *   - a fork child made by a thread that is not the main one execs: that
 *     thread goes on as the process, on the TLS and the shadow call stack the
 *     libc gave it, and its record is on that list too.
 *
 * Each time the new program then makes threads, which is the libc's
 * pthread_create in the monitor: it runs on that TLS and shadow call stack, and
 * links the new thread into that list.
 *
 *   forkthr            every leg
 *   forkthr run TAG    (internal) the exec'd program: make threads, say TAG */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void *spin(void *a) {
    (void)a;
    for (int i = 0; i < 1000; i++)
        __asm__ volatile("" ::: "memory");
    return a;
}

static void *sleeper(void *a) {
    (void)a;
    for (;;)
        sleep(100);
    return 0;
}

/* The exec'd program: four threads, each made and joined twice. */
static int run(const char *tag) {
    for (int round = 0; round < 2; round++) {
        pthread_t t[4];
        for (int i = 0; i < 4; i++)
            if (pthread_create(&t[i], 0, spin, (void *)(long)i) != 0) {
                printf("%s: pthread_create failed\n", tag);
                return 1;
            }
        for (int i = 0; i < 4; i++) {
            void *r;
            pthread_join(t[i], &r);
            if ((long)r != i) {
                printf("%s: a thread returned the wrong value\n", tag);
                return 1;
            }
        }
    }
    printf("%s: the new program's threads ran\n", tag);
    return 0;
}

static char *g_self;

/* Leg 3's thread: a fork whose child execs, the exit status for main. */
static void *forker(void *a) {
    (void)a;
    pid_t p = fork();
    if (p == 0) {
        char *av[] = {g_self, "run", "a fork child of a thread's exec", 0};
        execv("/proc/self/exe", av);
        _exit(1);
    }
    int st;
    waitpid(p, &st, 0);
    return (void *)(long)st;
}

static void *execer(void *a) {
    (void)a;
    usleep(10000);
    char *av[] = {g_self, "run", "exec from a thread", 0};
    execv("/proc/self/exe", av);
    perror("execv");
    _exit(1);
}

int main(int argc, char **argv) {
    setvbuf(stdout, 0, _IONBF, 0);
    if (argc > 2 && !strcmp(argv[1], "run"))
        return run(argv[2]);
    g_self = argv[0];

    if (argc > 1 && !strcmp(argv[1], "thread")) {
        /* Leg 1, in a process of its own: a sleeping sibling, and the exec
         * from a second thread. */
        pthread_t s, e;
        pthread_create(&s, 0, sleeper, 0);
        pthread_create(&e, 0, execer, 0);
        pause();
        return 1;
    }

    /* Leg 1, run as a child so its exec ends only it. */
    pid_t p = fork();
    if (p == 0) {
        char *av[] = {argv[0], "thread", 0};
        execv("/proc/self/exe", av);
        _exit(1);
    }
    int st;
    waitpid(p, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st))
        printf("exec from a thread: %s %d\n",
               WIFSIGNALED(st) ? "killed by signal" : "exit status",
               WIFSIGNALED(st) ? WTERMSIG(st) : WEXITSTATUS(st));

    /* Leg 2: two live threads, then a fork whose child execs. */
    pthread_t s1, s2;
    pthread_create(&s1, 0, sleeper, 0);
    pthread_create(&s2, 0, sleeper, 0);
    usleep(10000);
    p = fork();
    if (p == 0) {
        char *av[] = {argv[0], "run", "a fork child's exec", 0};
        execv("/proc/self/exe", av);
        _exit(1);
    }
    waitpid(p, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st))
        printf("a fork child's exec: %s %d\n",
               WIFSIGNALED(st) ? "killed by signal" : "exit status",
               WIFSIGNALED(st) ? WTERMSIG(st) : WEXITSTATUS(st));

    /* Leg 3: the same fork, made by a third thread. */
    pthread_t f;
    void *r;
    pthread_create(&f, 0, forker, 0);
    pthread_join(f, &r);
    st = (int)(long)r;
    if (!WIFEXITED(st) || WEXITSTATUS(st))
        printf("a fork child of a thread's exec: %s %d\n",
               WIFSIGNALED(st) ? "killed by signal" : "exit status",
               WIFSIGNALED(st) ? WTERMSIG(st) : WEXITSTATUS(st));
    return 0;
}
