/* Is the memory just below the stack pointer left alone across a syscall?
 *
 * A kernel's svc touches no memory of the caller's, so what lies below sp
 * survives it, and Go counts on that: a thread its runtime clones finds its m,
 * g and function 8 to 32 bytes under its new sp, and reads them after the
 * gettid that is its first syscall. The -R trampoline built its register
 * frame right under sp and wrote the saved flags over the function, and every
 * Go program died at its first thread (SIGSEGV at 0x60000000, NZCV's Z and
 * C). Here the 64 bytes under sp are filled with a pattern, a gettid is made
 * with nothing else in between, and the words that changed are counted.
 *
 * Prints "clobbered=N". AArch64 only.
 */
#include <stdio.h>

int main(void) {
    unsigned long got[8];
    unsigned long pat = 0xa5a55a5aa5a55a5aUL;
    __asm__ volatile("stp %[p], %[p], [sp, #-16]\n\t"
                     "stp %[p], %[p], [sp, #-32]\n\t"
                     "stp %[p], %[p], [sp, #-48]\n\t"
                     "stp %[p], %[p], [sp, #-64]\n\t"
                     "mov x8, #178\n\t" /* gettid */
                     "svc #0\n\t"
                     "ldp x10, x11, [sp, #-16]\n\t"
                     "stp x10, x11, [%[g], #0]\n\t"
                     "ldp x10, x11, [sp, #-32]\n\t"
                     "stp x10, x11, [%[g], #16]\n\t"
                     "ldp x10, x11, [sp, #-48]\n\t"
                     "stp x10, x11, [%[g], #32]\n\t"
                     "ldp x10, x11, [sp, #-64]\n\t"
                     "stp x10, x11, [%[g], #48]\n\t"
                     :
                     : [p] "r"(pat), [g] "r"(got)
                     : "x0", "x8", "x10", "x11", "memory");
    int n = 0;
    for (int i = 0; i < 8; i++)
        n += got[i] != pat;
    printf("clobbered=%d\n", n);
    return n != 0;
}
