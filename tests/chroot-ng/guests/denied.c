/* The designed-ENOSYS set, as a guest meets it (M5b). Calls that reach the
 * filesystem or the host by a route the path traps cannot see are refused
 * outright: io_uring submits paths through a ring, clone3's flags sit behind
 * a pointer the filter cannot read, a POSIX mqueue name lives in the host's
 * IPC namespace, the mount family and acct name host objects, and a guest
 * filter would govern the monitor's own re-issued calls. Each is ENOSYS -- not
 * EPERM, which would say it might have worked with privilege. getpid is the
 * control: an ordinary call still runs. Prints "denied NAME: rc=R -> OK|FAIL". */
#include <errno.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SYS_statmount
#define SYS_statmount 457
#endif
#ifndef SYS_open_tree
#define SYS_open_tree 428
#endif
#ifndef SYS_clone3
#define SYS_clone3 435
#endif

int main(void) {
    static const struct { const char *name; long nr; } d[] = {
        {"io_uring_setup", SYS_io_uring_setup},
        {"io_uring_enter", SYS_io_uring_enter},
        {"io_uring_register", SYS_io_uring_register},
        {"clone3", SYS_clone3},
        {"statmount", SYS_statmount},
        {"open_tree", SYS_open_tree},
        {"mq_open", SYS_mq_open},
        {"mq_timedsend", SYS_mq_timedsend},
        {"mount", SYS_mount},
        {"umount2", SYS_umount2},
        {"pivot_root", SYS_pivot_root},
        {"quotactl", SYS_quotactl},
        {"swapon", SYS_swapon},
        {"acct", SYS_acct},
        {"seccomp", SYS_seccomp},
    };
    for (unsigned i = 0; i < sizeof d / sizeof *d; i++) {
        long r = syscall(d[i].nr, 0, 0, 0, 0, 0, 0);
        long rc = r < 0 ? -errno : r;
        printf("denied %s: rc=%ld -> %s\n", d[i].name, rc,
               rc == -ENOSYS ? "OK" : "FAIL");
    }
    long g = syscall(SYS_getpid);
    printf("denied control getpid: rc=%ld -> %s\n", g, g > 0 ? "OK" : "FAIL");
    return 0;
}
