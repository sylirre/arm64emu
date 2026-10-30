# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Sylirre
#
# The one build of this repository's three programs:
#
#   arm64emu     AArch64 full-system emulator              src/arm64emu
#   arm64chroot  AArch64 Linux user-space emulator         src/arm64chroot
#   chroot-ng    in-process chroot for AArch64 hosts       src/chroot-ng
#
# and of the code they share in lib/: lib/arm64 is the AArch64 emulation both
# emulators link. A library is compiled once per program that links it, with
# that program's flags — there is no separately built archive, because the
# programs are built for different targets with different language modes.
#
# Everything this file produces lands under build/ (and nowhere else):
#   build/<program>          the programs, plus arm64chroot32 and arm64chroot_asim
#   build/obj/<tree>/        objects and .d files, one tree per build flavour
#   build/tests/<program>/   the programs the test suites build
#   build/cache/<program>/   provisioned test rootfs images and recordings,
#                            which `make clean` keeps (see clean-testenv)
#
# `make` builds every program this host can build: chroot-ng needs an AArch64
# C compiler (native, or the aarch64-linux-gnu cross one), arm64emu a 64-bit
# host. `make test` runs every program's suite, one after another. `make help`
# lists the rest.
#
# Variables: CC, CFLAGS and LDFLAGS are the host compiler and extra flags for
# the two emulators. Everything specific to one program carries its prefix:
# ARM64EMU_*, ARM64CHROOT_*, CHROOTNG_* (chroot-ng is cross-built, so it has a
# compiler and flags of its own: CHROOTNG_CC, CHROOTNG_CFLAGS, CHROOTNG_LDFLAGS).

BUILD    := build
OBJDIR   := $(BUILD)/obj
HOSTARCH := $(shell uname -m)

# The version of all three programs, defined in one place: the VERSION file.
# Only the units that print it get it on their command line, and they depend on
# the file, so a bump rebuilds exactly them (see each program's *_VERSIONED).
PROJECT_VERSION := $(shell cat VERSION)
VERSION_DEFINE  := -DPROJECT_VERSION='"$(PROJECT_VERSION)"'

