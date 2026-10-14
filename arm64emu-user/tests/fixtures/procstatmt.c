/* /proc/self/stat read by several threads at once, self-checking.
 *
 * Every guest thread is a host thread of the emulator, and the synthesized
 * stat line (src/sys_procfs.c, put_pidstat) was cut into fields with strtok(3),
 * whose position glibc keeps once per process: two threads building the line
 * at the same time tokenized each other's, and about one read in five here
 * came back with fields missing, repeated or run together. Each read must be
 * one whole line -- the reader's pid first, a state letter after the comm, and
 * as many fields as a read made before any other thread existed. */
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define THREADS 4
#define READS   1000

static int pid, want, bad;

/* The number of fields in one read of the file, or -1 for a malformed line. */
static int fields(void) {
    char buf[2048];
    int fd = open("/proc/self/stat", O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = 0;
    char *rp = strrchr(buf, ')');
    if (atoi(buf) != pid || !rp || rp[1] != ' ' || !rp[2] || !strchr("RSDZTtXIPKWx", rp[2]))
        return -1;
    int f = 2;
    for (char *p = rp + 1; *p && *p != '\n'; p++)
        if (*p == ' ') f++;
    return f;
}

static void *run(void *arg) {
    (void)arg;
    for (int i = 0; i < READS; i++)
        if (fields() != want) __atomic_add_fetch(&bad, 1, __ATOMIC_RELAXED);
    return NULL;
}

int main(void) {
    pid = getpid();
    want = fields();
    printf("single=%s\n", want > 0 ? "ok" : "malformed");
    pthread_t t[THREADS];
    for (int i = 0; i < THREADS; i++) pthread_create(&t[i], NULL, run, NULL);
    for (int i = 0; i < THREADS; i++) pthread_join(t[i], NULL);
    printf("concurrent bad=%d\n", bad);
    printf("done\n");
    return 0;
}
