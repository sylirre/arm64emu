/* How deep a #! chain runs: five scripts and the program they end in, a sixth
 * script ELOOP (exec_binprm's `depth > 5`, from 5.8; the advertised kernel is
 * 6.1). Self-checking: qemu-user hands a script to the host's own exec. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "ran"))
        return 42;
    char dir[] = "/tmp/shdXXXXXX", p[64], line[128];
    if (!mkdtemp(dir))
        return 1;
    for (int i = 1; i <= 6; i++) {
        snprintf(p, sizeof p, "%s/s%d", dir, i);
        if (i == 1)
            snprintf(line, sizeof line, "#!%s ran\n", argv[0]);
        else
            snprintf(line, sizeof line, "#!%s/s%d\n", dir, i - 1);
        FILE *f = fopen(p, "w");
        fputs(line, f);
        fclose(f);
        chmod(p, 0755);
    }
    for (int i = 4; i <= 6; i++) {
        snprintf(p, sizeof p, "%s/s%d", dir, i);
        pid_t k = fork();
        if (k == 0) {
            char *av[] = {p, 0}, *ev[] = {0};
            execve(p, av, ev);
            _exit(errno);
        }
        int st;
        waitpid(k, &st, 0);
        printf("%d deep: %s\n", i, WEXITSTATUS(st) == 42 ? "ran" : strerror(WEXITSTATUS(st)));
    }
    for (int i = 1; i <= 6; i++) {
        snprintf(p, sizeof p, "%s/s%d", dir, i);
        unlink(p);
    }
    rmdir(dir);
    return 0;
}
