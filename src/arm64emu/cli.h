/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* The command line both modes share (cli.c): the help renderer, the option
 * helpers, the version, and the options and environment knobs of the engines
 * every mode runs -- the interpreter, the predecoded tier and the JIT, which
 * are lib/arm64's. Each mode's own options are its main.c's. */
#ifndef ARM64EMU_CLI_H
#define ARM64EMU_CLI_H

#include <stdbool.h>
#include <stdio.h>

/* The modes: argv is the program's past its first word, so argv[0] is the
 * mode's name. boot_main is absent where the host cannot run a full system
 * (main.c). */
int boot_main(int argc, char **argv);
int chroot_main(int argc, char **argv);

/* --- help renderer: reflow the reference to the terminal width ---------
 * Layout mirrors the proot-distro help renderer: UPPERCASE sections framed by
 * blank lines, a two-column name/description table (one blank line between
 * entries) that collapses to a stacked layout on narrow PTYs, minus coloring. */

/* A named entry (option / argument / env var) with its description. */
struct help_def { const char *name, *desc; };
#define HELP_N(a) ((int)(sizeof (a) / sizeof *(a)))

int  help_cols(void);
void help_wrap(FILE *f, const char *text, int width, int indent, int skip_first);
void help_defs(FILE *f, const struct help_def *d, int n, int width);
void help_examples(FILE *f, const char *const *ex, int n, int width);
void help_section(FILE *f, const char *title);

/* -v/--version: print the version to stdout and exit 0. */
_Noreturn void version(void);

/* --- option parsing helpers --------------------------------------------
 * GNU-style options: single-letter short (-j), --word long; value-taking
 * options accept "--opt VAL"/"--opt=VAL" and "-o VAL"/"-oVAL". A usage error
 * is reported on stderr and exits 2. */

/* Fatal: a flag that takes no value was given one ("--help=x"). */
_Noreturn void opt_no_value(const char *opt);
/* Value for a long value-taking option: "--opt=VAL" (val, possibly "") if a
 * '=' was present, else the next argv token (consumed via *pi). Fatal if none. */
char *long_value(const char *opt, char *val, char **argv, int argc, int *pi);
/* Value for a short value-taking option: the attached rest-of-token "-oVAL"
 * (rest) when non-empty, else the next argv token (consumed via *pi). */
char *short_value(const char *opt, char *rest, char **argv, int argc, int *pi);

/* --- the engine options ------------------------------------------------
 * -d/--debug, -j/--jit and --no-predecode mean the same in every mode. A
 * mode's parser offers each option to these first: true when it was one of
 * them (a long one given a value is a usage error). */
bool engine_long_opt(const char *name, const char *val, const char *arg);
bool engine_short_opt(char c);
/* Once the mode knows whether a per-instruction facility is on (which needs
 * the interpreter's single step): settle the engine. --jit yields to such a
 * facility and to a host without a code generator, saying so; the predecoded
 * tier, being the default, yields to both silently. */
void engine_settle(bool per_insn);

/* The engine options' help entries, for each mode's OPTIONS table. */
#define HELP_OPT_HELP    {"-h, --help",    "Show this help and exit."}
#define HELP_OPT_VERSION {"-v, --version", "Show version and exit."}
#define HELP_OPT_DEBUG   {"-d, --debug",   "Per-instruction trace to stderr. " \
        "Very verbose; forces the interpreter."}
#define HELP_OPT_JIT     {"-j, --jit",     "Translate hot code to native code " \
        "on AArch64, x86-64, i686 and ARM32 hosts. Falls back to the " \
        "interpreter elsewhere."}
#define HELP_OPT_NO_PREDECODE {"    --no-predecode", "Run the plain " \
        "interpreter instead of the default predecoded tier (diagnostic, " \
        "slower)."}

/* The engines' environment knobs (lib/arm64's, read through arm64_getenv),
 * for each mode's ENVIRONMENT tables: tuning, then diagnostics. */
#define HELP_ENV_TUNE \
    {"ARM64EMU_JIT_MB=N", "JIT code-cache size in MiB, per guest CPU (per " \
        "guest thread in chroot mode). Default 32, clamped to 1-128 (1-32 on " \
        "ARM32 hosts)."}
#define HELP_ENV_DIAG \
    {"ARM64EMU_JIT_STATS=1|/path", "Rank the instruction words still run " \
        "through the exec_a64 helper; =/path dumps the ranking to a file at " \
        "exit."}, \
    {"ARM64EMU_JIT_DUMP=PREFIX", "Write each translated block into a sparse " \
        "code-cache image (PREFIX.<pid>.<tid>.code plus a .map)."}, \
    {"ARM64EMU_JIT_PDMAX=N", "Translate only predecode ops with id <= N " \
        "natively, the rest through the helper (bisects a codegen bug to one " \
        "instruction class)."}, \
    {"ARM64EMU_JIT_SLOWMEM=1", "Force every inline memory access down its " \
        "slow helper branch."}, \
    {"ARM64EMU_JIT_NOFUSE=1", "Disable instruction / D-TLB-probe fusion."}, \
    {"ARM64EMU_JIT_NOVRA=1", "Disable the V-register cache."}, \
    {"ARM64EMU_JIT_NOFP16=1", "Disable FP16 native codegen (AArch64 backend)."}, \
    {"ARM64EMU_JIT_SSE=2", "Force SSE2-baseline capability answers (x86-64)."}, \
    {"ARM64EMU_PD_MAX=N", "Dispatch only predecode ops with id <= N natively " \
        "in the predecoded tier (0 = the plain interpreter); the analogue of " \
        "ARM64EMU_JIT_PDMAX."}

#endif /* ARM64EMU_CLI_H */
