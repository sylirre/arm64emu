/* Is a page made executable the same whatever lies beside it?
 *
 * Under SELinux the kernel judges mprotect(PROT_EXEC) against the whole VMA the
 * range lies in, before splitting it, and a VMA that holds the caller's stack
 * pointer is "a stack being made executable" — which an Android app domain is
 * not granted. Adjacent anonymous mappings of equal permissions are one VMA, and
 * a thread's stack is one such mapping, so a page mapped beside a thread's stack
 * and then made RWX is refused, though nothing about the page itself is wrong.
 * The JVM does exactly this at startup (a one-page RWX probe issued by the
 * thread that runs the VM) and exits with "Failed to mark memory page as
 * executable". bionic never meets it because it names its stacks, which keeps
 * them out of any merge; musl and glibc guests do.
 *
 * The shape is made here directly, with no libc policy in the way: one anonymous
 * mapping, its first page the probe and the rest the stack a thread is started
 * on, so the page and the thread's stack pointer are in the same VMA by
 * construction rather than by where the allocator happened to put them.
 *
 * Prints one `name=value` line per case, 0 or -errno:
 *   rwx_main         a page of its own, from the main thread. Nothing of the
 *                    guest's is beside it; what can be is the monitor's stack,
 *                    when a trapped call is made from one that is not bounded;
 *   rwx_beside_stack the probe page of the shared VMA, RWX, from the thread;
 *   rx_beside_stack  the same, R+X;
 *   rwx_after        the first case again, after the thread has gone, so one
 *                    that wedged the mapping shows.
 * A host that denies anonymous executable memory altogether answers every one
 * of them with an error; the harness asks that question separately (--probe)
 * and does not run this there.
 * Exits 0 when the setup could be made, 2 when it could not.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#define STACK (256 * 1024)

static long pg;
static char *region; /* [probe page][thread stack] — one anonymous VMA */

static void *thread_fn(void *arg) {
    int prot = (int)(long)arg;
    return (void *)(long)(mprotect(region, (size_t)pg, prot) == 0 ? 0 : -errno);
}

/* The probe page's mprotect, made by a thread running on the rest of the VMA. */
static long beside_stack(int prot) {
    pthread_attr_t a;
    pthread_t t;
    void *ret = (void *)-1L;
    /* Fresh protections each time: an earlier case left the page executable. */
    if (mprotect(region, (size_t)pg, PROT_READ | PROT_WRITE) != 0)
        return -errno;
    if (pthread_attr_init(&a) ||
        pthread_attr_setstack(&a, region + pg, STACK) ||
        pthread_create(&t, &a, thread_fn, (void *)(long)prot))
        return -1000;
    pthread_join(t, &ret);
    return (long)ret;
}

static long main_page(void) {
    char *p = mmap(0, (size_t)pg, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return -errno;
    long r = mprotect(p, (size_t)pg, PROT_READ | PROT_WRITE | PROT_EXEC) == 0
                 ? 0
                 : -errno;
    munmap(p, (size_t)pg);
    return r;
}

int main(void) {
    pg = sysconf(_SC_PAGESIZE);
    region = mmap(0, (size_t)pg + STACK, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (region == MAP_FAILED) {
        perror("mmap");
        return 2;
    }
    printf("rwx_main=%ld\n", main_page());
    printf("rwx_beside_stack=%ld\n",
           beside_stack(PROT_READ | PROT_WRITE | PROT_EXEC));
    printf("rx_beside_stack=%ld\n", beside_stack(PROT_READ | PROT_EXEC));
    printf("rwx_after=%ld\n", main_page());
    return 0;
}
