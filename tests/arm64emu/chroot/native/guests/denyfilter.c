/* Run a program under a filter that refuses what the native tier's preflight
 * asks for (M29), so that the fallback it takes, and the reason it gives, can
 * be seen on a host that would grant all of it:
 *
 *   denyfilter seccomp PROG ARGS...   a further seccomp filter is EACCES (the
 *                                     prctl and the seccomp syscall alike), as
 *                                     a policy that forbids one answers
 *   denyfilter exec PROG ARGS...      memory may not become executable: an
 *                                     mprotect or mmap with PROT_EXEC is
 *                                     EACCES, as SELinux's execmem denial and
 *                                     a noexec mount answer together
 *
 * Started outside arm64emu, so the filter is in place when it starts, as an
 * ambient one is on a device. Where no filter can be installed (qemu-user) it
 * says SKIP instead. */
#include <errno.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#define ARG_LO(n) (offsetof(struct seccomp_data, args) + 8 * (n))
#define DENY (SECCOMP_RET_ERRNO | EACCES)

int main(int argc, char **argv) {
    if (argc < 3)
        return 2;
    struct sock_filter seccomp_f[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_seccomp, 4, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_prctl, 0, 2),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, ARG_LO(0)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, PR_SET_SECCOMP, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_RET | BPF_K, DENY),
    };
    struct sock_filter exec_f[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_mprotect, 1, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_mmap, 0, 2),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, ARG_LO(2)),
        BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, PROT_EXEC, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_RET | BPF_K, DENY),
    };
    struct sock_fprog p;
    if (!strcmp(argv[1], "seccomp"))
        p = (struct sock_fprog){sizeof seccomp_f / sizeof *seccomp_f, seccomp_f};
    else if (!strcmp(argv[1], "exec"))
        p = (struct sock_fprog){sizeof exec_f / sizeof *exec_f, exec_f};
    else
        return 2;
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 ||
        prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &p) != 0) {
        printf("SKIP: no seccomp filter can be installed here\n");
        return 0;
    }
    execv(argv[2], argv + 2);
    perror("denyfilter: execv");
    return 127;
}
