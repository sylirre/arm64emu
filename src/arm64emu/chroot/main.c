/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* Chroot mode: run an AArch64 Linux program from a rootfs directory under
 * user-space emulation -- its command line, the rootfs setup and the initial
 * exec.
 *
 *   arm64emu chroot [options] <rootfs> <program> [args...]
 *
 * The full option / environment-variable reference lives in help() below
 * (reachable via -h/--help), which reflows it to the terminal width. Keep the
 * option strings there and the overview in README.md in sync. The engine
 * options and the help renderer are both modes' (../cli.c).
 */
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "machine.h"
#include "thread.h"
#include "jit.h"
#include "pdrun.h"
#include "guest_abi.h"
#include "ptrace.h"
#include "sys.h"
#include "cli.h"
#include "native/native.h"
#include "link2symlink.h"

/* The native tier (native/): built where the target is AArch64 only, and
 * null elsewhere, where --native says so and the emulator runs the guest. */
extern int native_run(const struct native_opts *o, char *why, size_t whysz)
    __attribute__((weak));

struct Machine g_machine;

extern char **environ;

/* Did a -E/--env flag already set the key of `kv` ("KEY=" or "KEY=VALUE")? Such
 * a key is neither inherited from the host nor given a default -- the flag is
 * the whole answer for it. */
static int env_set_by_flag(char **extra, int n, const char *kv) {
    const char *eq = strchr(kv, '=');
    size_t kl = eq ? (size_t)(eq - kv) + 1 : strlen(kv);
    for (int j = 0; j < n; j++)
        if (!strncmp(extra[j], kv, kl)) return 1;
    return 0;
}

/* Terse synopsis for argument errors: one line to stderr, exit 2. The full
 * reference lives in help() below, reachable via -h/--help. */
static void usage(void) {
    fprintf(stderr,
            "usage: arm64emu chroot [options] <rootfs> <program> [args...]\n"
            "try 'arm64emu chroot --help' for details\n");
    exit(2);
}

/* Full reference help: purpose, usage, arguments, every option, environment
 * variables, and examples. Reflowed to the terminal width. Printed to stdout
 * on -h/--help, exit 0. */
