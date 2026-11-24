/* The initial stack's strings end a pointer short of its top: bprm_mm_init
 * starts bprm->p at vm_end - sizeof(void *), so the last word is zero and lies
 * above every string, AT_EXECFN's included. A reader of the argument block
 * may read that word (a word past execfn's NUL is still the stack); the
 * emulator put execfn's last byte at the very top, and the read faulted.
 * Checked on the initial stack and on the one an execve builds. */
#include <stdio.h>
#include <string.h>
#include <sys/auxv.h>
#include <unistd.h>

extern char **environ;

static int check(int argc, char **argv) {
    char *at = argv[0];
    for (int i = 0; i < argc; i++) {
        if (argv[i] != at) { printf("argv[%d] out of place\n", i); return 1; }
        at += strlen(argv[i]) + 1;
    }
    for (int i = 0; environ[i]; i++) {
        if (environ[i] != at) { printf("envp[%d] out of place\n", i); return 1; }
        at += strlen(environ[i]) + 1;
    }
    char *fn = (char *)getauxval(AT_EXECFN);
    if (fn != at) { printf("AT_EXECFN out of place\n"); return 1; }
    at += strlen(fn) + 1;
    unsigned long top;
    memcpy(&top, at, sizeof top);
    printf("argc=%d top_word=%lu aligned=%d\n", argc, top, !((unsigned long)at & 7));
    return 0;
}

int main(int argc, char **argv) {
    if (check(argc, argv)) return 1;
    if (argc > 1) return 0;
    fflush(stdout);
    char *av[] = {argv[0], "again", "x", 0};
    /* The environment goes along: a dynamic oracle's re-exec is a qemu that
     * binfmt starts, which finds its ld.so through QEMU_LD_PREFIX. */
    execve(argv[0], av, environ);
    return 2;
}
