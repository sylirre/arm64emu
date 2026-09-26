/* sigretarget.c's pthread_sigqueue row: a thread-directed SI_QUEUE -- an
 * rt_tgsigqueueinfo -- is the thread's alone, and dies with the thread when
 * it exits with the signal still pending; the process never takes it.
 *
 * Its siginfo does not say it is the thread's, so the sending emulator marks
 * it (signal.c, sig_thread_code). A host that cannot carry the mark -- qemu-
 * user, the ARM32 tier's, which fails an si_code of no known layout and cuts
 * the rest to sixteen bits -- has the emulator send it unmarked, taken for the
 * process's (sig_probe_host), so the row stands on its own, gated:
 * NEEDS-HOST-SYSCALL: sigqueue-siginfo */
#define SIGRETARGET_TQ 1
#include "sigretarget.c"