static void help(void) {
    FILE *f = stdout;
    int w = help_cols();

    static const struct help_def args[] = {
        {"<rootfs>",  "Directory tree holding an AArch64 userland (e.g. an "
                      "Alpine or Debian ARM64 root filesystem); '/' runs "
                      "host-native aarch64 binaries directly. Guest paths "
                      "resolve inside the rootfs."},
        {"<program>", "Guest program to execute, absolute path inside the "
                      "rootfs."},
        {"args...",   "Arguments passed on to the guest program."},
    };
    static const struct help_def opts[] = {
        HELP_OPT_HELP,
        HELP_OPT_VERSION,
        {"    --native", "Run the guest's code on the host CPU instead of "
                        "emulating it (AArch64 hosts): the native tier loads "
                        "the program and its interpreter into this process "
                        "and intercepts its system calls with a seccomp "
                        "filter (SIGSYS), serving them against the same view "
                        "of the rootfs as the emulator -- the binds, the fake "
                        "identity, /proc and /dev, the emulated hardlinks. "
                        "Where it cannot run -- another host architecture, a "
                        "kernel or a policy that refuses the filter, code "
                        "that cannot be made executable, an option that "
                        "needs the emulator (--strace, the step diagnostics) "
                        "-- a warning says why and the emulator runs the "
                        "guest, with --jit (with the interpreter for a step "
                        "diagnostic). The engine options apply to that "
                        "fallback only."},
        {"    --strace", "Log guest syscalls to stderr."},
        {"    --strace-full", "Like --strace, but decode arguments strace-style: "
                        "symbolic flags, quoted strings, structs, errno returns."},
        HELP_OPT_DEBUG,
        HELP_OPT_REG_TRACE,
        HELP_OPT_MAX_INSN,
        HELP_OPT_JIT,
        HELP_OPT_NO_PREDECODE,
        {"-l, --link2symlink", "Emulate hardlinks with tracked symlinks where "
                        "the host forbids link() (Android OS); also reads and "
                        "keeps the hardlink groups a rootfs installed through "
                        "proot (proot-distro) holds."},
        {"    --shared-proc", "Share synthesized /proc view between multiple "
                        "emulator sessions within same rootfs."},
        {"    --no-dev", "Disable the synthesized /dev."},
        {"    --no-proc", "Disable the synthesized /proc."},
        {"-b, --bind SRC:DST[:ro]", "Expose host directory SRC at guest path "
                        "DST (repeatable). Append :ro for a read-only mount. "
                        "Host paths may not contain ':'."},
        {"-E, --env VAR=VAL", "Set a guest environment variable (repeatable), "
                        "overriding anything below that would set the same "
                        "name. The guest environment is otherwise built fresh: "
                        "TERM and COLORTERM are inherited from the host, PATH "
                        "defaults to "
                        "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:"
                        "/sbin:/bin, HOME is left unset, and every other "
                        "host variable is dropped as naming host locations."},
        {"-0, --argv0 ARG0", "Override argv[0] for the guest program"},
        {"-u, --fake-id[=ID]", "Present a fake user identity. Accepts values "
                        "like uid or uid:gid, default 0:0 (root)."},
        {"    --setuid-root", "Show set-uid executables as owned by root, so "
                        "that executing one raises the effective user id to 0 "
                        "(su, sudo). Implies --fake-id, with the invoking "
                        "user's id unless -u gives one."},
        {"    --setgid-root", "The same for set-gid executables and the "
                        "effective group id."},
        {"-F, --file-backed", "Load the program and its interpreter by mapping "
                        "their file pages (shared with the page cache) "
                        "instead of copying them; a segment that cannot be "
                        "mapped is copied. Under --native the program's code "
                        "is always mapped from its file this way -- which is "
                        "otherwise the choice only where anonymous memory "
                        "may not become executable (Android's no-new-privs, "
                        "SELinux execmem)."},
        {"    --share-abstract-sockets", "Share abstract namespace sockets "
                        "of the host, disable per-rootfs socket isolation."},
        {"    --host-keyring", "Forward keyctl/add_key/request_key to the host "
                        "keyring. They report the facility as absent by default: "
                        "keyrings are not scoped to a rootfs, so the guest would "
                        "read, add to and revoke the invoking user's own keys."},
        {"    --keep-fds", "Let the guest inherit the open descriptors above "
                        "stdin/stdout/stderr that the caller left it. They are "
                        "closed before the guest starts by default: guest fd is "
                        "host fd, so an inherited one is a live handle onto a "
                        "host file outside the rootfs."},
        {"-w, --work-dir DIR", "Use a given path of the guest as current "
                        "working directory, resolved through the rootfs and "
                        "its binds; a relative one against the default, which "
                        "is '/', or under the identity rootfs '/' the host's "
                        "own working directory."},
        {"    --",      "Stop option parsing."},
    };
    static const struct help_def env_tune[] = {
        HELP_ENV_TUNE,
        {"XDG_RUNTIME_DIR, TMPDIR", "First writable of these (or /dev/shm) holds "
                        "the backing directories of emulated tmpfs mounts, plus "
                        "the --shared-proc fallback registry file and System V "
                        "shm segment files when memfd_create is unavailable."},
    };
    static const struct help_def env_diag[] = {
        HELP_ENV_STEP_DIAG,
        HELP_ENV_DIAG,
        {"ARM64EMU_PROCSTAT_FORCE_SYNTH", "Force the synthetic /proc/stat fallback."},
        {"ARM64EMU_PROCFS_FORCE_OLD", "Hide from the host's /proc what a kernel "
                        "older than the advertised one does not have -- stat "
                        "past field 44 (3.3) and status's Rss* component "
                        "lines (4.5) -- so the synthesized views must supply "
                        "them (exercises the Android 7 tier)."},
        {"ARM64EMU_PROCSTAT_HOTPLUG_SIM", "Walk the online-CPU count down on every "
                        "/proc/stat sample (simulates a host that hotplugs "
                        "cores, as Android does)."},
        {"ARM64EMU_OVERFLOWID_FORCE_SYNTH", "Force the synthetic "
                        "/proc/sys/kernel/overflow{u,g}id fallback."},
        {"ARM64EMU_PATHFAST_OFF", "Resolve every guest path by walking it one "
                        "component at a time (a readlink each) instead of "
                        "folding it and letting the pin certify that it held "
                        "no symlink. Same answers, more syscalls."},
        {"ARM64EMU_PATHFAST_VERIFY", "Resolve every guest path BOTH ways and abort "
                        "on any disagreement. For a quiescent tree: a guest "
                        "racing its own mounts or renames makes the two walks "
                        "differ for a reason that is not a bug."},
        {"ARM64EMU_PINWALK_FORCE_LOOP", "Pin a resolved path's parent directory one "
                        "component at a time instead of in a single openat2 "
                        "(the tier a host kernel older than 5.6 is served by). "
                        "Same containment, one syscall per component."},
        {"ARM64EMU_PROCSYNTH_FORCE_FAIL", "Make the synthesized /proc views find no "
                        "anonymous backing (the tier a host with neither "
                        "memfd_create nor a writable directory is served by): "
                        "the per-process files are denied, never served from "
                        "the host's own /proc."},
        {"ARM64EMU_PROCSYNTH_FORCE_STAT_DENY", "Make the host refuse stat, statx, "
                        "access and statfs of every synthesized /proc file "
                        "(EACCES, as Android's SELinux policy does for the "
                        "files whose open the emulator serves): the guest is "
                        "then answered from the synthesized view instead."},
        {"ARM64EMU_NETLINK_FORCE_BLOCK", "Force the netlink fallback path."},
        {"ARM64EMU_PROCTAB_FORCE_FILE", "Skip the diskless guest-PID registry "
                        "broker for the named-file tier (the tier a host with "
                        "neither memfd_create nor abstract sockets is served "
                        "by); the file is refused unless it is a regular file "
                        "of this user that nobody else can reach."},
        {"ARM64EMU_SHM_FORCE_FILE", "Force System V shm segments onto file backing "
                        "instead of an anonymous memfd (exercises the fallback "
                        "tier)."},
        {"ARM64EMU_GETRANDOM_FORCE_DEV", "Force getrandom(2), and the guest's "
                        "AT_RANDOM seed, onto the /dev/urandom / /dev/random "
                        "fallback (the tier a host kernel without getrandom "
                        "is served by)."},
        {"ARM64EMU_GETRANDOM_FORCE_OLD", "Refuse GRND_INSECURE on the host "
                        "getrandom(2) path the way a 3.17-5.5 kernel does "
                        "(the tier such a host is served by: the flag is "
                        "then answered from /dev/urandom)."},
        {"ARM64EMU_PAGEPROBE_FORCE_PIPE", "Probe a grown file mapping's pages "
                        "with a pipe instead of process_vm_readv (the tier a "
                        "host kernel older than 3.2 is served by)."},
        {"ARM64EMU_MINCORE_FORCE_FAIL", "Make the resident-set sample fail (the "
                        "tier a host that will not answer mincore(2) is served "
                        "by): the sizes the guest reads about its own memory "
                        "stay its own, and the Rss figures fall back to the "
                        "host's, bounded by what the guest mapped -- which is "
                        "what every reader of ANOTHER process already gets; "
                        "and a fault on a page a mapping refuses is a "
                        "permission fault in the frame's ESR whether or not "
                        "the page was ever touched."},
        {"ARM64EMU_MEMFD_FORCE_FILE", "Force guest memfd_create(2) onto the "
                        "unlinked-file fallback tier with broker-held seals "
                        "(what a host kernel without memfd_create is served "
                        "by)."},
        {"ARM64EMU_MEMFD_SEAL_FORCE_OLD", "Refuse a read-only shared mapping of a "
                        "write-sealed memfd the way a kernel older than 6.x "
                        "does, so the emulator's own backing is what serves "
                        "the guest that mapping."},
        {"ARM64EMU_SIGRT_MAX=N", "Reserve the emulator's own host signals below N "
                        "instead of at the top of the RT range (exercises the "
                        "tier a host that cannot deliver its top RT signals is "
                        "served by)."},
        {"ARM64EMU_OWNFD_FORCE_DENY", "Refuse the path spelling of the emulator's "
                        "own fds (/proc/self/fd/N), as Android's policy does "
                        "for a memfd, so exec and open take the descriptor "
                        "fallback."},
        {"ARM64EMU_MEMFD_CHMOD_FORCE_DENY", "Refuse every mode change on a memfd, "
                        "as Android's policy does, so the guest-set mode is "
                        "held in the broker registry instead of by the host."},
        {"ARM64EMU_TERMIOS2_FORCE_DENY", "Refuse the termios2 ioctls (TCGETS2, "
                        "TCSETS2, ...) with EACCES, as Android's policy does, "
                        "so they are served from TCGETS/TCSETS -- without "
                        "which a glibc 2.42+ guest's isatty() is false and "
                        "an interactive bash prints no prompt."},
        {"ARM64EMU_TIOCGSID_FORCE_DENY", "Refuse TIOCGSID (tcgetsid) with EACCES, "
                        "as Android's policy does on a pty slave and a pipe, "
                        "so it is served from TCGETS, TIOCGPGRP and getsid."},
        {"ARM64EMU_TTYIOCTL_FORCE_DENY", "Refuse terminal ioctls with EACCES "
                        "as Android's policy does -- all of them and "
                        "FIOCLEX/FIONCLEX on a pipe, socket, file or "
                        "directory, all but its short list on a pty slave -- "
                        "so the guest is answered from what the kernel does "
                        "(ENOTTY, F_SETFD, the tty core's answers)."},
        {"ARM64EMU_TLBPUB_MAX=N", "Cap the published-epoch table at N slots "
                        "(exercises the tier a guest with more live threads "
                        "than it holds is served by: the retired-backing "
                        "quarantine stops draining)."},
        {"ARM64EMU_SIGQ_MAX=N", "Cap a thread's pending-signal queue at N entries "
                        "(exercises the back-pressure gate: a queue with no "
                        "room leaves the rest of a flood in the host kernel's "
                        "own queue until it drains)."},
        {"ARM64EMU_SIGCHLD_DELAY=MS", "Hold a guest thread MS milliseconds as it "
                        "starts on each SIGCHLD notice the host sends (what a "
                        "loaded host does to a thread it preempts there), so "
                        "the emulator's SIGCHLD watcher runs meanwhile -- the "
                        "condition under which a dead tracer's notice and the "
                        "release of the child it traced must keep the "
                        "kernel's order."},
        {"ARM64EMU_SICODE_FORCE_KNOWN", "Send the emulator's own signals between "
                        "processes with si_codes of a layout the kernel knows "
                        "only (the tier a host that cannot carry a private "
                        "si_code, qemu-user, is served by): a queued siginfo "
                        "the host would not carry whole goes through the "
                        "receiver's registry inbox instead, and a job-control "
                        "signal to a traced process rides the kick as "
                        "SI_QUEUE."},
        {"ARM64EMU_NOCLDWAIT_FORCE_EMULATE", "Reap an SA_NOCLDWAIT parent's "
                        "children and drop an SA_NOCLDSTOP parent's stop "
                        "notices in the emulator instead of leaving it to the "
                        "host (the tier a host that ignores the two flags, "
                        "qemu-user, is served by)."},
        {"ARM64EMU_STACKGROW_FORCE_PIECE", "Reserve no host room below a stack "
                        "and never extend its backing downward, so a stack "
                        "that grows goes on in a new piece of backing of its "
                        "own (the tier a host whose address space under a "
                        "stack is taken is served by)."},
    };
    static const struct help_def env_native[] = {
        {"ARM64EMU_NATIVE_DEBUG", "Under --native, log verbose syscall-error "
                        "diagnostics to stderr, including each syscall the "
                        "host refuses and the tier emulates."},
        {"ARM64EMU_NATIVE_NO_FALLBACK", "Make a --native that cannot be used "
                        "an error (exit 126, after the reason) instead of a "
                        "run under the emulator. Test aid: the tier's suite "
                        "must not pass on the emulator's answers."},
        {"ARM64EMU_L2S_FORCE", "With -l, route every linkat through the "
                        "emulation without trying the real hardlink first. "
                        "Test aid for hosts whose filesystem allows link(2)."},
        {"ARM64EMU_UACCESS_FORCE_MEMFD", "Validate guest pointers through a "
                        "scratch memfd instead of process_vm_readv/writev: "
                        "the fallback a host without that pair uses."},
        {"ARM64EMU_MMAP_FORCE_ANON", "Serve every executable file mapping from "
                        "an anonymous copy without asking the kernel first: "
                        "what a true noexec mount forces anyway."},
        {"ARM64EMU_PROCTAB_FORCE_NONE", "Behave as if the guest-PID registry "
                        "could not be mapped at all."},
        {"ARM64EMU_EXEC_RECLAIM_FORCE", "Treat the process as single-threaded "
                        "when an emulated execve gives the replaced program's "
                        "mappings back (a host emulator running a thread of "
                        "its own beside the guest's); against a genuinely "
                        "multithreaded guest it unmaps memory those threads "
                        "are running on."},
        {"ARM64EMU_SCRATCH_FORCE_NONE", "Behave as if the per-thread scratch "
                        "stack table had no slot to give, so every dispatch "
                        "runs on a stack mapped for the call (what a host "
                        "reaches only with 256 threads live at once)."},
        {"ARM64EMU_ALTSTACK_DELAY=US", "Spend US microseconds on the monitor's "
                        "own stack, at the start of a trap's handling, before "
                        "the guest's alternate signal stack is disarmed: "
                        "widens a window of a few instructions, in which a "
                        "SIGSYS sent from elsewhere must not reach that stack."},
        {"ARM64EMU_DEVSHM_FORCE_TMP", "Serve the guest's /dev/shm from the "
                        "per-uid directory under $TMPDIR even where the host "
                        "has a real /dev/shm, as a device must."},
        {"ARM64EMU_TMP_FORCE_ENV", "Look for the tier's private temp "
                        "directories (the /dev/shm stand-in, read-only /proc "
                        "views) only under $TMPDIR and $XDG_RUNTIME_DIR, "
                        "passing over /data/local/tmp and /tmp, as an Android "
                        "app with an unusable $TMPDIR has to."},
        {"ARM64EMU_PROCFS_REOPEN_FORCE_DENY", "Take the reopen that makes a "
                        "synthesized /proc file read-only as refused, as "
                        "Android's policy refuses an app the reopen of a "
                        "memfd, so the read-only view is served from a file "
                        "in a private directory under $TMPDIR."},
        {"ARM64EMU_TCSETSF_FORCE_DENY", "Refuse TCSETSF (tcsetattr with "
                        "TCSAFLUSH) with EACCES before the host is asked, as "
                        "Android 8's policy does on a pty, so it is served "
                        "from TCFLSH and TCSETSW -- and so is TCSETSF2."},
        {"ARM64EMU_SETID_FORCE_DENY", "Without -u, take the uid and gid "
                        "setters as refused by the host, as Android's app "
                        "filter refuses them, so they are answered from the "
                        "real ids and capabilities the way the kernel would."},
        {"ARM64EMU_OPENAT2_FORCE_DENY", "Take openat2 as refused by the host, "
                        "as Android's app filter refuses it, so a guest's is "
                        "emulated, as on a kernel without one."},
        {"ARM64EMU_MM_MAP_FORCE_DENY", "Take PR_SET_MM_MAP as refused, as "
                        "Android's kernels refuse it, so /proc/<pid>/cmdline "
                        "and environ are read out of guest memory."},
        {"ARM64EMU_BROKER_FORCE_NO_PEERCRED", "Behave as if the kernel would "
                        "not say who is on the other end of the IPC broker's "
                        "rendezvous socket, which refuses the connection."},
        {"ARM64EMU_NETLINK_FORCE_NO_RELAY", "Force the netlink fallback, and "
                        "take the host's relay socket as refused too, so the "
                        "dump is the loopback-only one of a host that grants "
                        "none."},
        {"ARM64EMU_NETLINK_FORCE_DENY_GETLINK", "Force the netlink fallback, "
                        "with RTM_GETLINK refused on the relay socket "
                        "(Android's nlmsg_readpriv), so the link dump is "
                        "synthesized."},
        {"ARM64EMU_NETLINK_FORCE_DENY_AUDIT", "Take a NETLINK_AUDIT socket as "
                        "refused (EACCES), as Android's SELinux policy does."},
    };
    static const char *const examples[] = {
        "arm64emu chroot ./rootfs /bin/sh",
        "arm64emu chroot --native -u ./rootfs /bin/sh -l",
        "arm64emu chroot --jit --fake-id ./rootfs /bin/bash -l",
        ("arm64emu chroot -b \"$PWD:/work\" --bind /etc/ssl:/etc/ssl:ro "
            "./rootfs /bin/su -l"),
    };

    help_section(f, "USAGE");
    help_wrap(f, "arm64emu chroot [options] <rootfs> <program> [args...]", w, 2, 0);

    help_section(f, "DESCRIPTION");
    help_wrap(f,
        "User-space emulation of AArch64 Linux, for running unprivileged, "
        "isolated chroot-like environments. Implements a full user ISA "
        "including FP, ASIMD, AES, PMULL, SHA*, CRC32, atomics. Supports "
        "~190 system calls which should be enough for most of workloads. On "
        "an AArch64 host --native runs the guest's code on the CPU itself "
        "instead.\n\n"
        "Directory layout for /dev expose a minimal set of device nodes "
        "needed for correct functioning of guest programs. The /proc is "
        "almost entirely synthesized to provide process data correct "
        "from the view point of emulator.\n\n"
        "This utility can execute programs on file systems mounted with "
        "'noexec' option or where execution is not allowed by SELinux.",
    w, 2, 0);

    help_section(f, "ARGUMENTS");
    help_defs(f, args, HELP_N(args), w);

    help_section(f, "OPTIONS");
    help_defs(f, opts, HELP_N(opts), w);

    help_section(f, "ENVIRONMENT");
    help_wrap(f, "Tuning:", w, 2, 0);
    fputc('\n', f);
    help_defs(f, env_tune, HELP_N(env_tune), w);
    fputc('\n', f);
    help_wrap(f, "Diagnostics, for development (the per-instruction "
                 "facilities force the interpreter):", w, 2, 0);
    fputc('\n', f);
    help_defs(f, env_diag, HELP_N(env_diag), w);
    fputc('\n', f);
    help_wrap(f, "The native tier's (--native), test aids but the first; it "
                 "reads these of the ones above too: "
                 "ARM64EMU_PROCSTAT_FORCE_SYNTH, ARM64EMU_OVERFLOWID_FORCE_SYNTH, "
                 "ARM64EMU_PROCSYNTH_FORCE_STAT_DENY, ARM64EMU_NETLINK_FORCE_BLOCK, "
                 "ARM64EMU_PROCTAB_FORCE_FILE, ARM64EMU_SHM_FORCE_FILE, "
                 "ARM64EMU_TERMIOS2_FORCE_DENY and ARM64EMU_TIOCGSID_FORCE_DENY:",
              w, 2, 0);
    fputc('\n', f);
    help_defs(f, env_native, HELP_N(env_native), w);

    help_section(f, "EXAMPLES");
    help_examples(f, examples, HELP_N(examples), w);
    fputc('\n', f);   /* trailing blank line so output ends clear of the prompt */

    exit(0);
}

