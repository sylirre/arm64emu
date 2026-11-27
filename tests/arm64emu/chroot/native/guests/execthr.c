/* Repeated execs from a threaded program: each generation starts four threads
 * that sleep, then execs itself, and an exec ends them (de_thread). A kernel's
 * exec throws the whole address space away, so the last generation has what
 * the first had; the native tier's keeps the address space, and has to give back
 * every thread it ended -- the guest's own stacks, and its own for each: the
 * pthread's stack and alternate stack, the scratch stack (monitor/thread.c).
 * What is counted is the memory that can be reached, every mapping but a
 * PROT_NONE one: a bionic guest's linker leaves gaps of that kind around the
 * libraries it loads, of a random size each time, tens of MiB between one
 * generation and the next; what a leak would leave is all read-write.
 *
 *   execthr           the first generation
 *   execthr N KB      (internal) generation N; KB the first's mapped kB */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define GENS 6

static unsigned long mapped_kb(void) {
    FILE *f = fopen("/proc/self/maps", "r");
    char line[512], perm[8];
    unsigned long kb = 0;
    while (f && fgets(line, sizeof line, f)) {
        unsigned long lo, hi;
        if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perm) == 3 &&
            strncmp(perm, "---", 3) != 0)
            kb += (hi - lo) >> 10;
    }
    if (f)
        fclose(f);
    return kb;
}

static void *sleeper(void *a) {
    (void)a;
    for (;;)
        sleep(100);
    return 0;
}

int main(int argc, char **argv) {
    int gen = argc > 1 ? atoi(argv[1]) : 0;
    unsigned long base = argc > 2 ? strtoul(argv[2], 0, 10) : 0;
    unsigned long now = mapped_kb();
    if (gen == 1)
        base = now;
    if (gen == GENS) {
        long grew = (long)now - (long)base;
        printf("after %d execs from four threads each, the address space %s\n",
               GENS - 1, grew < 1024 ? "did not grow" : "grew");
        if (grew >= 1024)
            fprintf(stderr, "grew by %ld kB\n", grew);
        return 0;
    }
    for (int i = 0; i < 4; i++) {
        pthread_t t;
        pthread_create(&t, 0, sleeper, 0);
    }
    usleep(20000);
    char g[16], b[32];
    snprintf(g, sizeof g, "%d", gen + 1);
    snprintf(b, sizeof b, "%lu", base);
    char *av[] = {argv[0], g, b, 0};
    execv("/proc/self/exe", av);
    perror("execv");
    return 1;
}
