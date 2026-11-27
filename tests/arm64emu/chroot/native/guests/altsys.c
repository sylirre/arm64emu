/* SIGSYS sent to a process spinning on trapped calls with an alternate signal
 * stack of its own, registered as bionic and Go register theirs, without
 * SS_AUTODISARM. The native tier's handler leaves that stack for one of its own and
 * disarms it from there (monitor/sigsys.c, scr_run); a SIGSYS sent from
 * elsewhere -- a tracer's kick, a sibling's de_thread request, a kill(2) --
 * that came in between was put at the alternate stack's top, over whatever
 * was there:
 *
 *   on   the calls are made from a signal handler running on that stack, and
 *        the frame went over the handler's own (bionic's crash handler died
 *        "stack corruption detected" so, when crash_dump attached);
 *   off  the calls are made from the main stack, and the frame went over the
 *        handler's, the trap it was serving: it resumed in the middle of our
 *        own code.
 *
 * The process counts its calls, those answered wrongly and any damage to a
 * pattern in its frame, while another sends it SIGSYS every few hundred
 * microseconds, which it ignores: the kernel discards each as it is sent, and
 * under the native tier each still comes to the handler of ours that owns the real
 * disposition, and is dropped there. It must still be running after them,
 * everything answered and kept, as it is under the kernel. The window is a
 * few instructions of each trap, which the milestone widens with
 * ARM64EMU_ALTSTACK_DELAY for these to be sent into.
 *
 * Paced, not a flood: SIGSYS is never blocked under the native tier, so each one a
 * handler of ours is still running for is delivered on top of it, and a flood
 * nests them deep enough to overflow the alternate stack whatever the window
 * does; and a handler of the program's own would be re-entered the same way.
 *
 *   altsys     both legs, in a directory where it may make altsys.d */
#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define SENDS 2000

/* calls made, answered wrongly, pattern damage */
static volatile unsigned long *sh;
static const char *dir = "altsys.d";

static void spin(void) {
    volatile unsigned char pat[2048];
    for (unsigned i = 0; i < sizeof pat; i++)
        pat[i] = (unsigned char)(i * 7 + 1);
    for (;;) {
        if (syscall(SYS_mkdirat, AT_FDCWD, dir, 0700) != 0)
            sh[1]++;
        if (syscall(SYS_unlinkat, AT_FDCWD, dir, AT_REMOVEDIR) != 0)
            sh[1]++;
        for (unsigned i = 0; i < sizeof pat; i++)
            if (pat[i] != (unsigned char)(i * 7 + 1)) {
                sh[2]++;
                pat[i] = (unsigned char)(i * 7 + 1);
            }
        sh[0]++;
    }
}

static void on_usr1(int s) {
    (void)s;
    spin();
}

static void leg(const char *name, int on) {
    sh[0] = sh[1] = sh[2] = 0;
    rmdir(dir);
    pid_t pid = fork();
    if (pid == 0) {
        static char alt[65536] __attribute__((aligned(16)));
        stack_t ss = {.ss_sp = alt, .ss_size = sizeof alt, .ss_flags = 0};
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = SIG_IGN;
        if (sigaltstack(&ss, 0) != 0 || sigaction(SIGSYS, &sa, 0) != 0)
            _exit(3);
        if (!on)
            spin();
        sa.sa_handler = on_usr1;
        sa.sa_flags = SA_ONSTACK;
        if (sigaction(SIGUSR1, &sa, 0) != 0)
            _exit(3);
        raise(SIGUSR1);
        _exit(4);
    }
    for (int k = 0; k < 20000 && sh[0] < 100; k++)
        usleep(1000);
    for (int i = 0; i < SENDS; i++) {
        kill(pid, SIGSYS);
        usleep(200);
    }
    /* Running on after it: ten more rounds, in at most twenty seconds. */
    unsigned long seen = sh[0];
    for (int k = 0; k < 20000 && sh[0] < seen + 10; k++)
        usleep(1000);
    int alive = sh[0] >= seen + 10;
    int st = 0;
    kill(pid, SIGKILL);
    waitpid(pid, &st, 0);
    printf("%s: ran on after the SIGSYS: %s\n", name, alive ? "yes" : "no");
    printf("%s: every call answered as it should: %s\n", name,
           sh[1] ? "no" : "yes");
    printf("%s: its frame kept: %s\n", name, sh[2] ? "no" : "yes");
    if (WIFSIGNALED(st))
        printf("%s: killed by signal %d\n", name, WTERMSIG(st));
    else
        printf("%s: exit status %d\n", name, WEXITSTATUS(st));
    rmdir(dir);
}

int main(void) {
    setvbuf(stdout, 0, _IONBF, 0);
    sh = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1,
              0);
    if (sh == MAP_FAILED)
        return 1;
    leg("calls from a handler on the alternate stack", 1);
    leg("calls from the main stack, the alternate one registered", 0);
    return 0;
}