/* Recognize a fake-id spec: "N" or "N:N" (all digits). */
static int is_id_spec(const char *s) {
    if (!s || !*s) return 0;
    int seen_colon = 0, digits = 0;
    for (const char *p = s; *p; p++) {
        if (*p == ':') {
            if (seen_colon || !digits) return 0;
            seen_colon = 1; digits = 0;
        } else if (isdigit((unsigned char)*p)) {
            digits = 1;
        } else return 0;
    }
    return digits;
}

/* Accumulate one decimal id, refusing anything a uid_t cannot hold. One that
 * simply wraps is the wrong kind of wrong here: "4294967296" is 0, and 0 is
 * root -- so a spec out of range did not fail, it quietly asked for the fake
 * identity with the most authority there is. -1 on overflow. */
static int parse_id_num(const char **pp, u32 *out) {
    unsigned long long v = 0;
    const char *p = *pp;
    for (; isdigit((unsigned char)*p); p++) {
        v = v * 10 + (unsigned long long)(*p - '0');
        if (v > 0xffffffffull) return -1;
    }
    *pp = p;
    *out = (u32)v;
    return 0;
}

/* Parse "N" (uid, gid defaults to uid) or "N:M" into *uid,*gid; fatal on an
 * id out of range. */
static void parse_id_spec(const char *s, u32 *uid, u32 *gid) {
    const char *p = s;
    if (parse_id_num(&p, uid) < 0 ||
        (*p == ':' ? (p++, parse_id_num(&p, gid) < 0) : (*gid = *uid, 0))) {
        fprintf(stderr, "arm64emu: --fake-id '%s': expected UID or UID:GID\n", s);
        exit(2);
    }
}

