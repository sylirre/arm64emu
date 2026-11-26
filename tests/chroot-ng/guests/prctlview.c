/* The guest's view of the two prctl ops that describe confinement, and of
 * seccomp(2), under the monitor (M17-9). The monitor's own filter and the
 * no_new_privs bit it set to install it are its own business: PR_GET_SECCOMP
 * says 0, PR_GET_NO_NEW_PRIVS says what the guest set, a filter the guest
 * tries to install over ours is refused as an unprivileged caller without
 * no_new_privs is (EACCES), seccomp(2) is ENOSYS, and an op the monitor does
 * not own reaches the kernel. Prints one "prctltest ..." line per question. */
#include <errno.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

static long rc(long r) { return r < 0 ? -errno : r; }

int main(void) {
    printf("prctltest GET_SECCOMP=%ld\n", rc(prctl(PR_GET_SECCOMP, 0, 0, 0, 0)));
    printf("prctltest GET_NO_NEW_PRIVS (host bit hidden)=%ld\n",
           rc(prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0)));
    struct sock_filter allow = BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    struct sock_fprog prog = {1, &allow};
    printf("prctltest SET_SECCOMP refused=%ld\n",
           rc(prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog, 0, 0)));
    printf("prctltest seccomp(2) refused=%ld\n",
           rc(syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &prog)));
    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    printf("prctltest GET_NO_NEW_PRIVS after the guest set it=%ld\n",
           rc(prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0)));
    printf("prctltest GET_NO_NEW_PRIVS with a stray argument is EINVAL=%ld\n",
           rc(prctl(PR_GET_NO_NEW_PRIVS, 1, 0, 0, 0)));
    long sr = rc(prctl(PR_SET_NAME, "cngprctl", 0, 0, 0));
    char comm[17] = {0};
    prctl(PR_GET_NAME, comm, 0, 0, 0);
    printf("prctltest SET_NAME passthrough rc=%ld comm=%s\n", sr, comm);
    return 0;
}
