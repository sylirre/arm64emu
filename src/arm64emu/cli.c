/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* The command line both modes share: cli.h says what is here. */
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "cpu.h"
#include "jit.h"
#include "pdrun.h"
#include "cli.h"

/* The version is the repository's, defined once in its VERSION file, which
 * the Makefile passes in. */
#ifndef PROJECT_VERSION
#error "PROJECT_VERSION is undefined: build with the Makefile, or define it from the VERSION file"
#endif

void version(void) {
    fputs("Version: " PROJECT_VERSION "\n", stdout);
    fputs("GitHub: https://github.com/sylirre/arm64emu\n", stdout);
    exit(0);
}

/* --- help renderer ------------------------------------------------------ */

#define HELP_MIN_COLS 32   /* clamp for very narrow phone PTYs           */
#define HELP_MAX_COLS 92   /* clamp so wide terminals stay readable      */
#define HELP_NARROW   60   /* below this, name+description stack vertically */

/* Terminal width for help output, clamped to [HELP_MIN_COLS, HELP_MAX_COLS].
 * Help is written to stdout, so probe stdout first, then stdin/stderr, then
 * $COLUMNS (for redirected runs), finally a sane default. */
int help_cols(void) {
    struct winsize ws;
    static const int fds[] = { 1, 0, 2 };   /* stdout, stdin, stderr */
    for (int i = 0; i < 3; i++) {
        if (ioctl(fds[i], TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
            int c = ws.ws_col;
            return c < HELP_MIN_COLS ? HELP_MIN_COLS
                 : c > HELP_MAX_COLS ? HELP_MAX_COLS : c;
        }
    }
    int c = 80;
    const char *cols = getenv("COLUMNS");
    if (cols && *cols) { int v = atoi(cols); if (v > 0) c = v; }
    return c < HELP_MIN_COLS ? HELP_MIN_COLS
         : c > HELP_MAX_COLS ? HELP_MAX_COLS : c;
}

/* Greedy word-wrap: emit *text* to *f* wrapped to *width* columns, every line
 * prefixed with *indent* spaces. Words are never split; a blank line in the
 * source ("\n\n") starts a new paragraph. When *skip_first* is set the leading
 * indent of the very first line is suppressed — the caller has already placed
 * the cursor at column *indent* (used by the two-column table for line 1). */
void help_wrap(FILE *f, const char *text, int width, int indent, int skip_first) {
    int avail = width - indent;
    if (avail < 8) avail = 8;
    const char *p = text;
    int col = 0;                 /* chars used on the current line (past indent) */
    int need_indent = !skip_first;
    while (*p) {
        if (p[0] == '\n' && p[1] == '\n') {   /* paragraph break */
            if (col) fputc('\n', f);
            fputc('\n', f);
            col = 0;
            need_indent = 1;
            p += 2;
            while (*p == ' ' || *p == '\n') p++;
            continue;
        }
        if (*p == ' ' || *p == '\n') { p++; continue; }
        const char *e = p;                    /* one word: [p, e) */
        while (*e && *e != ' ' && *e != '\n') e++;
        int wlen = (int)(e - p);
        if (col == 0) {
            if (need_indent) fprintf(f, "%*s", indent, "");
            fwrite(p, 1, wlen, f);
            col = wlen;
            need_indent = 1;      /* every subsequent line is indented */
        } else if (col + 1 + wlen <= avail) {
            fputc(' ', f);
            fwrite(p, 1, wlen, f);
            col += 1 + wlen;
        } else {
            fprintf(f, "\n%*s", indent, "");
            fwrite(p, 1, wlen, f);
            col = wlen;
        }
        p = e;
    }
    if (col) fputc('\n', f);
}

/* Render name/description pairs as an aligned two-column table, falling back
 * to a stacked layout (name on its own line, description indented below) when
 * the terminal is too narrow to give the description a usable column. Entries
 * are separated by one blank line (proot-distro options spacing). */
void help_defs(FILE *f, const struct help_def *d, int n, int width) {
    size_t longest = 0;
    for (int i = 0; i < n; i++)
        if (strlen(d[i].name) > longest) longest = strlen(d[i].name);

    int cap = width / 3; if (cap < 16) cap = 16;
    int opt_col = (int)longest < cap ? (int)longest : cap;
    int desc_col = width - opt_col - 4;
    int stacked = width < HELP_NARROW || desc_col < 24;

    for (int i = 0; i < n; i++) {
        if (stacked) {
            fprintf(f, "  %s\n", d[i].name);
            help_wrap(f, d[i].desc, width, 4, 0);
        } else {
            int cont = 2 + opt_col + 2;   /* description column start */
            if ((int)strlen(d[i].name) <= opt_col) {
                /* Name and its 2-space pad place the cursor at column *cont*,
                 * so the first description line skips its own indent. */
                fprintf(f, "  %-*s  ", opt_col, d[i].name);
                help_wrap(f, d[i].desc, width, cont, 1);
            } else {
                fprintf(f, "  %s\n", d[i].name);
                help_wrap(f, d[i].desc, width, cont, 0);
            }
        }
        if (i != n - 1) fputc('\n', f);   /* one blank line between entries */
    }
}

/* Print shell examples, each prefixed with "  $ " and wrapped with a trailing
 * " \\" continuation so long command lines stay copy-pasteable. */
void help_examples(FILE *f, const char *const *ex, int n, int width) {
    int avail = width - 6; if (avail < 8) avail = 8;   /* -4 prefix, -2 " \\" */
    for (int i = 0; i < n; i++) {
        const char *p = ex[i];
        int first = 1, col = 0;
        while (*p) {
            while (*p == ' ') p++;
            if (!*p) break;
            const char *e = p;
            while (*e && *e != ' ') e++;
            int wlen = (int)(e - p);
            if (col == 0) {
                fputs(first ? "  $ " : "    ", f);
                fwrite(p, 1, wlen, f);
                col = wlen;
            } else if (col + 1 + wlen <= avail) {
                fputc(' ', f);
                fwrite(p, 1, wlen, f);
                col += 1 + wlen;
            } else {
                fputs(" \\\n", f);
                fputs("    ", f);
                fwrite(p, 1, wlen, f);
                col = wlen;
                first = 0;
            }
            p = e;
        }
        fputc('\n', f);
    }
}

/* Blank line, an UPPERCASE section heading, then a blank line beneath it
 * (proot-distro section spacing; no rule, no color). */
void help_section(FILE *f, const char *title) {
    fprintf(f, "\n%s\n\n", title);
}

/* --- option parsing helpers --------------------------------------------- */

void opt_no_value(const char *opt) {
    fprintf(stderr, "arm64emu: option '%s' takes no value\n", opt);
    exit(2);
}

char *long_value(const char *opt, char *val, char **argv, int argc, int *pi) {
    if (val) return val;
    if (*pi + 1 < argc) return argv[++*pi];
    fprintf(stderr, "arm64emu: option '%s' requires an argument\n", opt);
    exit(2);
}

char *short_value(const char *opt, char *rest, char **argv, int argc, int *pi) {
    if (*rest) return rest;
    if (*pi + 1 < argc) return argv[++*pi];
    fprintf(stderr, "arm64emu: option '%s' requires an argument\n", opt);
    exit(2);
}

/* --- the engine options ------------------------------------------------- */

unsigned long long g_max_insn;

bool engine_long_opt(const char *name, char *val, const char *arg,
                     char **argv, int argc, int *pi) {
    if (!strcmp(name, "max-insn")) {
        g_max_insn = strtoull(long_value("--max-insn", val, argv, argc, pi), 0, 0);
        return true;
    }
    if      (!strcmp(name, "debug"))        g_trace = 1;
    else if (!strcmp(name, "jit"))          g_jit = 1;
    else if (!strcmp(name, "no-predecode")) g_pd = 0;
    else if (!strcmp(name, "reg-trace"))    g_rtrace = 1;
    else return false;
    if (val) opt_no_value(arg);
    return true;
}

bool engine_short_opt(char c) {
    if      (c == 'd') g_trace = 1;
    else if (c == 'j') g_jit = 1;
    else return false;
    return true;
}

void step_diag_env(void) {
    const char *v;
    if ((v = arm64_getenv("PROF"))) g_prof = atoi(v);
    if ((v = arm64_getenv("TPC")))  g_tpc = strtoull(v, 0, 0);
    if ((v = arm64_getenv("RING"))) g_ring = atoi(v);
    if ((v = arm64_getenv("COV")))  { g_ring = 1; cov_load(v); }
}

void engine_settle(bool per_insn) {
    if (g_jit && per_insn) {
        fprintf(stderr, "arm64emu: JIT disabled by a per-instruction debug "
                        "facility, using the interpreter\n");
        g_jit = 0;
    }
    if (g_jit && !jit_backend_available()) {
        fprintf(stderr, "arm64emu: JIT has no code generator for this host, "
                        "using the interpreter\n");
        g_jit = 0;
    }
    if (g_pd && (g_jit || per_insn)) g_pd = 0;
}