/* Lexically canonicalize an absolute guest path (collapse '.', '..', '//',
 * strip trailing '/') into out (>= PATH_MAX) — the form -bind mount points are
 * matched against in path.c. Returns 0, or -1 on overflow. */
static int canon_guest(const char *in, char *out) {
    strcpy(out, "/");
    for (const char *p = in; *p;) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *e = p;
        while (*e && *e != '/') e++;
        size_t cl = (size_t)(e - p);
        if (cl == 1 && p[0] == '.') { p = e; continue; }
        if (cl == 2 && p[0] == '.' && p[1] == '.') {   /* pop, clamped at "/" */
            char *s = strrchr(out, '/');
            if (s == out) out[1] = 0; else *s = 0;
            p = e; continue;
        }
        size_t ol = strlen(out);
        if (ol + (ol > 1) + cl + 1 > PATH_MAX) return -1;
        if (ol > 1) out[ol++] = '/';
        memcpy(out + ol, p, cl);
        out[ol + cl] = 0;
        p = e;
    }
    return 0;
}

/* Parse a --bind mount "src:dst[:ro]": host path src, realpath'd into `host`
 * (PATH_MAX), is exposed at the absolute guest mount point dst, canonicalized
 * into `guest` (PATH_MAX); an optional trailing ":ro" (or the default ":rw")
 * marks it read-only. Host paths may not contain ':'. Fatal on any malformed
 * spec, for both tiers alike. */
static void parse_bind(const char *spec, char *host, char *guest, int *ro_out) {
    char buf[2 * PATH_MAX];
    if (strlen(spec) + 1 > sizeof buf) {
        fprintf(stderr, "arm64emu: --bind '%s': too long\n", spec);
        exit(2);
    }
    strcpy(buf, spec);
    char *colon = strchr(buf, ':');   /* first ':' splits src | dst[:ro] */
    if (!colon || colon == buf) {
        fprintf(stderr, "arm64emu: --bind '%s': expected src:dst[:ro]\n", spec);
        exit(2);
    }
    *colon = 0;
    const char *src = buf;
    char *dst = colon + 1;
    int ro = 0;
    size_t dl = strlen(dst);
    if (dl >= 3 && !strcmp(dst + dl - 3, ":ro")) { ro = 1; dst[dl - 3] = 0; }
    else if (dl >= 3 && !strcmp(dst + dl - 3, ":rw")) { dst[dl - 3] = 0; }
    if (dst[0] != '/') {
        fprintf(stderr, "arm64emu: --bind '%s': destination path must be absolute\n", spec);
        exit(2);
    }
    if (!realpath(src, host)) {
        fprintf(stderr, "arm64emu: --bind source path '%s': not found\n", src);
        /* The operands the other way round -- a guest path first, which is
         * what chroot-ng took before its 0.1.0 -- read as a missing source
         * whose destination is there on the host: say so, since the bind
         * that would have worked is otherwise a guess. */
        struct stat st;
        if (stat(dst, &st) == 0)
            fprintf(stderr, "arm64emu: --bind takes SRC:DST, the host path "
                            "first; did you mean '%s:%s'?\n", dst, src);
        exit(126);
    }
    if (!strcmp(host, "/")) {
        fprintf(stderr, "arm64emu: --bind '%s': cannot bind host root\n", spec);
        exit(2);
    }
    if (canon_guest(dst, guest) < 0) {
        fprintf(stderr, "arm64emu: --bind '%s': destination path too long\n", spec);
        exit(2);
    }
    if (!strcmp(guest, "/")) {
        fprintf(stderr, "arm64emu: --bind '%s': cannot bind over guest root\n", spec);
        exit(2);
    }
    *ro_out = ro;
}

/* Register a parsed --bind mount with the emulator. */
static void add_bind(struct Machine *m, const char *spec, const char *host,
                     const char *guest, int ro) {
    /* Register into the shared bind table (same path the runtime mount(2) uses). */
    /* A --bind source is a host path the invoker chose: nothing the guest can
     * rename lies above it, so all of it may be opened by name (path.c). And
     * the mount is the invoker's, not the guest's: locked against umount, and
     * a :ro one against being made writable (machine.h, BIND_LOCK*). */
    int r = bind_add(m, guest, host, (unsigned)strlen(host), ro,
                     BIND_LOCKED | (ro ? BIND_LOCK_RO : 0));
    if (r == -ENOMEM) {
        fprintf(stderr, "arm64emu: too many --bind mounts (max %d)\n", BIND_MAX);
        exit(2);
    }
    if (r < 0) {
        fprintf(stderr, "arm64emu: --bind '%s': %s\n", spec, strerror(-r));
        exit(2);
    }
}

