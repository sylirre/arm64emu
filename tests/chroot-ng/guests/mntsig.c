/* A mount table opened from a signal handler that interrupted the opening of
 * another (M11 regression, -R).
 *
 * The mount tables are emitted from a snapshot of the view held in one static
 * buffer, taken one reader at a time by thread id. On the -R tier the
 * dispatcher runs on the guest's own signal mask, so a handler delivered while
 * the thread was emitting a table — on the way out of any syscall the emission
 * makes — that opened a mount table itself waited for the snapshot its own
 * thread held, forever.
 *
 * A loop opens and reads /proc/self/mountinfo, and before each read arms a
 * one-shot timer whose handler reads it too. The delay is swept across the
 * time one read takes here, measured first, so some land inside the emission
 * whatever the host's speed; an interval timer instead starved the loop
 * wherever a read took longer than the interval (a TCG guest kernel), each
 * handler's return finding the next signal pending. Every read, inside the
 * handler or not, must be the table the first one read: a nested emission
 * that shared the snapshot would tear it. A hang is the bug itself. */
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define ROUNDS 1000

static char g_ref[8192];
static long g_ref_len;
static volatile long g_handled, g_bad;

static long slurp(char *buf, long sz) {
    int fd = open("/proc/self/mountinfo", O_RDONLY);
    if (fd < 0)
        return -1;
    long n = 0, r;
    while (n < sz && (r = read(fd, buf + n, (size_t)(sz - n))) > 0)
        n += r;
    close(fd);
    return n;
}

static int differs(const char *buf, long n) {
    return n != g_ref_len || memcmp(buf, g_ref, (size_t)n) != 0;
}

static void on_alarm(int sig) {
    (void)sig;
    char buf[sizeof g_ref];
    long n = slurp(buf, sizeof buf);
    if (differs(buf, n))
        g_bad++;
    g_handled++;
}

static long now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000L + t.tv_nsec / 1000;
}

static void arm(long us) {
    struct itimerval it;
    memset(&it, 0, sizeof it);
    it.it_value.tv_usec = us;
    setitimer(ITIMER_REAL, &it, 0);
}

int main(void) {
    g_ref_len = slurp(g_ref, sizeof g_ref);
    if (g_ref_len <= 0) {
        printf("mntsig: no /proc/self/mountinfo\n");
        return 1;
    }
    char buf[sizeof g_ref];
    long bad = 0, span = 0;
    for (int i = 0; i < 8; i++) {
        long t0 = now_us();
        bad += differs(buf, slurp(buf, sizeof buf));
        long d = now_us() - t0;
        if (!span || d < span)
            span = d;
    }
    if (span > 999999)
        span = 999999;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGALRM, &sa, 0);
    for (int i = 0; i < ROUNDS; i++) {
        arm(1 + span * (i % 64) / 64);
        bad += differs(buf, slurp(buf, sizeof buf));
    }
    arm(0);
    printf("mntsig: handled>0=%d bad=%ld\n", g_handled > 0, bad + g_bad);
    return 0;
}
