/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* arm64emu: an AArch64 emulator with two modes.
 *
 *   arm64emu boot [options]                                 (boot/main.c)
 *   arm64emu chroot [options] <rootfs> <program> [args...]  (chroot/main.c)
 *
 * This is the entry: it hands the command line past its first word to the
 * mode that word names, so a mode sees its own name as argv[0]. What both
 * modes' command lines share -- the help renderer, the option helpers, the
 * engine options -- is cli.c's.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "cli.h"

/* Boot mode is built only where the host is 64-bit (its guest-RAM model uses
 * __int128): elsewhere nothing defines it and the reference is null. */
int boot_main(int argc, char **argv) __attribute__((weak));

/* Terse synopsis for argument errors: to stderr, exit 2. */
_Noreturn static void usage(void) {
    fputs("usage: arm64emu boot [options]\n"
          "       arm64emu chroot [options] <rootfs> <program> [args...]\n"
          "try 'arm64emu --help' for details\n", stderr);
    exit(2);
}

/* The program's reference: what it is and its modes; each mode's own
 * options are its --help's. Printed to stdout on -h/--help, exit 0. */
_Noreturn static void help(void) {
    FILE *f = stdout;
    int w = help_cols();

    const struct help_def modes[] = {
        {"boot", boot_main
            ? "Full-system emulation of the QEMU 'virt' machine: boots its "
              "firmware (EDK2) and a Linux kernel, or a bare-metal image, "
              "with its devices -- GICv2, generic timer, PL011 UART, PL031 "
              "RTC, PSCI, fw_cfg, flash and virtio disks, network, 9p "
              "shares and consoles."
            : "Full-system emulation of the QEMU 'virt' machine. Not in this "
              "build: it needs a 64-bit host."},
        {"chroot", "User-space emulation: runs the programs of an AArch64 "
              "Linux root filesystem on the host's own kernel, unprivileged, "
              "in an isolated chroot-like environment with bind mounts, a "
              "synthesized /proc and /dev, a fake user identity and hardlink "
              "emulation."},
    };
    static const struct help_def opts[] = {
        HELP_OPT_HELP,
        HELP_OPT_VERSION,
    };

    help_section(f, "USAGE");
    help_wrap(f, "arm64emu boot [options]", w, 2, 0);
    help_wrap(f, "arm64emu chroot [options] <rootfs> <program> [args...]", w, 2, 0);

    help_section(f, "DESCRIPTION");
    help_wrap(f,
        "AArch64 (ARMv8-A) emulator, of a whole machine or of a Linux user "
        "space. Both modes run the same CPU: the plain interpreter, which is "
        "the reference, the predecoded tier, which is the default, and the "
        "JIT (--jit), the two faster tiers kept identical to the reference.\n\n"
        "'arm64emu <mode> --help' lists the options and the environment "
        "variables of a mode.",
    w, 2, 0);

    help_section(f, "MODES");
    help_defs(f, modes, HELP_N(modes), w);

    help_section(f, "OPTIONS");
    help_defs(f, opts, HELP_N(opts), w);
    fputc('\n', f);   /* trailing blank line so output ends clear of the prompt */

    exit(0);
}

/* Embedded builds (-DANDROID_JNI) expose the entry under a name of its own so
 * a host process can link the emulator in and call it directly -- the
 * arrangement github.com/sylirre/ghostty-android-terminal uses, where
 * Android's W^X rule leaves nothing to exec: the caller forks, enters here
 * with the mode's word as argv[1], and _exit()s the return value. Fatal paths
 * leave through _exit() too (embed.h). Nothing else differs; a boot run
 * driven that way is one with every console bound (--console-fd).
 *
 * It is marked visible on purpose, because such a host is expected to build
 * the emulator as its own shared object with -fvisibility=hidden, so that the
 * emulator's global names stay its own: this one symbol is the whole
 * interface. */
#ifdef ANDROID_JNI
__attribute__((visibility("default")))
int arm64emu_main(int argc, char **argv)
#else
int main(int argc, char **argv)
#endif
{
    g_env_prefix = "ARM64EMU_";          /* lib/arm64's knobs are ours */

    if (argc < 2) usage();
    const char *mode = argv[1];
    if (!strcmp(mode, "boot")) {
        if (!boot_main) {
            fputs("arm64emu: boot mode is not in this build: it needs a "
                  "64-bit host\n", stderr);
            return 2;
        }
        return boot_main(argc - 1, argv + 1);
    }
    if (!strcmp(mode, "chroot")) return chroot_main(argc - 1, argv + 1);
    if (!strcmp(mode, "-h") || !strcmp(mode, "--help")) help();
    if (!strcmp(mode, "-v") || !strcmp(mode, "--version")) version();
    if (mode[0] == '-') fprintf(stderr, "arm64emu: unknown option '%s'\n", mode);
    else fprintf(stderr, "arm64emu: unknown mode '%s'\n", mode);
    usage();
}