/* ---- fork safety: the emulator's lock hierarchy, written down once --------
 *
 * fork(2) duplicates the calling thread alone, so a mutex a *sibling* held at
 * that instant crosses into the child locked and owned by a thread that does
 * not exist there. One pthread_atfork triple settles all of them: prepare takes
 * every lock (which also means no sibling is mid-mutation, so the child
 * inherits a settled page table rather than a half-rewritten one), parent
 * releases them, and the child re-initializes rather than unlocks, because fork
 * gives the surviving thread a new tid and a recursive mutex's recorded owner no
 * longer matches it.
 *
 * The order below IS the emulator's lock hierarchy, outermost first, and this is
 * the only place it is stated. It must agree with the order real code nests
 * them in, or a fork deadlocks against a sibling coming the other way: prepare
 * would hold an inner lock and wait for an outer one while the sibling holds the
 * outer and waits for the inner. as_lock is innermost because any critical
 * section that touches guest memory takes it underneath its own lock —
 * copy_to/from_guest → translate() takes it on a D-TLB miss, and
 * nl_take_request does that on every guest netlink request while holding
 * nl_lock. casp16 sits just above it (a CASP retry can miss the D-TLB), and
 * pf_lock above est_lock (the refresh path already holds pf_lock).
 *
 * This used to be five separate registrations, one per module, which encoded the
 * same hierarchy in the *reverse* order of five adjacent calls — sorting them
 * was enough to deadlock every fork. tests/fixtures/forklock.c is the guard. */
static void emu_atfork_prepare(void) {
    sig_helpers_park();      /* the emulator's own threads asleep (signal.c),
                              * before any lock one of them may still want */
    jit_locks_take();        /* jit stats  — outermost */
    procfs_locks_take();     /* pf_lock, then est_lock */
    netlink_locks_take();    /* nl_lock    — taken above as_lock by real code */
    sig_locks_take();        /* sfd_lock */
    sigact_locks_take();     /* sigact_lock — inside sfd_lock by rank (signal.c) */
    thr_locks_take();        /* thr_lock — the robust walk copies guest memory */
    task_locks_take();       /* task_lock — nothing nests inside it */
    mem_locks_take();        /* casp16, then as_lock — innermost */
    fdheld_fork_prepare();   /* the fd-window barrier: last, so that a window
                              * is never waiting on a lock this already holds
                              * (machine.h, "the emulator's own descriptors") */
}
static void emu_atfork_parent(void) {
    fdheld_fork_parent();
    mem_locks_drop();        /* innermost first, mirroring prepare */
    task_locks_drop();
    thr_locks_drop();
    sigact_locks_drop();
    sig_locks_drop();
    netlink_locks_drop();
    procfs_locks_drop();
    jit_locks_drop();
    sig_helpers_release();
}
static void emu_atfork_child(void) {
    sig_helpers_fork_child(); /* none came across */
    ptrace_helpers_fork_child();
    /* Not a lock: the shared registry's publisher caches a pointer to this
     * process's own slot, and a fork child inherits one that names its
     * parent's. Cleared here rather than where the child adopts its own slot,
     * because a fork that found no free slot never gets there. */
    proctab_fork_child();
    fdheld_fork_child();     /* close what sibling threads held (machine.h) */
    mem_locks_reinit();
    task_locks_reinit();
    thr_locks_reinit();
    sigact_locks_reinit();
    sig_locks_reinit();
    netlink_locks_reinit();
    procfs_locks_reinit();
    jit_locks_reinit();
    prof_reset();            /* ARM64EMU_PROF: the child's own profile */
}
static void emu_atfork_init(void) {
    pthread_atfork(emu_atfork_prepare, emu_atfork_parent, emu_atfork_child);
}

/* Sandbox diagnosis: report an inherited seccomp filter (Seccomp: 2 in
 * /proc/self/status — the Android app sandbox is the common case). Under one,
 * a blocked host syscall raises SIGSYS instead of returning ENOSYS; the net
 * armed in chroot_main() converts those, and this one-liner answers the first
 * question in any "works on one device, dies on another" report. An embedded
 * build (-DANDROID_JNI) runs in an app's sandbox by definition, so it never
 * says so. */
#ifndef ANDROID_JNI
static void seccomp_notice(void) {
    FILE *f = fopen("/proc/self/status", "re");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "Seccomp:", 8)) {
            if (atoi(line + 8) == 2)
                fprintf(stderr, "arm64emu: seccomp filter active on this "
                                "process; trapped host syscalls return "
                                "ENOSYS\n");
            break;
        }
    }
    fclose(f);
}
#endif

/* --- command-line option parsing helpers ------------------------------- */

/* Append a guest -E/--env "VAR=VAL" entry. The name is everything before the
 * first '=', so a value may contain more of them. A spec with no '=' at all is
 * refused rather than guessed at: the bare string in envp is an entry no
 * getenv() can ever match, so a mistyped name would do nothing at all. A
 * repeated name overwrites the earlier entry in place instead of adding a
 * duplicate, which getenv() (first match) and a shell re-exporting envp (last
 * match) would disagree about. */
static void push_env(char ***env, int *n, char *v) {
    const char *eq = strchr(v, '=');
    if (!eq || eq == v) {
        fprintf(stderr, "arm64emu: --env '%s': expected VAR=VAL\n", v);
        exit(2);
    }
    size_t kl = (size_t)(eq - v) + 1;   /* compare through the '=' */
    for (int k = 0; k < *n; k++)
        if (!strncmp((*env)[k], v, kl)) { (*env)[k] = v; return; }
    char **nv = realloc(*env, sizeof(char *) * (size_t)(*n + 1));
    if (!nv) { perror("arm64emu: realloc"); exit(127); }
    *env = nv;
    (*env)[(*n)++] = v;
}

/* Collect a --bind spec, parsed once the tier is known. */
static void push_bind(char ***specs, int *n, char *v) {
    char **nv = realloc(*specs, sizeof(char *) * (size_t)(*n + 1));
    if (!nv) { perror("arm64emu: realloc"); exit(127); }
    *specs = nv;
    (*specs)[(*n)++] = v;
}

/* Apply -u/--fake-id: enable fake identity and parse its optional ID. `attached`
 * is the text glued to the flag ("--fake-id=ID" or "-uID"), NULL/"" when none;
 * absent, the next argv token is taken as the ID iff it looks like an id-spec
 * (the "-u 1000:1000" space-separated form). A bad attached spec is fatal. */
static void set_fake_id(struct Machine *m, const char *attached,
                        char **argv, int argc, int *pi, u32 *uid, u32 *gid) {
    m->fake_id = 1;
    if (attached && *attached) {
        if (!is_id_spec(attached)) {
            fprintf(stderr, "arm64emu: --fake-id '%s': expected UID or UID:GID\n",
                    attached);
            exit(2);
        }
        parse_id_spec(attached, uid, gid);
    } else if (*pi + 1 < argc && is_id_spec(argv[*pi + 1])) {
        parse_id_spec(argv[++*pi], uid, gid);
    }
}

/* chdir the host into the guest directory `canon` (namespace-absolute): the
 * host cwd is the guest's from then on (path.c). 0, or 126 after a message. */
static int enter_guest_cwd(struct Machine *m, const char *canon) {
    char host[PATH_MAX];
    int r = path_resolve(m, G_AT_FDCWD, canon, 0, host, NULL);
    if (r == 0 && chdir(host) != 0) r = -errno;
    if (r < 0) {
        fprintf(stderr, "arm64emu: cannot enter the working directory '%s': %s\n",
                canon, strerror(-r));
        return 126;
    }
    return 0;
}

