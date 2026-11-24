/* semctl's refusals in the kernel's order, and SEM_UNDO at a SIGKILL.
 *
 * semctl_setval judges the value (ERANGE) before it looks the set up, then
 * semnum (EINVAL), then write access; semctl_main asks for read access before
 * it looks at semnum. The emulator's broker looked the set up and checked
 * semnum first for both. And a holder killed with an undo row: exit_sem runs
 * in do_exit, so the parent that reaps it reads the value back with the
 * adjustment made -- the broker applied it on a one-second tick. */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/wait.h>
#include <unistd.h>

union semun { int val; struct semid_ds *buf; unsigned short *array; };

static const char *en(long r) {
    static char b[16];
    if (r >= 0) { snprintf(b, sizeof b, "%ld", r); return b; }
    return errno == EINVAL ? "EINVAL" : errno == ERANGE ? "ERANGE" :
           errno == EACCES ? "EACCES" : errno == EPERM ? "EPERM" : "other";
}

int main(void) {
    int id = semget(IPC_PRIVATE, 2, 0600);
    if (id < 0) { printf("semget: %s\n", en(-1)); return 0; }
    union semun a = {.val = 40000};
    printf("setval range beats bad id: %s\n", en(semctl(0x7ffffff, 0, SETVAL, a)));
    printf("setval range beats bad semnum: %s\n", en(semctl(id, 9, SETVAL, a)));
    a.val = 1;
    printf("setval bad semnum: %s\n", en(semctl(id, 9, SETVAL, a)));
    printf("getval bad semnum: %s\n", en(semctl(id, 9, GETVAL)));
    /* Read access taken away: the owner can still change the mode back. */
    struct semid_ds ds;
    union semun su = {.buf = &ds};
    semctl(id, 0, IPC_STAT, su);
    ds.sem_perm.mode = 0200;
    semctl(id, 0, IPC_SET, su);
    if (geteuid() != 0) {
        printf("getval access beats semnum: %s\n", en(semctl(id, 9, GETVAL)));
        printf("getncnt access beats semnum: %s\n", en(semctl(id, 9, GETNCNT)));
        a.val = 40000;
        printf("setval range beats no access: %s\n", en(semctl(id, 0, SETVAL, a)));
    }
    ds.sem_perm.mode = 0600;
    semctl(id, 0, IPC_SET, su);

    /* SEM_UNDO at a SIGKILL, read straight after the reap. */
    a.val = 2;
    semctl(id, 1, SETVAL, a);
    int p[2];
    if (pipe(p)) return 1;
    pid_t k = fork();
    if (k == 0) {
        struct sembuf op = {1, -2, SEM_UNDO};
        semop(id, &op, 1);
        if (write(p[1], "x", 1) != 1) _exit(1);
        pause();
        _exit(0);
    }
    char c;
    if (read(p[0], &c, 1) != 1) return 1;
    printf("while held: %d\n", semctl(id, 1, GETVAL));
    kill(k, SIGKILL);
    waitpid(k, NULL, 0);
    printf("after kill: %d\n", semctl(id, 1, GETVAL));
    semctl(id, 0, IPC_RMID);
    return 0;
}