# Code shared by both emulators.
LIB_ARM64 := $(wildcard lib/arm64/*.c)

# $(call cfgstamp,TREE,TEXT): the path of a stamp file holding TEXT, rewritten
# only when TEXT changes. Every object of TREE depends on it, so changing the
# compiler or the flags (CC=clang, a cross compiler, LTO) rebuilds that tree
# instead of linking objects built with the old ones against new ones.
cfgstamp = $(shell mkdir -p $(OBJDIR)/$(1); \
	printf '%s\n' '$(2)' > $(OBJDIR)/$(1)/.cfg.tmp; \
	cmp -s $(OBJDIR)/$(1)/.cfg.tmp $(OBJDIR)/$(1)/.cfg 2>/dev/null \
	  || mv $(OBJDIR)/$(1)/.cfg.tmp $(OBJDIR)/$(1)/.cfg; \
	rm -f $(OBJDIR)/$(1)/.cfg.tmp; echo $(OBJDIR)/$(1)/.cfg)

DEPFLAGS := -MMD -MP

.PHONY: all help clean clean-testenv test
.DEFAULT_GOAL := all

# ============================================================================
# arm64emu — AArch64 full-system emulator. C11 + libc/POSIX only; networking
# through the built-in usernet stack (src/arm64emu/net/), no external libraries.
# ============================================================================

# -fno-math-errno lets __builtin_sqrt/fabs inline to hardware FP ops without
# errno handling. libm (-lm) is linked for __builtin_fma/fmaf: the interpreter's
# FMADD/FMLA use a genuinely fused multiply-add (single rounding, matching
# AArch64), which the compiler turns into a hardware FMA under -march=native and
# otherwise into a libm fma() call. Building with -march=native drops the call.
#
# Build-flag survey (TODO_OPTIMIZATIONS #14, 2026-07-19, gcc 13 x86-64):
# -O3/-flto is ~5% faster for --jit and neutral for the interpreter, but the
# predecoded tier's computed-goto dispatch TU regresses ~20% under it, and
# -flto spellings aren't portable across gcc/clang — so the default stays plain
# -O2 and `make ARM64EMU_OPT="-O3 -flto=8"` is the supported opt-in fast recipe.
# The pin on predecode.c below keeps it at -O2 in that case (a no-op for the
# default build). -mtune=native measured neutral; PGO not pursued (adds a
# two-phase build for a workload-specific gain).
ARM64EMU_OPT    ?= -O2
ARM64EMU_FLAGS   = -std=c11 $(ARM64EMU_OPT) -Wall -Wextra -Wno-unused-parameter \
                   -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
                   -Isrc/arm64emu -Ilib/arm64 -fno-math-errno -g $(CFLAGS)
ARM64EMU_SRC    := $(wildcard src/arm64emu/*.c src/arm64emu/devices/*.c \
                     src/arm64emu/fdt/*.c src/arm64emu/net/*.c \
                     src/arm64emu/jit/*.c) $(LIB_ARM64)
ARM64EMU_OBJ    := $(ARM64EMU_SRC:%.c=$(OBJDIR)/arm64emu/%.o)
# The guest-RAM model and the JIT use __int128, so only a 64-bit host builds it.
ARM64EMU_HOST64 := $(shell printf '__SIZEOF_POINTER__\n' | \
                     $(CC) $(CFLAGS) -E -P -x c - 2>/dev/null | tr -d ' \n')
ARM64EMU_CFG     = $(call cfgstamp,arm64emu,$(CC) $(ARM64EMU_FLAGS))

$(OBJDIR)/arm64emu/%.o: %.c $(ARM64EMU_CFG)
	@mkdir -p $(@D)
	$(CC) $(ARM64EMU_FLAGS) $(DEPFLAGS) -c -o $@ $<
$(OBJDIR)/arm64emu/src/arm64emu/jit/predecode.o: override ARM64EMU_OPT := -O2

ARM64EMU_VERSIONED := $(addprefix $(OBJDIR)/arm64emu/,src/arm64emu/main.o) \
                      $(addprefix $(OBJDIR)/arm64emu-embed/,src/arm64emu/main.o)
$(ARM64EMU_VERSIONED): VERSION
$(ARM64EMU_VERSIONED): ARM64EMU_FLAGS += $(VERSION_DEFINE)

$(BUILD)/arm64emu: $(ARM64EMU_OBJ)
	$(CC) $(ARM64EMU_FLAGS) $(LDFLAGS) -o $@ $^ -lm

# Embedded configuration. An app that links the emulator in builds it with
# -DANDROID_JNI: main() becomes arm64emu_main() so the host can call it after
# fork(), and fatal paths leave through _exit() so they never run the host's
# atexit handlers. Its own object tree, since the flags differ; the stub main
# stands in for the caller a real embedder supplies.
ARM64EMU_EMBED_OBJ := $(ARM64EMU_SRC:%.c=$(OBJDIR)/arm64emu-embed/%.o)
ARM64EMU_EMBED_CFG  = $(call cfgstamp,arm64emu-embed,$(CC) $(ARM64EMU_FLAGS) -DANDROID_JNI)

$(OBJDIR)/arm64emu-embed/%.o: %.c $(ARM64EMU_EMBED_CFG)
	@mkdir -p $(@D)
	$(CC) $(ARM64EMU_FLAGS) -DANDROID_JNI $(DEPFLAGS) -c -o $@ $<
$(OBJDIR)/arm64emu-embed/src/arm64emu/jit/predecode.o: override ARM64EMU_OPT := -O2

$(OBJDIR)/arm64emu-embed/stub.c:
	@mkdir -p $(@D)
	printf 'extern int arm64emu_main(int, char **);\nint main(int c, char **v) { return arm64emu_main(c, v); }\n' > $@

$(BUILD)/arm64emu-embed: $(ARM64EMU_EMBED_OBJ) $(OBJDIR)/arm64emu-embed/stub.c
	$(CC) $(ARM64EMU_FLAGS) $(LDFLAGS) -o $@ $^ -lm

# AArch64-backend validation without ARM hardware: a static aarch64 build
# (clang + lld + the Debian cross libc) run under qemu-user.
ARM64EMU_A64_SYSROOT ?= /usr/aarch64-linux-gnu
ARM64EMU_A64_CC      ?= clang --target=aarch64-linux-gnu -nostdlibinc \
                        -I$(ARM64EMU_A64_SYSROOT)/include -fuse-ld=lld -static \
                        -L$(ARM64EMU_A64_SYSROOT)/lib -B$(ARM64EMU_A64_SYSROOT)/lib

$(BUILD)/arm64emu-a64: $(ARM64EMU_SRC) VERSION
	$(ARM64EMU_A64_CC) -std=c11 $(ARM64EMU_OPT) -Wall -Wextra -Wno-unused-parameter \
	    -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -Isrc/arm64emu -Ilib/arm64 \
	    -fno-math-errno $(VERSION_DEFINE) -o $@ $(ARM64EMU_SRC) -lm

-include $(ARM64EMU_OBJ:.o=.d) $(ARM64EMU_EMBED_OBJ:.o=.d)

# ============================================================================
# arm64chroot — AArch64 Linux user-space emulator (interpreter + optional JIT).
# C11 + libc only; builds on x86_64/i386/arm/aarch64 Linux with GCC or Clang.
# ============================================================================

# LTO (opt-in: `make ARM64CHROOT_LTO=-flto`). It measured ~28% faster before
# the data-TLB and merged run loop landed, but those capture the same cross-TU
# seam in source; on top of them LTO measures slightly slower, so it is off by
# default.
ARM64CHROOT_LTO ?=
# -fno-math-errno: the FP core wants __builtin_sqrt to BE the hardware
# instruction (exec_fpsimd.c says so, and its exception flags are the guest's).
# Without it the compiler must keep a libm call for a possibly-negative operand
# just to set errno, which nothing here reads — and whether that call raises
# Invalid is then the libc's business, not ours.
#
# clang warns that the 16-byte atomics (guest CASP / 128-bit exclusives) are
# not lock-free. They are deliberate — the guest semantics need a single atomic
# access of that width — and libatomic's lock is the price. Silenced rather
# than left to make a clang build noisy; probed because gcc has no such option.
ARM64CHROOT_FLAGS = -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -fno-math-errno \
                    -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -D_TIME_BITS=64 \
                    -Isrc/arm64chroot -Isrc/arm64chroot/core -Isrc/arm64chroot/jit \
                    -Ilib/arm64 $(ARM64CHROOT_LTO) $(ARM64CHROOT_NOATOMICWARN) $(CFLAGS)
ARM64CHROOT_NOATOMICWARN := $(shell echo 'int main(){return 0;}' | $(CC) -Werror \
                    -Wno-atomic-alignment -x c - -o /dev/null 2>/dev/null \
                    && echo -Wno-atomic-alignment)

# The ILP32-host variant keeps 32-bit-host correctness continuously tested, and
# what "32-bit host" means depends on the machine doing the building: on x86-64
# it is the same compiler with -m32, on an AArch64 host it is armhf, which is a
# different compiler rather than a flag. `make test-arm64chroot32` checks both
# that the toolchain exists and that its output runs here before using it.
#
# -mfpmath=sse (x86 only): the i386 x87 default computes guest FP in 80-bit
# excess precision and, worse, transits every by-value double over FLD/FSTP,
# where an sNaN load signals invalid — poisoning the lazily accumulated FPSR
# flags (tests/arm64chroot/asm/m22_fpsr). SSE math makes the FP core behave
# exactly like the 64-bit build; every x86 host that can run this has SSE2.
ifeq ($(HOSTARCH),aarch64)
ARM64CHROOT_M32CC    ?= arm-linux-gnueabihf-gcc
ARM64CHROOT_M32FLAGS ?=
else
ARM64CHROOT_M32CC    ?= $(CC)
ARM64CHROOT_M32FLAGS ?= -m32 -msse2 -mfpmath=sse
endif

# The Android-behavior variant: A64_LINK2SYMLINK compiles in the
# emulated-hardlink scheme that __ANDROID__ enables automatically, and
# A64_L2S_FORCE makes --link2symlink take it even where the host allows real
# hardlinks. Both are inert unless the guest is run with --link2symlink, so they
# cost the rest of this variant nothing — and without them the scheme has no
# coverage anywhere off Android.
ARM64CHROOT_ASIMFLAGS := -DA64_STATX_FORCE_FALLBACK -DA64_KEYRING_ENOSYS \
                         -DA64_LINK2SYMLINK -DA64_L2S_FORCE

ARM64CHROOT_SRC := $(LIB_ARM64) \
    src/arm64chroot/core/cpu.c src/arm64chroot/core/decode.c \
    src/arm64chroot/jit/jit.c src/arm64chroot/jit/frontend.c \
    src/arm64chroot/jit/backend_x86_64.c src/arm64chroot/jit/backend_a64.c \
    src/arm64chroot/jit/backend_x86_32.c src/arm64chroot/jit/backend_arm32.c \
    $(addprefix src/arm64chroot/,mem.c exception.c loop.c predecode.c elf.c path.c \
      signal.c syscall.c strace.c sys_file.c sys_mm.c sys_ipc.c sys_proc.c \
      sys_sig.c sys_time.c sys_misc.c sys_net.c sys_netlink.c sys_procfs.c \
      sys_ptrace.c sys_seccomp.c proctab.c ptracetab.c main.c)
# (JIT backends self-select by host #ifdef — empty TUs elsewhere — so the list
# is host-independent and the 32-bit builds get the interpreter-only stub.)

ARM64CHROOT_OBJ      := $(ARM64CHROOT_SRC:%.c=$(OBJDIR)/arm64chroot/native/%.o)
ARM64CHROOT_M32_OBJ  := $(ARM64CHROOT_SRC:%.c=$(OBJDIR)/arm64chroot/m32/%.o)
ARM64CHROOT_ASIM_OBJ := $(ARM64CHROOT_SRC:%.c=$(OBJDIR)/arm64chroot/asim/%.o)
ARM64CHROOT_CFG       = $(call cfgstamp,arm64chroot/native,$(CC) $(ARM64CHROOT_FLAGS))
ARM64CHROOT_M32_CFG   = $(call cfgstamp,arm64chroot/m32,$(ARM64CHROOT_M32CC) $(ARM64CHROOT_FLAGS) $(ARM64CHROOT_M32FLAGS))
ARM64CHROOT_ASIM_CFG  = $(call cfgstamp,arm64chroot/asim,$(CC) $(ARM64CHROOT_FLAGS) $(ARM64CHROOT_ASIMFLAGS))

# 8/16-byte atomics on 32-bit hosts may need libatomic; harmless if unused.
# Probed with the compiler and flags that will actually link the variant, since
# the 32-bit one may be a different compiler entirely. Recursively expanded on
# purpose: the probe then runs when a link needs it, not on every `make`.
arm64chroot_atomic = $(shell echo 'int main(){return 0;}' | $(1) $(ARM64CHROOT_FLAGS) $(2) \
                       -latomic -x c - -o /dev/null 2>/dev/null && echo -latomic)

$(OBJDIR)/arm64chroot/native/%.o: %.c $(ARM64CHROOT_CFG)
	@mkdir -p $(@D)
	$(CC) $(ARM64CHROOT_FLAGS) $(DEPFLAGS) -c $< -o $@
$(OBJDIR)/arm64chroot/m32/%.o: %.c $(ARM64CHROOT_M32_CFG)
	@mkdir -p $(@D)
	$(ARM64CHROOT_M32CC) $(ARM64CHROOT_FLAGS) $(ARM64CHROOT_M32FLAGS) $(DEPFLAGS) -c $< -o $@
$(OBJDIR)/arm64chroot/asim/%.o: %.c $(ARM64CHROOT_ASIM_CFG)
	@mkdir -p $(@D)
	$(CC) $(ARM64CHROOT_FLAGS) $(ARM64CHROOT_ASIMFLAGS) $(DEPFLAGS) -c $< -o $@

ARM64CHROOT_VERSIONED := $(foreach v,native m32 asim,$(OBJDIR)/arm64chroot/$(v)/src/arm64chroot/main.o)
$(ARM64CHROOT_VERSIONED): VERSION
$(ARM64CHROOT_VERSIONED): ARM64CHROOT_FLAGS += $(VERSION_DEFINE)

$(BUILD)/arm64chroot: $(ARM64CHROOT_OBJ)
	$(CC) $(ARM64CHROOT_FLAGS) -o $@ $^ $(LDFLAGS) -lm -lpthread $(call arm64chroot_atomic,$(CC),)
$(BUILD)/arm64chroot32: $(ARM64CHROOT_M32_OBJ)
	$(ARM64CHROOT_M32CC) $(ARM64CHROOT_FLAGS) $(ARM64CHROOT_M32FLAGS) -o $@ $^ $(LDFLAGS) -lm -lpthread \
	    $(call arm64chroot_atomic,$(ARM64CHROOT_M32CC),$(ARM64CHROOT_M32FLAGS))
$(BUILD)/arm64chroot_asim: $(ARM64CHROOT_ASIM_OBJ)
	$(CC) $(ARM64CHROOT_FLAGS) $(ARM64CHROOT_ASIMFLAGS) -o $@ $^ $(LDFLAGS) -lm -lpthread \
	    $(call arm64chroot_atomic,$(CC),)

-include $(ARM64CHROOT_OBJ:.o=.d) $(ARM64CHROOT_M32_OBJ:.o=.d) $(ARM64CHROOT_ASIM_OBJ:.o=.d)

# ============================================================================
# chroot-ng — ptrace-free chroot/bind emulation for rootless Android (AArch64).
# Freestanding: no libc, our own _start. Built with an AArch64 compiler — the
# aarch64-linux-gnu cross toolchain on other hosts, run there under qemu-aarch64.
# ============================================================================

# The cross toolchain when one is installed, else the host compiler (on an
# AArch64 host, natively or Termux/NDK clang on-device, that is the right one;
# anywhere else the target check below finds it is not, and chroot-ng is
# skipped). CHROOTNG_CC set by hand always wins.
CHROOTNG_CROSS ?= aarch64-linux-gnu-
ifeq ($(origin CHROOTNG_CC),undefined)
ifneq ($(shell command -v $(CHROOTNG_CROSS)gcc-13 2>/dev/null),)
CHROOTNG_CC := $(CHROOTNG_CROSS)gcc-13
else ifneq ($(shell command -v $(CHROOTNG_CROSS)gcc 2>/dev/null),)
CHROOTNG_CC := $(CHROOTNG_CROSS)gcc
else
CHROOTNG_CC := $(CC)
endif
endif
# Non-empty when CHROOTNG_CC produces AArch64 code.
CHROOTNG_OK := $(filter aarch64% arm64%,$(shell $(CHROOTNG_CC) -dumpmachine 2>/dev/null))

# How to run an AArch64 binary here. On an AArch64 host (native Linux or Termux)
# that is "directly", so CHROOTNG_QEMU stays EMPTY and `$(CHROOTNG_QEMU) BIN` is
# just BIN; elsewhere whichever qemu-user build is installed.
# tests/chroot-ng/lib.sh does the same detection for the suite.
ifeq ($(filter aarch64 arm64,$(HOSTARCH)),)
CHROOTNG_QEMU ?= $(shell command -v qemu-aarch64-static 2>/dev/null \
                   || command -v qemu-aarch64 2>/dev/null || echo qemu-aarch64-static)
else
CHROOTNG_QEMU ?=
endif

# Is the compiler clang (Termux/NDK) rather than gcc? Some flags are gcc-only.
CHROOTNG_IS_CLANG := $(shell $(CHROOTNG_CC) --version 2>/dev/null | grep -ci clang)

# Only the compile-time halves of "freestanding, no PIE" belong here
# (-ffreestanding, -fno-pie); the link-only `-static -nostdlib -no-pie` live in
# the link flags below. They used to be listed in both, which is a silent no-op
# on gcc but makes clang (Termux/NDK) report `-no-pie` as "argument unused
# during compilation" — fatal in the one unit built with -Werror.
CHROOTNG_FLAGS = -O2 -g -std=gnu11 -ffreestanding -fno-pie \
                 -fno-stack-protector \
                 -fno-asynchronous-unwind-tables -fno-builtin \
                 -ffunction-sections -fdata-sections \
                 -Wall -Wextra -Wno-unused-parameter -Isrc/chroot-ng/include \
                 $(CHROOTNG_GCCONLY) $(CHROOTNG_INLINEATOMICS) -mgeneral-regs-only \
                 $(CHROOTNG_CFLAGS)
# Stop gcc turning our mem* loops into calls to themselves. Clang rejects this
# flag and instead honors -ffreestanding/-fno-builtin for the same effect.
ifeq ($(CHROOTNG_IS_CLANG),0)
CHROOTNG_GCCONLY := -fno-tree-loop-distribute-patterns
endif
# GCC 13+ defaults to -moutline-atomics, which routes every __atomic builtin
# through a libgcc helper (__aarch64_cas4_acq_rel) that a -nostdlib link has no
# way to resolve. The PID registry uses CAS, so ask for inline atomics — the
# LL/SC form works on every ARMv8. Probed, since not every compiler has it.
CHROOTNG_INLINEATOMICS := $(shell $(CHROOTNG_CC) -mno-outline-atomics -E -x c /dev/null \
                            >/dev/null 2>&1 && echo -mno-outline-atomics)
# -mgeneral-regs-only: no floating point or SIMD anywhere in the monitor. This
# is not a size or a purity argument: on the -R tier the trampoline calls
# straight into this C code on the guest's own register file, and the kernel
# preserves the FP registers across a syscall, so anything we spend there is
# taken from the guest. gcc does spend it — `cnt v0.8b` for a popcount, and
# `fmov d1, x0` as a cheap spill slot for a pointer — which cost a guest
# d0/d1/d16 across an openat (measured; see the sentinels in cng_rwtest_fn).
# Saving all 32 q registers per rewritten syscall would cost 512 bytes of stack
# traffic on the hot path and still leave the live registers wrong at a ptrace
# stop, where a stop with no signal frame reads and writes them as the guest's
# (NT_PRFPREG, src/chroot-ng/monitor/ptrace.c); not generating the instructions
# in the first place costs nothing and is enforced at build time. Deliberately
# not probed: silently dropping it would put the clobber back.

# Link chroot-ng far above the guest ET_EXEC range via an explicit linker
# script (src/chroot-ng/chroot-ng.ld). A non-PIE guest (notably gcc's cc1)
# loads at the fixed address 0x400000 — where a -no-pie executable also
# defaults to — so if chroot-ng lived there, MAP_FIXED-loading such a guest
# would overwrite our own monitor code. The script sets the location counter to
# 0x1000000000 (64 GiB) so the first segment *starts* there; the -T*/
# --image-base flags don't relocate a -no-pie binary cleanly on lld (they pad a
# segment up from 0x200000 instead).
#
# gcc links PIE by default on most distributions, so it needs -no-pie to keep
# the fixed-address ET_EXEC the script above lays out. Clang already implies
# non-PIE from -static (-static-pie being the opt-in), and says so by warning
# that -no-pie went unused, so it is only passed to gcc.
CHROOTNG_LDS     := src/chroot-ng/chroot-ng.ld
CHROOTNG_LDFLAGS ?=
CHROOTNG_LINK     = $(CHROOTNG_LDFLAGS) -static -nostdlib -Wl,-T,$(CHROOTNG_LDS) \
                    -Wl,--build-id=none -Wl,-z,noexecstack \
                    -Wl,--gc-sections -Wl,-e,_start \
                    $(if $(filter 0,$(CHROOTNG_IS_CLANG)),-no-pie)

CHROOTNG_SRC := $(shell find src/chroot-ng -name '*.c' -o -name '*.S' 2>/dev/null | LC_ALL=C sort)
CHROOTNG_OBJ := $(CHROOTNG_SRC:%=$(OBJDIR)/chroot-ng/%.o)
CHROOTNG_CFG  = $(call cfgstamp,chroot-ng,$(CHROOTNG_CC) $(CHROOTNG_FLAGS))

$(OBJDIR)/chroot-ng/%.c.o: %.c $(CHROOTNG_CFG)
	@mkdir -p $(@D)
	$(CHROOTNG_CC) $(CHROOTNG_FLAGS) $(DEPFLAGS) -c -o $@ $<
$(OBJDIR)/chroot-ng/%.S.o: %.S $(CHROOTNG_CFG)
	@mkdir -p $(@D)
	$(CHROOTNG_CC) $(CHROOTNG_FLAGS) $(DEPFLAGS) -c -o $@ $<

# The syscall table is ours (src/chroot-ng/include/cng/unistd.h); this unit
# holds it against the build host's <asm/unistd.h>, and a number the two spell
# differently is a macro redefinition — an error here, so it cannot be built past.
$(OBJDIR)/chroot-ng/src/chroot-ng/rt/unistd_check.c.o: CHROOTNG_FLAGS += -Werror

CHROOTNG_VERSIONED := $(addprefix $(OBJDIR)/chroot-ng/src/chroot-ng/,main.c.o run.c.o)
$(CHROOTNG_VERSIONED): VERSION
$(CHROOTNG_VERSIONED): CHROOTNG_FLAGS += $(VERSION_DEFINE)

# The linker script shapes the image as much as the objects do (its PHDRS is
# the whole program header table), so a change to it relinks.
$(BUILD)/chroot-ng: $(CHROOTNG_OBJ) $(CHROOTNG_LDS)
	$(CHROOTNG_CC) $(CHROOTNG_LINK) -o $@ $(CHROOTNG_OBJ)

-include $(CHROOTNG_OBJ:.o=.d)

# ============================================================================
# Programs
# ============================================================================

PROGRAMS := arm64chroot
ifeq ($(ARM64EMU_HOST64),8)
PROGRAMS += arm64emu
endif
ifneq ($(CHROOTNG_OK),)
PROGRAMS += chroot-ng
endif

all: $(PROGRAMS)
ifneq ($(ARM64EMU_HOST64),8)
	@echo "skipped arm64emu: it needs a 64-bit host compiler ($(CC))"
endif
ifeq ($(CHROOTNG_OK),)
	@echo "skipped chroot-ng: no AArch64 C compiler (install $(CHROOTNG_CROSS)gcc, or set CHROOTNG_CC)"
endif

.PHONY: arm64emu arm64emu-embed arm64emu-a64 arm64chroot arm64chroot32 arm64chroot_asim chroot-ng
arm64chroot: $(BUILD)/arm64chroot
arm64chroot32: $(BUILD)/arm64chroot32
arm64chroot_asim: $(BUILD)/arm64chroot_asim
ifeq ($(ARM64EMU_HOST64),8)
arm64emu: $(BUILD)/arm64emu
arm64emu-embed: $(BUILD)/arm64emu-embed
else
arm64emu arm64emu-embed:
	@echo "$@: arm64emu needs a 64-bit host compiler ($(CC))" >&2; exit 1
endif
arm64emu-a64: $(BUILD)/arm64emu-a64
ifneq ($(CHROOTNG_OK),)
chroot-ng: $(BUILD)/chroot-ng
else
chroot-ng:
	@echo "chroot-ng: no AArch64 C compiler (install $(CHROOTNG_CROSS)gcc, or set CHROOTNG_CC)" >&2; exit 1
endif

# ============================================================================
# Test suites
#
# Two suites never run at once: they share fixed scratch names (/tmp/t.bin)
# and timing-sensitive rows. `make test` runs one after another; naming several
# suites on one command line chains them the same way, even under -j, and each
# builds what it needs only once its turn comes.
# ============================================================================

SUITES := $(filter test-% fuzz-%,$(MAKECMDGOALS))
$(foreach n,$(wordlist 2,$(words $(SUITES)),$(shell seq 1 $(words $(SUITES)))),\
  $(eval $(word $(n),$(SUITES)): | $(word $(shell expr $(n) - 1),$(SUITES))))

# Every program's main suite. A failing suite does not stop the next one; the
# run fails at the end if any did.
test:
	@rc=0; \
	$(MAKE) --no-print-directory test-arm64emu || rc=1; \
	$(MAKE) --no-print-directory test-arm64chroot || rc=1; \
	$(MAKE) --no-print-directory test-chroot-ng || rc=1; \
	exit $$rc

# ---- arm64emu ----

.PHONY: test-arm64emu test-arm64emu-pd test-arm64emu-jit test-arm64emu-jit-full \
        test-arm64emu-pd-full test-arm64emu-embed test-arm64emu-jit-a64 fuzz-arm64emu

ifeq ($(ARM64EMU_HOST64),8)
# Asm self-tests on the plain interpreter — the reference engine (--no-pd opts
# out of the default predecoded tier).
test-arm64emu:
	@$(MAKE) --no-print-directory $(BUILD)/arm64emu
	@ARM64EMU_EMU_FLAGS=--no-pd tests/arm64emu/run_tests.sh

# The predecoded tier, the default engine (so no ARM64EMU_EMU_FLAGS), then its
# consistency against the plain interpreter.
test-arm64emu-pd:
	@$(MAKE) --no-print-directory $(BUILD)/arm64emu
	@tests/arm64emu/run_tests.sh
	@tests/arm64emu/run_pd_consist.sh

# The asm tests under --jit, then interpreter-vs-jit consistency on a
# deterministic firmware+Linux boot (byte-identical state checkpoints).
test-arm64emu-jit:
	@$(MAKE) --no-print-directory $(BUILD)/arm64emu
	@ARM64EMU_EMU_FLAGS=--jit tests/arm64emu/run_tests.sh
	@tests/arm64emu/run_jit_consist.sh

# The suites plus the full-boot log gate (docs/arm64emu/jit.md: mandatory for
# frontend changes — the consistency checkpoints all sit inside the UEFI phase).
test-arm64emu-jit-full: test-arm64emu-jit
	@tests/arm64emu/run_bootlog_gate.sh --jit
test-arm64emu-pd-full: test-arm64emu-pd
	@tests/arm64emu/run_bootlog_gate.sh --pd

# The embedded build (see arm64emu-embed above) against the asm suite. Nothing
# else builds that configuration, which is exactly why it rots unnoticed.
test-arm64emu-embed:
	@$(MAKE) --no-print-directory $(BUILD)/arm64emu-embed
	@ARM64EMU_EMU=$(BUILD)/arm64emu-embed tests/arm64emu/run_tests.sh

# Cross-engine differential fuzzing: random blocks over the JIT's inlined
# surface, interpreter vs the predecoded tier vs --jit (+ SLOWMEM/NOFUSE/NOVRA
# variants), byte-identical HLT line and full register dump required per seed.
# ARM64EMU_SEEDS overrides the seed count (default 200); see docs/arm64emu/parity.md.
fuzz-arm64emu:
	@$(MAKE) --no-print-directory $(BUILD)/arm64emu
	@tests/arm64emu/run_fuzz_engines.sh
else
test-arm64emu test-arm64emu-pd test-arm64emu-jit test-arm64emu-jit-full \
test-arm64emu-pd-full test-arm64emu-embed fuzz-arm64emu:
	@echo "SKIP $@: arm64emu needs a 64-bit host compiler ($(CC))"
endif

# The --jit suite plus one consistency checkpoint, for the AArch64 backend,
# under qemu-user. Skips politely when the cross pieces are absent.
test-arm64emu-jit-a64:
	@command -v qemu-aarch64 >/dev/null 2>&1 && command -v clang >/dev/null 2>&1 \
	    && [ -e $(ARM64EMU_A64_SYSROOT)/lib/libc.a ] \
	    || { echo "SKIP: qemu-aarch64 / clang / aarch64 cross libc missing"; exit 0; }; \
	$(MAKE) --no-print-directory $(BUILD)/arm64emu-a64 && \
	ARM64EMU_RUNNER=qemu-aarch64 ARM64EMU_EMU=$(CURDIR)/$(BUILD)/arm64emu-a64 ARM64EMU_EMU_FLAGS=--jit \
	    tests/arm64emu/run_tests.sh && \
	ARM64EMU_RUNNER=qemu-aarch64 ARM64EMU_EMU=$(CURDIR)/$(BUILD)/arm64emu-a64 ARM64EMU_POINTS=4000000 \
	    tests/arm64emu/run_jit_consist.sh

# ---- arm64chroot ----
#
# Every test script is invoked through an explicit interpreter, never through
# its shebang: on Termux without the termux-exec LD_PRELOAD shim there is no
# /bin/sh or /bin/bash for the kernel to find, and the exec fails with
# "No such file or directory". The generated wrapper scripts get the running
# sh's own absolute path as their shebang for the same reason.

.PHONY: test-arm64chroot test-arm64chroot-jit test-arm64chroot32 test-arm64chroot32-jit \
        test-arm64chroot-android-sim test-arm64chroot-seccomp test-arm64chroot-pack \
        test-arm64chroot-env

ARM64CHROOT_TESTS := $(BUILD)/tests/arm64chroot

# Provision the Alpine + glibc test rootfs (build/cache/arm64chroot/rootfs).
# The suite also does this on its first run; this is the explicit step.
test-arm64chroot-env:
	bash tests/arm64chroot/setup_env.sh

# The full differential suite vs the oracle, interpreter mode.
test-arm64chroot:
	@$(MAKE) --no-print-directory $(BUILD)/arm64chroot
	bash tests/arm64chroot/run_tests.sh $(BUILD)/arm64chroot

# The ENTIRE differential suite with the JIT enabled (same wrapper trick as
# test-arm64chroot-seccomp): the oracle keeps the translator honest.
# Random-input FP consistency (jit vs interpreter, same host) runs first — the
# FP corner semantics are host-C by design and have no external oracle. On an
# AArch64 host this is also the only thing that ever executes backend_a64.c.
test-arm64chroot-jit:
	@$(MAKE) --no-print-directory $(BUILD)/arm64chroot
	sh tests/arm64chroot/run_consist.sh $(BUILD)/arm64chroot
	@mkdir -p $(ARM64CHROOT_TESTS)
	printf '#!%s\nexec %s --jit "$$@"\n' "$$(command -v sh)" "$(CURDIR)/$(BUILD)/arm64chroot" \
	    > $(ARM64CHROOT_TESTS)/jit_emu.sh
	chmod +x $(ARM64CHROOT_TESTS)/jit_emu.sh
	bash tests/arm64chroot/run_tests.sh $(ARM64CHROOT_TESTS)/jit_emu.sh

# The ILP32-host variant needs a 32-bit toolchain for THIS host *and* a 32-bit
# runtime able to execute what it produces: gcc -m32 plus multilib on x86-64,
# an armhf cross compiler plus armhf libraries on an AArch64 host — and there,
# an AArch64 CPU that still implements AArch32 at EL0, which the server cores
# do not. Both halves are probed, because a machine that cannot run the variant
# should say so rather than fail a build the rest of the suite never needs.
# Shared by both 32-bit suites; the whole thing is one shell list whose status
# is the final test's, so it can drive an `if`.
arm64chroot_m32_runnable = t=$$(mktemp -d) || exit 1; \
	printf 'int main(void){return 0;}' | \
	  $(ARM64CHROOT_M32CC) $(ARM64CHROOT_FLAGS) $(ARM64CHROOT_M32FLAGS) -x c - -o $$t/probe 2>/dev/null && \
	  $$t/probe >/dev/null 2>&1; \
	rc=$$?; rm -rf $$t; [ $$rc = 0 ]

test-arm64chroot32:
	@if $(arm64chroot_m32_runnable); then \
	    $(MAKE) --no-print-directory $(BUILD)/arm64chroot32 && \
	    ARM64CHROOT_EMU_CC="$(ARM64CHROOT_M32CC)" ARM64CHROOT_EMU_CFLAGS="$(ARM64CHROOT_FLAGS) $(ARM64CHROOT_M32FLAGS)" \
	      bash tests/arm64chroot/run_tests.sh $(BUILD)/arm64chroot32; \
	else \
	    echo "SKIP $@: no runnable 32-bit host toolchain ($(ARM64CHROOT_M32CC) $(ARM64CHROOT_M32FLAGS))"; \
	fi

# The ILP32 build with --jit on: the gate for the 32-bit code generators, shaped
# exactly like test-arm64chroot-jit. Where the 32-bit build has no code
# generator yet this still runs end to end and checks the other half of the
# contract — that --jit says so and the interpreter takes over — which nothing
# else covers, since both 64-bit hosts have a backend.
test-arm64chroot32-jit:
	@if $(arm64chroot_m32_runnable); then \
	    set -e; \
	    $(MAKE) --no-print-directory $(BUILD)/arm64chroot32; \
	    sh tests/arm64chroot/run_consist.sh $(BUILD)/arm64chroot32; \
	    mkdir -p $(ARM64CHROOT_TESTS); \
	    printf '#!%s\nexec %s --jit "$$@"\n' "$$(command -v sh)" "$(CURDIR)/$(BUILD)/arm64chroot32" \
	      > $(ARM64CHROOT_TESTS)/jit32_emu.sh; \
	    chmod +x $(ARM64CHROOT_TESTS)/jit32_emu.sh; \
	    ARM64CHROOT_EMU_CC="$(ARM64CHROOT_M32CC)" ARM64CHROOT_EMU_CFLAGS="$(ARM64CHROOT_FLAGS) $(ARM64CHROOT_M32FLAGS)" \
	      bash tests/arm64chroot/run_tests.sh $(ARM64CHROOT_TESTS)/jit32_emu.sh; \
	else \
	    echo "SKIP $@: no runnable 32-bit host toolchain ($(ARM64CHROOT_M32CC) $(ARM64CHROOT_M32FLAGS))"; \
	fi

# Android-behavior simulation on the dev host: the statx ENOSYS fallback, the
# Bionic keyring gate and the emulated-hardlink scheme, under the full
# differential suite (ARM64CHROOT_L2S_BUILD=1 tells it the scheme is compiled in).
test-arm64chroot-android-sim:
	@$(MAKE) --no-print-directory $(BUILD)/arm64chroot_asim
	ARM64CHROOT_L2S_BUILD=1 bash tests/arm64chroot/run_tests.sh $(BUILD)/arm64chroot_asim

# Android-seccomp regression gate: the ENTIRE differential suite with the
# emulator under a SECCOMP_RET_TRAP filter for the full Oreo-blocked set
# (tests/arm64chroot/seccomp_wrap.c). Proves no handler forwards a blocked
# syscall and the SIGSYS net covers the rest. Needs host seccomp (any normal
# Linux).
test-arm64chroot-seccomp:
	@$(MAKE) --no-print-directory $(BUILD)/arm64chroot
	@mkdir -p $(ARM64CHROOT_TESTS)
	$(CC) $(ARM64CHROOT_FLAGS) -o $(ARM64CHROOT_TESTS)/seccomp_wrap.bin tests/arm64chroot/seccomp_wrap.c
	printf '#!%s\nexec %s %s "$$@"\n' "$$(command -v sh)" "$(CURDIR)/$(ARM64CHROOT_TESTS)/seccomp_wrap.bin" \
	    "$(CURDIR)/$(BUILD)/arm64chroot" > $(ARM64CHROOT_TESTS)/seccomp_emu.sh
	chmod +x $(ARM64CHROOT_TESTS)/seccomp_emu.sh
	ARM64CHROOT_EMU_WRAP=$(ARM64CHROOT_TESTS)/seccomp_wrap.bin bash tests/arm64chroot/run_tests.sh \
	    $(ARM64CHROOT_TESTS)/seccomp_emu.sh

# Record a portable test pack: one full suite run under the qemu oracle with
# every oracle answer captured (ARM64CHROOT_RECORD=1), then the built guest binaries,
# the recordings and the small glibc test rootfs bundled into
# build/arm64chroot-testpack.tar.gz. A host that can neither build the guests
# nor run any oracle — a 32-bit ARM device: its CPU cannot execute AArch64, and
# qemu-user cannot map a 64-bit guest into a 32-bit address space at all —
# unpacks it in the repository root (at the SAME commit; the pack carries the
# id and the replay warns on mismatch) and replays with
# `ARM64CHROOT_ORACLE=recorded make test-arm64chroot`. The Alpine-backed self-checking
# sections additionally want an aarch64 rootfs with busybox at
# build/cache/arm64chroot/rootfs/alpine — a symlink to any existing rootfs is
# fine. fpconsist is built here rather than left to test-arm64chroot-jit,
# because a replay host has no compiler and the check needs no oracle — it
# referees the two engines against each other, which is the whole point of
# running it on silicon the code generator has never been validated against.
#
# Only the guest programs travel: the host-side helpers the suite also builds
# into build/tests/arm64chroot (seccomp_wrap, maxrss, ...) are for this host.
ARM64CHROOT_PACKBINS = $$(find $(addprefix $(ARM64CHROOT_TESTS)/,asm c fixtures ptrace) -name '*.bin') \
                       $$(ls $(ARM64CHROOT_TESTS)/fpconsist.bin 2>/dev/null)
test-arm64chroot-pack:
	@$(MAKE) --no-print-directory $(BUILD)/arm64chroot
	rm -rf $(BUILD)/cache/arm64chroot/recorded
	ARM64CHROOT_ORACLE=qemu ARM64CHROOT_RECORD=1 bash tests/arm64chroot/run_tests.sh $(BUILD)/arm64chroot
	sh tests/arm64chroot/run_consist.sh $(BUILD)/arm64chroot
	git rev-parse HEAD > $(BUILD)/cache/arm64chroot/recorded/COMMIT
	md5sum $(ARM64CHROOT_PACKBINS) < /dev/null > $(BUILD)/cache/arm64chroot/recorded/BINSUMS
	tar czf $(BUILD)/arm64chroot-testpack.tar.gz $(ARM64CHROOT_PACKBINS) \
	    $(BUILD)/cache/arm64chroot/recorded $(BUILD)/cache/arm64chroot/rootfs/glibc
	@ls -lh $(BUILD)/arm64chroot-testpack.tar.gz

# ---- chroot-ng ----

.PHONY: test-chroot-ng probe-chroot-ng run-chroot-ng

# The suite runs chroot-ng natively on an AArch64 host and under qemu-aarch64
# elsewhere. Its differential legs (M10/M17/M19) take the arm64chroot built
# here as their oracle, so that is built first.
ifneq ($(CHROOTNG_OK),)
test-chroot-ng:
	@$(MAKE) --no-print-directory $(BUILD)/chroot-ng $(BUILD)/arm64chroot
	@sh tests/chroot-ng/run.sh

# The capability probe (a real AArch64 kernel is needed for the seccomp line,
# so on a cross host under qemu that line always reads "inert").
probe-chroot-ng: $(BUILD)/chroot-ng
	$(CHROOTNG_QEMU) $(BUILD)/chroot-ng --probe

# Run the tool itself, natively or under qemu:
# `make run-chroot-ng CHROOTNG_ARGS="--version"`.
run-chroot-ng: $(BUILD)/chroot-ng
	$(CHROOTNG_QEMU) $(BUILD)/chroot-ng $(CHROOTNG_ARGS)
else
test-chroot-ng probe-chroot-ng run-chroot-ng:
	@echo "SKIP $@: no AArch64 C compiler (install $(CHROOTNG_CROSS)gcc, or set CHROOTNG_CC)"
endif

# ============================================================================
# Housekeeping
# ============================================================================

# Everything but the provisioned test rootfs images and recordings, whose
# rebuild means a fresh download.
clean:
	@if [ -d $(BUILD) ]; then \
	    find $(BUILD) -mindepth 1 -maxdepth 1 ! -name cache -exec rm -rf {} +; \
	fi

clean-testenv:
	rm -rf $(BUILD)/cache

help:
	@echo 'Programs (into build/):'
	@echo '  all (default)            every program this host can build'
	@echo '  arm64emu                 full-system emulator (64-bit hosts)'
	@echo '  arm64emu-embed           its embedded (-DANDROID_JNI) configuration'
	@echo '  arm64chroot              user-space emulator'
	@echo '  arm64chroot32            its ILP32-host build'
	@echo '  arm64chroot_asim         its Android-behavior build'
	@echo '  chroot-ng                in-process chroot (needs an AArch64 compiler)'
	@echo 'Tests (one suite at a time):'
	@echo '  test                     every program'"'"'s main suite'
	@echo '  test-arm64emu            asm suite, plain interpreter'
	@echo '  test-arm64emu-pd         predecoded tier + consistency'
	@echo '  test-arm64emu-jit        --jit + consistency'
	@echo '  test-arm64emu-{pd,jit}-full  the same plus the full-boot log gate'
	@echo '  test-arm64emu-embed      the embedded configuration'
	@echo '  test-arm64emu-jit-a64    AArch64 JIT backend under qemu-user'
	@echo '  fuzz-arm64emu            cross-engine differential fuzzing'
	@echo '  test-arm64chroot         differential suite, interpreter'
	@echo '  test-arm64chroot-jit     FP consistency + suite with --jit'
	@echo '  test-arm64chroot32[-jit] the same on the ILP32-host build'
	@echo '  test-arm64chroot-android-sim  Android-behavior build'
	@echo '  test-arm64chroot-seccomp suite under an Android-Oreo seccomp filter'
	@echo '  test-arm64chroot-pack    record a replayable test pack'
	@echo '  test-arm64chroot-env     provision the test rootfs'
	@echo '  test-chroot-ng           chroot-ng suite'
	@echo '  probe-chroot-ng          host capability probe'
	@echo '  run-chroot-ng            run it with CHROOTNG_ARGS'
	@echo 'Housekeeping:'
	@echo '  clean                    remove build/ but the test cache'
	@echo '  clean-testenv            remove the test cache (rootfs images, recordings)'