/* --native that cannot be used: why, and what runs the guest instead. The
 * emulator, with --jit -- unless what made the tier unusable is a step
 * diagnostic, which needs the interpreter (engine_settle). A test aid makes
 * it the end of the run instead (ARM64EMU_NATIVE_NO_FALLBACK). */
static void native_fallback(const char *why, bool per_insn) {
    const char *strict = arm64_getenv("NATIVE_NO_FALLBACK");
    if (strict && *strict && strcmp(strict, "0")) {
        fprintf(stderr, "arm64emu: --native: %s\n", why);
        exit(126);
    }
    fprintf(stderr, "arm64emu: --native: %s; continuing with %s\n", why,
            per_insn ? "the interpreter" : "--jit");
    if (!per_insn) g_jit = 1;
}

/* Why --native cannot be what runs this command line, before the tier is
 * asked anything (it asks the rest itself: native_run): no tier in this build,
 * or an option or a knob that needs the emulator. NULL when there is none. */
static const char *native_unusable(struct Machine *m, char *buf, size_t n) {
    if (!native_run)
        return "this build has no native tier (it runs on AArch64 hosts only)";
    const char *opt = m->strace_full ? "--strace-full" : m->strace ? "--strace"
                    : g_trace ? "--debug" : g_rtrace ? "--reg-trace"
                    : g_max_insn ? "--max-insn" : NULL;
    if (!opt) {
        static const char *const knobs[] = { "PROF", "RING", "TPC", "COV" };
        for (unsigned k = 0; k < sizeof knobs / sizeof *knobs; k++) {
            const char *v = arm64_getenv(knobs[k]);
            if (v && *v) {
                snprintf(buf, n, "ARM64EMU_%s needs the emulator", knobs[k]);
                return buf;
            }
        }
        return NULL;
    }
    snprintf(buf, n, "%s needs the emulator", opt);
    return buf;
}

/* Chroot mode's entry (../main.c): argv[0] is the mode's name. */
int chroot_main(int argc, char **argv)
{
    struct Machine *m = &g_machine;
    const char *argv0 = NULL;
    const char *work_dir = NULL;
    char **extra_env = NULL;
    int n_extra = 0;
    char **bind_specs = NULL;
    int n_binds = 0;
    int native = 0;

    u32 fake_uid = 0, fake_gid = 0;
    int fake_explicit = 0;   /* -u/--fake-id given, not only implied */

    /* GNU-style options: single-letter short (-j), --word long. Value-taking
     * options accept "-b VAL"/"-bVAL" and "--bind VAL"/"--bind=VAL"; no-arg
     * shorts bundle ("-dl"). Parsing stops at the first non-option, at "--", or
     * at the <rootfs> argument, so guest args are never consumed as options. */
    int i = 1;
    for (; i < argc; i++) {
        char *arg = argv[i];
        if (arg[0] != '-' || arg[1] == '\0') break;   /* positional (or lone "-") */
        if (!strcmp(arg, "--")) { i++; break; }        /* explicit end of options */

        if (arg[1] == '-') {                           /* long option: --name[=val] */
            char *eq = strchr(arg, '='), *val = NULL;
            if (eq) { *eq = '\0'; val = eq + 1; }      /* argv is writable */
            const char *n = arg + 2;
            if      (!strcmp(n, "help"))         { if (val) opt_no_value(arg); help(); }
            else if (!strcmp(n, "version"))      { if (val) opt_no_value(arg); version(); }
            else if (!strcmp(n, "native"))       { if (val) opt_no_value(arg); native = 1; }
            else if (!strcmp(n, "strace"))       { if (val) opt_no_value(arg); m->strace = 1; }
            else if (!strcmp(n, "strace-full"))   { if (val) opt_no_value(arg); m->strace = 1; m->strace_full = 1; }
            else if (engine_long_opt(n, val, arg, argv, argc, &i)) ;
            else if (!strcmp(n, "link2symlink")) { if (val) opt_no_value(arg); m->link2symlink = 1; }
            else if (!strcmp(n, "shared-proc"))  { if (val) opt_no_value(arg); m->shared_proc = 1; }
            else if (!strcmp(n, "no-dev"))       { if (val) opt_no_value(arg); m->no_dev = 1; }
            else if (!strcmp(n, "no-proc"))      { if (val) opt_no_value(arg); m->no_proc = 1; }
            else if (!strcmp(n, "share-abstract-sockets")) { if (val) opt_no_value(arg); m->share_abstract = 1; }
            else if (!strcmp(n, "keep-fds"))     { if (val) opt_no_value(arg); m->keep_fds = 1; }
            else if (!strcmp(n, "host-keyring")) { if (val) opt_no_value(arg); m->host_keyring = 1; }
            else if (!strcmp(n, "bind"))   push_bind(&bind_specs, &n_binds, long_value("--bind", val, argv, argc, &i));
            else if (!strcmp(n, "env"))    push_env(&extra_env, &n_extra, long_value("--env", val, argv, argc, &i));
            else if (!strcmp(n, "argv0"))  argv0 = long_value("--argv0", val, argv, argc, &i);
            else if (!strcmp(n, "work-dir")) work_dir = long_value("--work-dir", val, argv, argc, &i);
            else if (!strcmp(n, "fake-id")) { set_fake_id(m, val, argv, argc, &i, &fake_uid, &fake_gid); fake_explicit = 1; }
            else if (!strcmp(n, "setuid-root")) { if (val) opt_no_value(arg); m->fake_id = 1; m->setuid_root = 1; }
            else if (!strcmp(n, "setgid-root")) { if (val) opt_no_value(arg); m->fake_id = 1; m->setgid_root = 1; }
            else if (!strcmp(n, "file-backed")) { if (val) opt_no_value(arg); m->file_backed = 1; }
            else usage();
        } else {                                       /* short cluster: -abc */
            for (char *p = arg + 1; *p; ) {
                char c = *p++;
                if      (c == 'h') help();
                else if (c == 'v') version();
                else if (engine_short_opt(c)) ;
                else if (c == 'l') m->link2symlink = 1;
                else if (c == 'b') { push_bind(&bind_specs, &n_binds, short_value("-b", p, argv, argc, &i)); break; }
                else if (c == 'E') { push_env(&extra_env, &n_extra, short_value("-E", p, argv, argc, &i)); break; }
                else if (c == '0') { argv0 = short_value("-0", p, argv, argc, &i); break; }
                else if (c == 'w') { work_dir = short_value("-w", p, argv, argc, &i); break; }
                else if (c == 'u') { set_fake_id(m, p, argv, argc, &i, &fake_uid, &fake_gid); fake_explicit = 1; break; }
                else if (c == 'F') m->file_backed = 1;
                else usage();
            }
        }
    }
    if (argc - i < 2) usage();

    const char *rootfs = argv[i];
    const char *prog = argv[i + 1];
    char **gargv_in = &argv[i + 1];

    /* An empty <rootfs> is almost always an unset variable -- `arm64emu chroot
     * "$ROOTFS" rm -rf /` -- and "/" is how the host's root is asked for on
     * purpose. */
    if (!*rootfs) {
        fprintf(stderr, "arm64emu: <rootfs> is empty (use '/' for the host root)\n");
        return 2;
    }

    /* The --bind specs, parsed (and refused, when malformed) the same way for
     * both tiers; each tier registers them its own way below. */
    char (*bind_host)[PATH_MAX] = NULL, (*bind_guest)[PATH_MAX] = NULL;
    int *bind_ro = NULL;
    if (n_binds) {
        bind_host = malloc(sizeof *bind_host * (size_t)n_binds);
        bind_guest = malloc(sizeof *bind_guest * (size_t)n_binds);
        bind_ro = malloc(sizeof *bind_ro * (size_t)n_binds);
        if (!bind_host || !bind_guest || !bind_ro) { perror("arm64emu: malloc"); exit(127); }
        for (int k = 0; k < n_binds; k++)
            parse_bind(bind_specs[k], bind_host[k], bind_guest[k], &bind_ro[k]);
    }

    /* The step diagnostics (--debug, --reg-trace, ARM64EMU_PROF/RING/TPC/COV)
     * are the per-instruction facilities here; --jit yields to them and to a
     * host without a code generator, the interpreter being always the correct
     * fallback. --max-insn needs none of that: every engine stops at it. */
    step_diag_env();
    bool per_insn = cpu_step_diag_on();

    /* Capture the real invoking identity (stat remap source) and, in fake mode,
     * seed the credential set to the configured identity. */
    m->host_uid = getuid();
    m->host_gid = getgid();
    /* An identity only implied by --setuid-root/--setgid-root is the invoking
     * one, not 0:0: the flags let set-id binaries raise it without making the
     * guest root from the start. An explicit -u/--fake-id wins. */
    if (m->fake_id && !fake_explicit) {
        fake_uid = m->host_uid;
        fake_gid = m->host_gid;
    }

    if (!realpath(rootfs, m->rootfs)) {
        fprintf(stderr, "arm64emu: rootfs '%s': not found\n", rootfs);
        return 126;
    }

    /* Guest argv: program args as given; argv[0] overridable. */
    int gargc = 0;
    while (gargv_in[gargc]) gargc++;
    char **gargv = malloc(sizeof(char *) * (size_t)(gargc + 1));
    if (!gargv) { perror("arm64emu: malloc"); exit(127); }
    for (int k = 0; k < gargc; k++) gargv[k] = gargv_in[k];
    gargv[gargc] = NULL;
    if (argv0) gargv[0] = (char *)argv0;

    /* Guest environ: a clean environment, the same for both tiers. Only the
     * host's terminal-appearance variables (TERM, COLORTERM) are inherited;
     * every other host variable (PATH, HOME, LD_*, XDG_*, ...) names host
     * locations, not guest ones, and is dropped. What the guest gets instead of
     * the dropped PATH is a guest-side default (guest_default below), not the
     * host's; HOME stays unset, as it is in a process nothing logged in (a
     * login shell, su -, or -E sets it), since no home directory can be assumed
     * for whatever identity the guest runs as.
     * -E/--env entries come first and win: a host var is inherited, and a
     * default supplied, only when no -E entry already sets that key. (Emitting
     * both as duplicates is not enough -- getenv() would return the -E copy but
     * a shell importing envp keeps the *last* duplicate, i.e. the other value,
     * defeating the override.) Callers re-add anything else they need with -E.
     *
     * Dropping PATH without replacing it was not neutral: it left the guest with
     * no PATH at all, which nothing on a real system ever has. Programs that
     * search it themselves rather than going through execvp(3) then fail before
     * they exec anything -- gcc's collect2 looks for `ld` over COMPILER_PATH +
     * $PATH and dies with "cannot find 'ld'", so compiling in a Debian rootfs
     * needed an explicit -E PATH=... The value is the one a login sets for
     * root, login.defs ENV_SUPATH: a superset of the user one, and nothing in
     * it is outside the rootfs. */
    static const char *const host_keep[] = { "TERM=", "COLORTERM=" };
    static const char *const guest_default[] = {
        "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
    };
    int n_keep = (int)(sizeof host_keep / sizeof *host_keep);
    int n_def  = (int)(sizeof guest_default / sizeof *guest_default);
    char **genv = malloc(sizeof(char *) * (size_t)(n_extra + n_keep + n_def + 1));
    if (!genv) { perror("arm64emu: malloc"); exit(127); }
    int ge = 0;
    for (int k = 0; k < n_extra; k++) genv[ge++] = extra_env[k];
    for (int k = 0; k < n_keep; k++) {
        if (env_set_by_flag(extra_env, n_extra, host_keep[k])) continue;
        size_t kl = strlen(host_keep[k]);
        for (char **e = environ; *e; e++)
            if (!strncmp(*e, host_keep[k], kl)) { genv[ge++] = *e; break; }
    }
    for (int k = 0; k < n_def; k++)
        if (!env_set_by_flag(extra_env, n_extra, guest_default[k]))
            genv[ge++] = (char *)guest_default[k];
    genv[ge] = NULL;

    /* --native: the guest's code on this CPU, in this process. It returns only
     * when it could not be entered: a fallback, before it did anything the
     * emulator below would mind, or a failure the emulator would meet too. */
    if (native) {
        char why[512];
        const char *no = native_unusable(m, why, sizeof why);
        if (!no) {
            struct native_bind *nb = calloc((size_t)n_binds + 1, sizeof *nb);
            if (!nb) { perror("arm64emu: malloc"); exit(127); }
            for (int k = 0; k < n_binds; k++)
                nb[k] = (struct native_bind){ bind_host[k], bind_guest[k], bind_ro[k] };
            struct native_opts o = {
                .rootfs = m->rootfs, .workdir = work_dir, .prog = prog,
                .argv = gargv, .envp = genv, .binds = nb, .nbinds = n_binds,
                .fake_id = m->fake_id, .fake_explicit = fake_explicit,
                .fake_uid = fake_uid, .fake_gid = fake_gid,
                .setuid_root = m->setuid_root, .setgid_root = m->setgid_root,
                .link2symlink = m->link2symlink, .file_backed = m->file_backed,
                .no_proc = m->no_proc, .no_dev = m->no_dev,
                .keep_fds = m->keep_fds, .host_keyring = m->host_keyring,
                .share_abstract = m->share_abstract,
                .shared_proc = m->shared_proc,
            };
            int r = native_run(&o, why, sizeof why);
            if (r != NATIVE_FALLBACK) return r;
            free(nb);
            no = why;
        }
        native_fallback(no, per_insn);
    }

    g_debug_hooks = per_insn;
    engine_settle(g_debug_hooks);
    l2s_host = &l2s_chroot_ops;   /* --link2symlink's host: the emulator's */

    if (m->fake_id) {
        m->fake_uid = fake_uid;
        m->fake_gid = fake_gid;
        m->cred.ruid = m->cred.euid = m->cred.suid = m->cred.fsuid = fake_uid;
        m->cred.rgid = m->cred.egid = m->cred.sgid = m->cred.fsgid = fake_gid;
        m->cred.ngroups = 0;
    }

    /* Create the process-shared bind table before the --bind registration
     * below and before the first fork, so every guest process maps the same
     * mounts. */
    bindtab_init();
    for (int k = 0; k < n_binds; k++)
        add_bind(m, bind_specs[k], bind_host[k], bind_guest[k], bind_ro[k]);
    free(bind_host);
    free(bind_guest);
    free(bind_ro);

    if (!strcmp(m->rootfs, "/")) m->rootfs[0] = 0;   /* rootfs "/": no prefix */

    /* Per-rootfs tag spliced into guest abstract AF_UNIX socket names to isolate
     * them from the host and from other rootfs instances (sys_net.c). Same
     * rootfs -> same tag, so guest processes still rendezvous. The 0x01 lead
     * makes a collision with a real host abstract name essentially impossible. */
    m->abs_tag[0] = 0x01;
    int atn = snprintf(m->abs_tag + 1, sizeof m->abs_tag - 1, "a64%08x",
                       (unsigned)fnv1a32(m->rootfs));
    m->abs_tag_len = (u8)(1 + atn);

    /* Per-invocation nonce keying the System V shm broker's rendezvous when
     * --shared-proc is off (machine.h / proctab.c). The root pid makes it unique
     * among live invocations; mixing in the start time keeps a pid reused within
     * a dying daemon's grace window from rejoining the old namespace. Seeded once
     * here in the root process and fork-inherited, so one launch's whole process
     * tree shares one shm namespace while separate launches stay isolated. */
    struct timespec sts;
    clock_gettime(CLOCK_REALTIME, &sts);
    m->shm_session = ((u64)getpid() << 32) ^
                     ((u64)sts.tv_sec * 1000000000ull + (u64)sts.tv_nsec);

    /* Not chrooted initially: the guest root is the rootfs root. */
    strcpy(m->chroot_base, "/");

    /* Guest cwd: the host cwd when it lies inside the rootfs, else "/" -- and
     * the host process moves there, because from here on the host cwd IS the
     * guest's (path.c, "the working directory is the host's"). Every host
     * path the emulator uses is absolute by now (the rootfs and the bind
     * sources are realpath'd above), so nothing of its own depends on where
     * it was launched from. */
    strcpy(m->cwd, "/");
    char hcwd[PATH_MAX];
    if (getcwd(hcwd, sizeof hcwd)) {
        size_t rl = strlen(m->rootfs);
        if (!strncmp(hcwd, m->rootfs, rl) && (hcwd[rl] == '/' || hcwd[rl] == 0))
            strcpy(m->cwd, hcwd[rl] ? hcwd + rl : "/");
    }
    if (enter_guest_cwd(m, m->cwd) != 0) return 126;

    /* -w/--work-dir overrides the initial cwd with a guest path (resolved inside
     * the rootfs, honoring -bind and symlinks). An absolute DIR resolves from
     * guest "/", a relative one against the default cwd above -- which the host
     * is standing in by now. Must land before do_execve, which resolves
     * <program> against the cwd. Fatal on a bad path. */
    if (work_dir) {
        char host[PATH_MAX], canon[PATH_MAX];
        struct stat st;
        int r = path_resolve(m, G_AT_FDCWD, work_dir, 0, host, canon);
        if (r == 0 && stat(host, &st) < 0) r = -errno;
        else if (r == 0 && !S_ISDIR(st.st_mode)) r = -ENOTDIR;
        if (r < 0) {
            fprintf(stderr, "arm64emu: work-dir '%s': %s\n",
                    work_dir, strerror(-r));
            return 126;
        }
        strcpy(m->cwd, canon);
        if (enter_guest_cwd(m, m->cwd) != 0) return 126;
    }

    m->dumpable = 1;  /* SUID_DUMP_USER, as a process starts (sys_proc.c prctl) */
    rlim_init(m);     /* seed the guest's resource limits from the host's,
                       * before anything can consult or change them */
    as_init(&m->as);
    /* Before any handler is installed or can fire: on Bionic (emulated TLS)
     * the first access to a __thread variable mallocs, and a signal handler
     * must never be the first toucher (sig_tls_prewarm). */
    sig_tls_prewarm();
    as_bus_init();   /* host SIGBUS on a file truncated from outside this
                      * address space becomes the guest's own abort */
    m->cpu.m = m;
    as_cpu_attach(&m->cpu);   /* this thread runs the main CPU: its caches */
    g_tls.tid = getpid();     /* main thread tid == pid */

    /* Arm the SIGSYS net (seccomp trap -> -ENOSYS) before any guest work:
     * the ELF loader below already forwards host syscalls. */
    sig_install_sigsys_net();
    sig_install_sync_nets();
    /* The main thread's personality and its place in the thread registry,
     * before the exec below judges it (and after the net, which is what a
     * seccomp filter refusing the host call lands on). */
    g_tls.personality = pers_initial();
    thr_reg_add(g_tls.tid, g_tls.personality);
    /* Which RT signals this host will let us reserve, before anything installs
     * a handler on one or sends one. A number the host accepts but never
     * delivers would turn the kick below -- and every other wake-up riding it
     * -- into a hang, so it is measured rather than assumed. */
    sig_probe_reserved();
    /* Arm the ptrace attach stop-kick net (reserved RT signal) so a later
     * PTRACE_ATTACH/SEIZE/INTERRUPT can stop this process cooperatively. */
    sig_install_kick_net();
    sig_kick_timer_init();   /* the capture kick's timer for the main thread:
                              * aimed at the reserved number just chosen */
    sig_inherit_host_mask(m);   /* the guest starts with the mask it was given
                                 * (execve keeps it), and the host's is the
                                 * guest's from here on -- after the reserved
                                 * numbers are known, which it holds out */
    sig_inherit_host_dispositions(m);   /* likewise SIG_IGN; and every
                                         * default-terminate signal caught
                                         * from here on, not from the guest's
                                         * first sigaction on it */
    /* Make every process-local mutex fork-safe before there is a second thread
     * to hold one (mem.c carries the full story and the hang it cost). */
    mem_locks_init();
    emu_atfork_init();

#ifndef ANDROID_JNI
    seccomp_notice();
#endif

    /* Shared guest-PID registry: must exist before the first do_execve (which
     * registers this process) and before any fork inherits the mapping. With
     * -shared-proc the registry is keyed by the rootfs so ps/top see the guest
     * processes of other emulator invocations of the same rootfs. */
    proctab_init(m->shared_proc ? m->rootfs : NULL);

    /* Backing directories of emulated tmpfs mounts (path.c) are removed when
     * this invocation ends; sweep what invocations killed before they could do
     * that left behind, so they cannot pile up. */
    tmpfs_sweep_stale();

    /* ptrace(2) tracer<->tracee link registry: same discipline — created before
     * the first fork so every guest process in the session maps it. */
    ptrace_init();

    /* What execve's CLOEXEC sweep may not close: read now, while the limit is
     * still the one this process started with and no guest code has run. */
    guest_fd_ceiling_init();
    /* And what the caller left open below it: closed before any guest code can
     * see it. Guest fd is host fd, so an inherited descriptor is a handle onto
     * a host file the rootfs does not contain -- readable and writable by
     * number, with its host path readable through /dev/fd. --keep-fds is for a
     * caller passing one in deliberately. Everything the emulator itself needs
     * from here on is a mapping, not a held descriptor (proctab.c). */
    if (!m->keep_fds) guest_fd_close_inherited();
    /* Likewise for the host tasks in our thread group that are not ours: name
     * them now, while "this process has one thread" is still a fact. Everything
     * that reads the host task list as the guest thread list depends on it. */
    proc_foreign_sample();

    /* Route the initial exec through do_execve so shebang scripts and PATH-less
     * relative programs behave exactly as an in-guest execve would. */
    if (argv0) gargv[0] = (char *)argv0;
    ExecVec av = { gargv, 0 }, ev = { genv, 0 };
    u64 r = do_execve(&m->cpu, prog, av, ev);
    if ((s64)r < 0) {
        fprintf(stderr, "arm64emu: cannot execute %s: %s\n", prog,
                strerror((int)-(s64)r));
        return 126;
    }

    return emu_loop(&m->cpu);
}
