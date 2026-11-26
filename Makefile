# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Sylirre
#
# The one build of this repository's two programs:
#
#   arm64emu   AArch64 emulator of a full system (boot mode)    src/arm64emu
#              and of Linux user space (chroot mode)
#   chroot-ng  in-process chroot for AArch64 hosts              src/chroot-ng
#
# and of the code in lib/: lib/arm64 is the AArch64 emulation arm64emu runs,
# compiled once per host compiler; lib/link2symlink, shared by arm64emu's
# chroot mode and chroot-ng, is compiled into each with that program's flags,
# since chroot-ng is freestanding.
#
# Everything this file produces lands under build/ (and nowhere else):
#   build/<program>          the programs, plus arm64emu's variant builds
#                            (arm64emu32, arm64emu_asim, arm64emu-embed,
#                            arm64emu-a64)
#   build/obj/<tree>/        objects and .d files, one tree per build flavour
#   build/tests/<program>/   the programs the test suites build
#   build/cache/<program>/   provisioned test rootfs images and recordings,
#                            which `make clean` keeps (see clean-testenv)
#
# `make` builds every program this host can build: chroot-ng needs an AArch64
# C compiler (native, or the aarch64-linux-gnu cross one); arm64emu builds
# anywhere, with its boot mode only on a 64-bit host. `make test` runs the
# main suites one after another. `make help` lists the rest.
#
# Variables: CC, CFLAGS and LDFLAGS are arm64emu's host compiler and extra
# flags, for its own sources and the lib/arm64 it links alike (`make
# CFLAGS="-O3 -flto=8"` optimizes all of it). Everything specific to one
# program carries its prefix: ARM64EMU_*, CHROOTNG_* (chroot-ng is cross-built,
# so it has a compiler and flags of its own: CHROOTNG_CC, CHROOTNG_CFLAGS,
# CHROOTNG_LDFLAGS, CHROOTNG_LINKMODE).

BUILD    := build
OBJDIR   := $(BUILD)/obj
HOSTARCH := $(shell uname -m)

# The version of both programs, defined in one place: the VERSION file. Only
# the units that print it get it on their command line, and they depend on
# the file, so a bump rebuilds exactly them (see each program's *_VERSIONED).
PROJECT_VERSION := $(shell cat VERSION)
VERSION_DEFINE  := -DPROJECT_VERSION='"$(PROJECT_VERSION)"'

# The AArch64 emulation.
LIB_ARM64 := $(wildcard lib/arm64/*.c lib/arm64/jit/*.c)
# Its 16-byte atomics (a guest CASP or 128-bit exclusive where guest threads
# run concurrently) are not lock-free on every host, and may need libatomic --
# on 32-bit hosts its 8-byte ones too. clang warns about them; they are
# deliberate -- the guest semantics need a single atomic access of that width
# -- and libatomic's lock is the price. Silenced rather than left to make a
# clang build noisy; probed because gcc has no such option.
# $(call lib_arm64_atomic,CC,FLAGS): -latomic where that compiler has it. Probed
# with the compiler and flags that will actually link, since a 32-bit variant
# may be a different compiler entirely. Recursively expanded on purpose: the
# probe then runs when a link needs it, not on every `make`.
lib_arm64_atomic = $(shell echo 'int main(){return 0;}' | $(1) $(2) \
                     -latomic -x c - -o /dev/null 2>/dev/null && echo -latomic)
LIB_ARM64_NOATOMICWARN := $(shell echo 'int main(){return 0;}' | $(CC) -Werror \
                    -Wno-atomic-alignment -x c - -o /dev/null 2>/dev/null \
                    && echo -Wno-atomic-alignment)
# Hardlink emulation, shared by arm64emu's chroot mode and chroot-ng.
LIB_L2S := $(wildcard lib/link2symlink/*.c)

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
# lib/arm64 — the AArch64 emulation, compiled once per host compiler and
# linked into every build of arm64emu made with it. It includes no program
# header and names no program function (the program reaches it through the
# CPU's ops, cpu.h), so its objects are the same whichever mode runs them;
# built with arm64emu's CC and CFLAGS. The ILP32 build has a compiler of its
# own, and so a copy of the library of its own (ARM64EMU_M32CC, below), as
# has the AArch64 cross build (ARM64EMU_A64_CC).
# ============================================================================

LIB_ARM64_BASEFLAGS = -O2 -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter -fno-math-errno \
                      -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -D_TIME_BITS=64 \
                      -Ilib/arm64 -Ilib/arm64/jit
LIB_ARM64_FLAGS     = $(LIB_ARM64_BASEFLAGS) $(LIB_ARM64_NOATOMICWARN) $(CFLAGS)
LIB_ARM64_OBJ  := $(LIB_ARM64:%.c=$(OBJDIR)/arm64/native/%.o)
LIB_ARM64_CFG   = $(call cfgstamp,arm64/native,$(CC) $(LIB_ARM64_FLAGS))

$(OBJDIR)/arm64/native/%.o: %.c $(LIB_ARM64_CFG)
	@mkdir -p $(@D)
	$(CC) $(LIB_ARM64_FLAGS) $(LIB_ARM64_PIN) $(DEPFLAGS) -c -o $@ $<
# The predecoded tier's computed-goto dispatch regresses ~20% under -O3 and
# LTO (the survey below): its TU stays at plain -O2 whatever CFLAGS asks.
LIB_ARM64_PINNED := lib/arm64/pdrun.o
$(addprefix $(OBJDIR)/arm64/native/,$(LIB_ARM64_PINNED)): LIB_ARM64_PIN := -O2 -fno-lto

-include $(LIB_ARM64_OBJ:.o=.d)

# ============================================================================
# arm64emu — the AArch64 emulator: one program, two modes. Boot mode
# (src/arm64emu/boot) is a full system, the QEMU virt machine; chroot mode
# (src/arm64emu/chroot) is Linux user space; src/arm64emu is what both share,
# the entry among it. C + libc/POSIX only, no external libraries (boot mode's
# networking is its own usernet stack). Builds on x86_64/i386/arm/aarch64
# Linux with GCC or Clang -- boot mode only on a 64-bit host, since its
# guest-RAM model uses __int128: elsewhere it is left out, and says so when
# asked for.
# ============================================================================

# -fno-math-errno: the FP core wants __builtin_sqrt to BE the hardware
# instruction (exec_fpsimd.c says so, and its exception flags are the guest's).
# Without it the compiler must keep a libm call for a possibly-negative operand
# just to set errno, which nothing here reads — and whether that call raises
# Invalid is then the libc's business, not ours. libm (-lm) is linked for
# __builtin_fma/fmaf: the interpreter's FMADD/FMLA use a genuinely fused
# multiply-add (single rounding, matching AArch64), which the compiler turns
# into a hardware FMA under -march=native and otherwise into a libm fma() call.
#
# Build-flag survey (2026-07-19, gcc 13 x86-64, boot mode): -O3/-flto is ~5%
# faster for --jit and neutral for the interpreter, but the predecoded tier's
# computed-goto dispatch TU regresses ~20% under it, and -flto spellings aren't
# portable across gcc/clang — so the default stays plain -O2 and `make
# CFLAGS="-O3 -flto=8"` is the supported opt-in fast recipe: it reaches
# lib/arm64, where the emulation is, and arm64emu's own sources alike. The pin
# on lib/arm64/pdrun.c above keeps that TU at -O2 (a no-op for the default
# build). -mtune=native measured neutral; PGO not pursued (adds a two-phase
# build for a workload-specific gain).
#
# ARM64EMU_OPT is the optimization of arm64emu's own sources alone. LTO across
# them (`make ARM64EMU_OPT="-O2 -flto"`) measured ~28% faster for chroot mode
# before its data-TLB and run loop captured the same cross-TU seam in source;
# on top of them it measures slightly slower, so it is off by default.
ARM64EMU_OPT    ?= -O2
ARM64EMU_BASEFLAGS = $(ARM64EMU_OPT) -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
                     -fno-math-errno -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -D_TIME_BITS=64 \
                     -Isrc/arm64emu -Ilib/arm64 -Ilib/arm64/jit -Ilib/link2symlink
ARM64EMU_FLAGS     = $(ARM64EMU_BASEFLAGS) $(LIB_ARM64_NOATOMICWARN) $(CFLAGS)
# Each mode's sources see the headers of their own directory and none of the
# other's: both modes have a machine.h and an mmu.h, each its own Machine.
arm64emu_modeinc = $(if $(filter src/arm64emu/boot/%,$(1)),-Isrc/arm64emu/boot,$(if \
                     $(filter src/arm64emu/chroot/%,$(1)),-Isrc/arm64emu/chroot -Isrc/arm64emu/chroot/jit))

ARM64EMU_COMMON := $(wildcard src/arm64emu/*.c)
ARM64EMU_BOOT   := $(sort $(wildcard src/arm64emu/boot/*.c src/arm64emu/boot/*/*.c))
ARM64EMU_CHROOT := $(sort $(wildcard src/arm64emu/chroot/*.c src/arm64emu/chroot/jit/*.c)) $(LIB_L2S)
# (The JIT backends, lib/arm64/jit's, self-select by host #ifdef — empty TUs
# elsewhere — so the source lists are host-independent, and a host with none
# gets the runtime's inert stubs.)

# Is CC 64-bit, i.e. are its builds to have boot mode?
ARM64EMU_HOST64 := $(shell printf '__SIZEOF_POINTER__\n' | \
                     $(CC) $(CFLAGS) -E -P -x c - 2>/dev/null | tr -d ' \n')
ARM64EMU_SRC    := $(ARM64EMU_COMMON) $(if $(filter 8,$(ARM64EMU_HOST64)),$(ARM64EMU_BOOT)) \
                   $(ARM64EMU_CHROOT)
ARM64EMU_LIBS    = -lm -lpthread

ARM64EMU_OBJ    := $(ARM64EMU_SRC:%.c=$(OBJDIR)/arm64emu/native/%.o)
ARM64EMU_CFG     = $(call cfgstamp,arm64emu/native,$(CC) $(ARM64EMU_FLAGS))

$(OBJDIR)/arm64emu/native/%.o: %.c $(ARM64EMU_CFG)
	@mkdir -p $(@D)
	$(CC) $(ARM64EMU_FLAGS) $(ARM64EMU_VER) $(call arm64emu_modeinc,$<) $(DEPFLAGS) -c -o $@ $<

$(BUILD)/arm64emu: $(ARM64EMU_OBJ) $(LIB_ARM64_OBJ)
	$(CC) $(ARM64EMU_FLAGS) $(LDFLAGS) -o $@ $^ $(ARM64EMU_LIBS) \
	    $(call lib_arm64_atomic,$(CC),$(ARM64EMU_FLAGS))

# The ILP32-host build keeps 32-bit-host correctness continuously tested (chroot
# mode only: boot mode needs a 64-bit host), and what "32-bit host" means
# depends on the machine doing the building: on x86-64 it is the same compiler
# with -m32, on an AArch64 host it is armhf, which is a different compiler
# rather than a flag. `make test-arm64emu-chroot32` checks both that the
# toolchain exists and that its output runs here before using it.
#
# -mfpmath=sse (x86 only): the i386 x87 default computes guest FP in 80-bit
# excess precision and, worse, transits every by-value double over FLD/FSTP,
# where an sNaN load signals invalid — poisoning the lazily accumulated FPSR
# flags (tests/arm64emu/chroot/asm/m22_fpsr). SSE math makes the FP core behave
# exactly like the 64-bit build; every x86 host that can run this has SSE2.
ifeq ($(HOSTARCH),aarch64)
ARM64EMU_M32CC    ?= arm-linux-gnueabihf-gcc
ARM64EMU_M32FLAGS ?=
else
ARM64EMU_M32CC    ?= $(CC)
ARM64EMU_M32FLAGS ?= -m32 -msse2 -mfpmath=sse
endif

# Its own copy of lib/arm64, built by its own compiler.
LIB_ARM64_M32_OBJ := $(LIB_ARM64:%.c=$(OBJDIR)/arm64/m32/%.o)
LIB_ARM64_M32_CFG  = $(call cfgstamp,arm64/m32,$(ARM64EMU_M32CC) $(LIB_ARM64_FLAGS) $(ARM64EMU_M32FLAGS))
$(OBJDIR)/arm64/m32/%.o: %.c $(LIB_ARM64_M32_CFG)
	@mkdir -p $(@D)
	$(ARM64EMU_M32CC) $(LIB_ARM64_FLAGS) $(ARM64EMU_M32FLAGS) $(LIB_ARM64_PIN) $(DEPFLAGS) -c -o $@ $<
$(addprefix $(OBJDIR)/arm64/m32/,$(LIB_ARM64_PINNED)): LIB_ARM64_PIN := -O2 -fno-lto

ARM64EMU_M32_OBJ := $(ARM64EMU_COMMON:%.c=$(OBJDIR)/arm64emu/m32/%.o) \
                    $(ARM64EMU_CHROOT:%.c=$(OBJDIR)/arm64emu/m32/%.o)
ARM64EMU_M32_CFG  = $(call cfgstamp,arm64emu/m32,$(ARM64EMU_M32CC) $(ARM64EMU_FLAGS) $(ARM64EMU_M32FLAGS))
$(OBJDIR)/arm64emu/m32/%.o: %.c $(ARM64EMU_M32_CFG)
	@mkdir -p $(@D)
	$(ARM64EMU_M32CC) $(ARM64EMU_FLAGS) $(ARM64EMU_M32FLAGS) $(ARM64EMU_VER) $(call arm64emu_modeinc,$<) \
	    $(DEPFLAGS) -c -o $@ $<

$(BUILD)/arm64emu32: $(ARM64EMU_M32_OBJ) $(LIB_ARM64_M32_OBJ)
	$(ARM64EMU_M32CC) $(ARM64EMU_FLAGS) $(ARM64EMU_M32FLAGS) $(LDFLAGS) -o $@ $^ $(ARM64EMU_LIBS) \
	    $(call lib_arm64_atomic,$(ARM64EMU_M32CC),$(ARM64EMU_FLAGS) $(ARM64EMU_M32FLAGS))

# The Android-behavior build: A64_LINK2SYMLINK compiles in the
# emulated-hardlink scheme that __ANDROID__ enables automatically, and
# A64_L2S_FORCE makes --link2symlink take it even where the host allows real
# hardlinks. Both are inert unless the guest is run with --link2symlink, so they
# cost the rest of this build nothing — and without them the scheme has no
# coverage anywhere off Android. (lib/arm64 is the same code here: the flags
# are chroot mode's.)
ARM64EMU_ASIMFLAGS := -DA64_STATX_FORCE_FALLBACK -DA64_KEYRING_ENOSYS \
                      -DA64_LINK2SYMLINK -DA64_L2S_FORCE

ARM64EMU_ASIM_OBJ := $(ARM64EMU_SRC:%.c=$(OBJDIR)/arm64emu/asim/%.o)
ARM64EMU_ASIM_CFG  = $(call cfgstamp,arm64emu/asim,$(CC) $(ARM64EMU_FLAGS) $(ARM64EMU_ASIMFLAGS))
$(OBJDIR)/arm64emu/asim/%.o: %.c $(ARM64EMU_ASIM_CFG)
	@mkdir -p $(@D)
	$(CC) $(ARM64EMU_FLAGS) $(ARM64EMU_ASIMFLAGS) $(ARM64EMU_VER) $(call arm64emu_modeinc,$<) $(DEPFLAGS) -c -o $@ $<

$(BUILD)/arm64emu_asim: $(ARM64EMU_ASIM_OBJ) $(LIB_ARM64_OBJ)
	$(CC) $(ARM64EMU_FLAGS) $(ARM64EMU_ASIMFLAGS) $(LDFLAGS) -o $@ $^ $(ARM64EMU_LIBS) \
	    $(call lib_arm64_atomic,$(CC),$(ARM64EMU_FLAGS) $(ARM64EMU_ASIMFLAGS))

# Embedded configuration. An app that links the emulator in builds it with
# -DANDROID_JNI: main() becomes arm64emu_main() so the host can call it after
# fork(), and fatal paths leave through _exit() so they never run the host's
# atexit handlers (src/arm64emu/embed.h, forced into every unit). arm64emu's
# sources get an object tree of their own, since the flags differ; lib/arm64
# is the same code here, as it never leaves the process itself. The stub main
# stands in for the caller a real embedder supplies.
ARM64EMU_EMBED_FLAGS = $(ARM64EMU_FLAGS) -DANDROID_JNI -include src/arm64emu/embed.h
ARM64EMU_EMBED_OBJ  := $(ARM64EMU_SRC:%.c=$(OBJDIR)/arm64emu/embed/%.o)
ARM64EMU_EMBED_CFG   = $(call cfgstamp,arm64emu/embed,$(CC) $(ARM64EMU_EMBED_FLAGS))

$(OBJDIR)/arm64emu/embed/%.o: %.c $(ARM64EMU_EMBED_CFG)
	@mkdir -p $(@D)
	$(CC) $(ARM64EMU_EMBED_FLAGS) $(ARM64EMU_VER) $(call arm64emu_modeinc,$<) $(DEPFLAGS) -c -o $@ $<

$(OBJDIR)/arm64emu/embed/stub.c:
	@mkdir -p $(@D)
	printf 'extern int arm64emu_main(int, char **);\nint main(int c, char **v) { return arm64emu_main(c, v); }\n' > $@

$(BUILD)/arm64emu-embed: $(ARM64EMU_EMBED_OBJ) $(LIB_ARM64_OBJ) $(OBJDIR)/arm64emu/embed/stub.c
	$(CC) $(ARM64EMU_FLAGS) $(LDFLAGS) -o $@ $^ $(ARM64EMU_LIBS) \
	    $(call lib_arm64_atomic,$(CC),$(ARM64EMU_FLAGS))

# AArch64-backend validation without ARM hardware: a static aarch64 build
# (clang + lld + the Debian cross libc) run under qemu-user, with a copy of
# lib/arm64 of its own. No CFLAGS or LDFLAGS: they are the host compiler's.
ARM64EMU_A64_SYSROOT ?= /usr/aarch64-linux-gnu
ARM64EMU_A64_CC      ?= clang --target=aarch64-linux-gnu -nostdlibinc \
                        -I$(ARM64EMU_A64_SYSROOT)/include
ARM64EMU_A64_LDFLAGS ?= -fuse-ld=lld -static -L$(ARM64EMU_A64_SYSROOT)/lib \
                        -B$(ARM64EMU_A64_SYSROOT)/lib
ARM64EMU_A64_FLAGS    = $(ARM64EMU_BASEFLAGS)
LIB_ARM64_A64_FLAGS   = $(LIB_ARM64_BASEFLAGS)

LIB_ARM64_A64_OBJ := $(LIB_ARM64:%.c=$(OBJDIR)/arm64/a64/%.o)
LIB_ARM64_A64_CFG  = $(call cfgstamp,arm64/a64,$(ARM64EMU_A64_CC) $(LIB_ARM64_A64_FLAGS))
$(OBJDIR)/arm64/a64/%.o: %.c $(LIB_ARM64_A64_CFG)
	@mkdir -p $(@D)
	$(ARM64EMU_A64_CC) $(LIB_ARM64_A64_FLAGS) $(LIB_ARM64_PIN) $(DEPFLAGS) -c -o $@ $<
$(addprefix $(OBJDIR)/arm64/a64/,$(LIB_ARM64_PINNED)): LIB_ARM64_PIN := -O2 -fno-lto

ARM64EMU_A64_SRC := $(ARM64EMU_COMMON) $(ARM64EMU_BOOT) $(ARM64EMU_CHROOT)
ARM64EMU_A64_OBJ := $(ARM64EMU_A64_SRC:%.c=$(OBJDIR)/arm64emu/a64/%.o)
ARM64EMU_A64_CFG  = $(call cfgstamp,arm64emu/a64,$(ARM64EMU_A64_CC) $(ARM64EMU_A64_FLAGS))
$(OBJDIR)/arm64emu/a64/%.o: %.c $(ARM64EMU_A64_CFG)
	@mkdir -p $(@D)
	$(ARM64EMU_A64_CC) $(ARM64EMU_A64_FLAGS) $(ARM64EMU_VER) $(call arm64emu_modeinc,$<) $(DEPFLAGS) -c -o $@ $<

$(BUILD)/arm64emu-a64: $(ARM64EMU_A64_OBJ) $(LIB_ARM64_A64_OBJ)
	$(ARM64EMU_A64_CC) $(ARM64EMU_A64_FLAGS) $(ARM64EMU_A64_LDFLAGS) -o $@ $^ $(ARM64EMU_LIBS) \
	    $(call lib_arm64_atomic,$(ARM64EMU_A64_CC) $(ARM64EMU_A64_LDFLAGS),)

# The version is printed by cli.c alone, in every build.
ARM64EMU_VERSIONED := $(foreach v,native m32 asim embed a64,$(OBJDIR)/arm64emu/$(v)/src/arm64emu/cli.o)
$(ARM64EMU_VERSIONED): VERSION
$(ARM64EMU_VERSIONED): ARM64EMU_VER := $(VERSION_DEFINE)

-include $(ARM64EMU_OBJ:.o=.d) $(ARM64EMU_M32_OBJ:.o=.d) $(ARM64EMU_ASIM_OBJ:.o=.d) \
         $(ARM64EMU_EMBED_OBJ:.o=.d) $(ARM64EMU_A64_OBJ:.o=.d) \
         $(LIB_ARM64_M32_OBJ:.o=.d) $(LIB_ARM64_A64_OBJ:.o=.d)

# ============================================================================
# chroot-ng — ptrace-free chroot/bind emulation for rootless Android (AArch64).
# An ordinary hosted program, linked against the libc of its toolchain, built
# with an AArch64 compiler -- the aarch64-linux-gnu cross toolchain on other
# hosts, run there under arm64emu's chroot mode.
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

# How to run chroot-ng here. On an AArch64 host (native Linux or Termux) that is
# "directly", so CHROOTNG_RUN stays EMPTY; elsewhere it runs under arm64emu's
# chroot mode with the host's tree as the rootfs, which applies a guest's
# seccomp filter as a kernel does (qemu-user does not, and chroot-ng runs
# nothing untranslated). tests/chroot-ng/lib.sh does the same for the suite.
ifeq ($(filter aarch64 arm64,$(HOSTARCH)),)
CHROOTNG_RUN ?= $(BUILD)/arm64emu chroot --jit --keep-fds -w $(CURDIR) /
else
CHROOTNG_RUN ?=
endif

# Is the compiler clang (Termux/NDK) rather than gcc? Some flags are gcc-only.
CHROOTNG_IS_CLANG := $(shell $(CHROOTNG_CC) --version 2>/dev/null | grep -ci clang)

# Position-independent, as the link below is. -fno-stack-protector because the
# monitor runs on the guest's TLS, where Bionic keeps the stack guard, and
# -fno-builtin because the mem/str helpers are the monitor's own (rt/rt.c):
# libc's use the SIMD registers, which a dispatch from guest context spends
# out of the guest's (see -mgeneral-regs-only below).
CHROOTNG_FLAGS = -O2 -g -std=gnu11 -fPIE \
                 -fno-stack-protector \
                 -fno-asynchronous-unwind-tables -fno-builtin \
                 -ffunction-sections -fdata-sections \
                 -Wall -Wextra -Wno-unused-parameter -Isrc/chroot-ng/include \
                 -Ilib/link2symlink \
                 $(CHROOTNG_GCCONLY) $(CHROOTNG_INLINEATOMICS) -mgeneral-regs-only \
                 $(CHROOTNG_CFLAGS)
# Stop gcc turning our mem* loops into calls to themselves. Clang rejects this
# flag and honors -fno-builtin for the same effect.
ifeq ($(CHROOTNG_IS_CLANG),0)
CHROOTNG_GCCONLY := -fno-tree-loop-distribute-patterns
endif
# GCC 13+ defaults to -moutline-atomics, which routes every __atomic builtin
# through a libgcc helper (__aarch64_cas4_acq_rel) that picks LSE or LL/SC by a
# flag a constructor of libgcc's sets. The monitor's atomics run in the SIGSYS
# handler, from the first trap on, so they are inline -- the LL/SC form works
# on every ARMv8. Probed, since not every compiler has it.
CHROOTNG_INLINEATOMICS := $(shell $(CHROOTNG_CC) -mno-outline-atomics -E -x c /dev/null \
                            >/dev/null 2>&1 && echo -mno-outline-atomics)
# -mgeneral-regs-only: no floating point or SIMD anywhere in the monitor. A
# dispatch made from guest context (a bounced call, sigsys.c BOUNCE_DISP) runs
# on the guest's own register file with no signal frame holding the FP half of
# it, and the kernel preserves those registers across a syscall, so anything we
# spend there is taken from the guest -- and a ptrace stop there reads and
# writes the live registers as the guest's (NT_PRFPREG, monitor/ptrace.c). gcc
# does spend it: `cnt v0.8b` for a popcount, `fmov d1, x0` as a cheap spill
# slot for a pointer. Deliberately not probed: silently dropping it would put
# the clobber back.

# A PIE, which the loader places where nothing a guest names can be: a non-PIE
# guest (gcc's cc1) loads at its link-time 0x400000, and the monitor shares its
# address space. Dynamic where it runs on the system that built it; static-PIE
# where it runs under arm64emu on a foreign host, whose tree has no AArch64
# dynamic loader for it to find. CHROOTNG_LINKMODE overrides the choice.
CHROOTNG_LINKMODE ?= $(if $(CHROOTNG_RUN),-static-pie,-pie)
CHROOTNG_LDFLAGS ?=
CHROOTNG_LINK     = $(CHROOTNG_LDFLAGS) $(CHROOTNG_LINKMODE) -Wl,-z,noexecstack \
                    -Wl,--gc-sections

CHROOTNG_SRC := $(shell find src/chroot-ng -name '*.c' -o -name '*.S' 2>/dev/null | LC_ALL=C sort) \
                $(LIB_L2S)
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

$(BUILD)/chroot-ng: $(CHROOTNG_OBJ)
	$(CHROOTNG_CC) $(CHROOTNG_LINK) -o $@ $(CHROOTNG_OBJ)

-include $(CHROOTNG_OBJ:.o=.d)

# ============================================================================
# Programs
# ============================================================================

PROGRAMS := arm64emu
ifneq ($(CHROOTNG_OK),)
PROGRAMS += chroot-ng
endif

all: $(PROGRAMS)
ifneq ($(ARM64EMU_HOST64),8)
	@echo "arm64emu: boot mode left out: it needs a 64-bit host compiler ($(CC))"
endif
ifeq ($(CHROOTNG_OK),)
	@echo "skipped chroot-ng: no AArch64 C compiler (install $(CHROOTNG_CROSS)gcc, or set CHROOTNG_CC)"
endif

.PHONY: arm64emu arm64emu32 arm64emu_asim arm64emu-embed arm64emu-a64 chroot-ng
arm64emu: $(BUILD)/arm64emu
arm64emu32: $(BUILD)/arm64emu32
arm64emu_asim: $(BUILD)/arm64emu_asim
arm64emu-embed: $(BUILD)/arm64emu-embed
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

# The main suites: arm64emu's, one per mode, and chroot-ng's. A failing suite
# does not stop the next one; the run fails at the end if any did.
test:
	@rc=0; \
	$(MAKE) --no-print-directory test-arm64emu-boot || rc=1; \
	$(MAKE) --no-print-directory test-arm64emu-chroot || rc=1; \
	$(MAKE) --no-print-directory test-chroot-ng || rc=1; \
	exit $$rc

# ---- arm64emu, boot mode ----

.PHONY: test-arm64emu-boot test-arm64emu-boot-pd test-arm64emu-boot-jit \
        test-arm64emu-boot-jit-full test-arm64emu-boot-pd-full test-arm64emu-boot-jit-a64 \
        test-arm64emu-embed fuzz-arm64emu

ifeq ($(ARM64EMU_HOST64),8)
# Asm self-tests on the plain interpreter — the reference engine
# (--no-predecode opts out of the default predecoded tier).
test-arm64emu-boot:
	@$(MAKE) --no-print-directory $(BUILD)/arm64emu
	@ARM64EMU_EMU_FLAGS=--no-predecode tests/arm64emu/boot/run_tests.sh

# The predecoded tier, the default engine (so no ARM64EMU_EMU_FLAGS), then its
# consistency against the plain interpreter.
test-arm64emu-boot-pd:
	@$(MAKE) --no-print-directory $(BUILD)/arm64emu
	@tests/arm64emu/boot/run_tests.sh
	@tests/arm64emu/boot/run_pd_consist.sh

# The asm tests under --jit, then interpreter-vs-jit consistency on a
# deterministic firmware+Linux boot (byte-identical state checkpoints).
test-arm64emu-boot-jit:
	@$(MAKE) --no-print-directory $(BUILD)/arm64emu
	@ARM64EMU_EMU_FLAGS=--jit tests/arm64emu/boot/run_tests.sh
	@tests/arm64emu/boot/run_jit_consist.sh

# The suites plus the full-boot log gate (docs/arm64emu/jit.md: mandatory for
# frontend changes — the consistency checkpoints all sit inside the UEFI phase).
test-arm64emu-boot-jit-full: test-arm64emu-boot-jit
	@tests/arm64emu/boot/run_bootlog_gate.sh --jit
test-arm64emu-boot-pd-full: test-arm64emu-boot-pd
	@tests/arm64emu/boot/run_bootlog_gate.sh --pd

# The embedded build (see arm64emu-embed above) against boot mode's asm suite.
# Nothing else builds that configuration, which is exactly why it rots
# unnoticed.
test-arm64emu-embed:
	@$(MAKE) --no-print-directory $(BUILD)/arm64emu-embed
	@ARM64EMU_EMU=$(BUILD)/arm64emu-embed tests/arm64emu/boot/run_tests.sh

# Cross-engine differential fuzzing: random blocks over the JIT's inlined
# surface, interpreter vs the predecoded tier vs --jit (+ SLOWMEM/NOFUSE/NOVRA
# variants), byte-identical HLT line and full register dump required per seed.
# ARM64EMU_SEEDS overrides the seed count (default 200); see docs/arm64emu/parity.md.
fuzz-arm64emu:
	@$(MAKE) --no-print-directory $(BUILD)/arm64emu
	@tests/arm64emu/boot/run_fuzz_engines.sh
else
test-arm64emu-boot test-arm64emu-boot-pd test-arm64emu-boot-jit test-arm64emu-boot-jit-full \
test-arm64emu-boot-pd-full test-arm64emu-embed fuzz-arm64emu:
	@echo "SKIP $@: boot mode needs a 64-bit host compiler ($(CC))"
endif

# The --jit suite plus one consistency checkpoint, for the AArch64 backend,
# under qemu-user. Skips politely when the cross pieces are absent.
test-arm64emu-boot-jit-a64:
	@command -v qemu-aarch64 >/dev/null 2>&1 && command -v clang >/dev/null 2>&1 \
	    && [ -e $(ARM64EMU_A64_SYSROOT)/lib/libc.a ] \
	    || { echo "SKIP: qemu-aarch64 / clang / aarch64 cross libc missing"; exit 0; }; \
	$(MAKE) --no-print-directory $(BUILD)/arm64emu-a64 && \
	ARM64EMU_RUNNER=qemu-aarch64 ARM64EMU_EMU=$(CURDIR)/$(BUILD)/arm64emu-a64 ARM64EMU_EMU_FLAGS=--jit \
	    tests/arm64emu/boot/run_tests.sh && \
	ARM64EMU_RUNNER=qemu-aarch64 ARM64EMU_EMU=$(CURDIR)/$(BUILD)/arm64emu-a64 ARM64EMU_POINTS=4000000 \
	    tests/arm64emu/boot/run_jit_consist.sh

# ---- arm64emu, chroot mode ----
#
# Every test script is invoked through an explicit interpreter, never through
# its shebang: on Termux without the termux-exec LD_PRELOAD shim there is no
# /bin/sh or /bin/bash for the kernel to find, and the exec fails with
# "No such file or directory". The generated wrapper scripts get the running
# sh's own absolute path as their shebang for the same reason.
#
# The suite takes the emulator as its argument and runs it as arm64emu runs:
# `EMU chroot [options] <rootfs> <program>`. A wrapper standing in for it
# (--jit, the seccomp filter) passes the mode word on, its options after it.

.PHONY: test-arm64emu-chroot test-arm64emu-chroot-jit test-arm64emu-chroot32 \
        test-arm64emu-chroot32-jit test-arm64emu-chroot-android-sim \
        test-arm64emu-chroot-seccomp test-arm64emu-chroot-pack test-arm64emu-chroot-env

ARM64EMU_CHROOT_TESTS := $(BUILD)/tests/arm64emu/chroot

# Provision the Alpine + glibc test rootfs (build/cache/arm64emu/chroot/rootfs).
# The suite also does this on its first run; this is the explicit step.
test-arm64emu-chroot-env:
	bash tests/arm64emu/chroot/setup_env.sh

# The full differential suite vs the oracle, interpreter mode.
test-arm64emu-chroot:
	@$(MAKE) --no-print-directory $(BUILD)/arm64emu
	bash tests/arm64emu/chroot/run_tests.sh $(BUILD)/arm64emu

# $(call arm64emu_jit_wrapper,EMU,SCRIPT): SCRIPT runs EMU with --jit.
arm64emu_jit_wrapper = mkdir -p $(ARM64EMU_CHROOT_TESTS); \
	printf '\#!%s\nm=$$1; shift; exec %s "$$m" --jit "$$@"\n' "$$(command -v sh)" \
	    "$(CURDIR)/$(1)" > $(2); \
	chmod +x $(2)

# The ENTIRE differential suite with the JIT enabled (same wrapper trick as
# test-arm64emu-chroot-seccomp): the oracle keeps the translator honest.
# Random-input FP consistency (jit vs interpreter, same host) runs first — the
# FP corner semantics are host-C by design and have no external oracle. On an
# AArch64 host this is also the only thing that ever executes backend_a64.c.
test-arm64emu-chroot-jit:
	@$(MAKE) --no-print-directory $(BUILD)/arm64emu
	sh tests/arm64emu/chroot/run_consist.sh $(BUILD)/arm64emu
	$(call arm64emu_jit_wrapper,$(BUILD)/arm64emu,$(ARM64EMU_CHROOT_TESTS)/jit_emu.sh)
	bash tests/arm64emu/chroot/run_tests.sh $(ARM64EMU_CHROOT_TESTS)/jit_emu.sh

# The ILP32-host build needs a 32-bit toolchain for THIS host *and* a 32-bit
# runtime able to execute what it produces: gcc -m32 plus multilib on x86-64,
# an armhf cross compiler plus armhf libraries on an AArch64 host — and there,
# an AArch64 CPU that still implements AArch32 at EL0, which the server cores
# do not. Both halves are probed, because a machine that cannot run the build
# should say so rather than fail a build the rest of the suite never needs.
# Shared by both 32-bit suites; the whole thing is one shell list whose status
# is the final test's, so it can drive an `if`.
arm64emu_m32_runnable = t=$$(mktemp -d) || exit 1; \
	printf 'int main(void){return 0;}' | \
	  $(ARM64EMU_M32CC) $(ARM64EMU_FLAGS) $(ARM64EMU_M32FLAGS) -x c - -o $$t/probe 2>/dev/null && \
	  $$t/probe >/dev/null 2>&1; \
	rc=$$?; rm -rf $$t; [ $$rc = 0 ]

test-arm64emu-chroot32:
	@if $(arm64emu_m32_runnable); then \
	    $(MAKE) --no-print-directory $(BUILD)/arm64emu32 && \
	    ARM64EMU_EMU_CC="$(ARM64EMU_M32CC)" ARM64EMU_EMU_CFLAGS="$(ARM64EMU_FLAGS) $(ARM64EMU_M32FLAGS)" \
	      bash tests/arm64emu/chroot/run_tests.sh $(BUILD)/arm64emu32; \
	else \
	    echo "SKIP $@: no runnable 32-bit host toolchain ($(ARM64EMU_M32CC) $(ARM64EMU_M32FLAGS))"; \
	fi

# The ILP32 build with --jit on: the gate for the 32-bit code generators, shaped
# exactly like test-arm64emu-chroot-jit. Where the 32-bit build has no code
# generator yet this still runs end to end and checks the other half of the
# contract — that --jit says so and the interpreter takes over — which nothing
# else covers, since both 64-bit hosts have a backend.
test-arm64emu-chroot32-jit:
	@if $(arm64emu_m32_runnable); then \
	    set -e; \
	    $(MAKE) --no-print-directory $(BUILD)/arm64emu32; \
	    sh tests/arm64emu/chroot/run_consist.sh $(BUILD)/arm64emu32; \
	    $(call arm64emu_jit_wrapper,$(BUILD)/arm64emu32,$(ARM64EMU_CHROOT_TESTS)/jit32_emu.sh); \
	    ARM64EMU_EMU_CC="$(ARM64EMU_M32CC)" ARM64EMU_EMU_CFLAGS="$(ARM64EMU_FLAGS) $(ARM64EMU_M32FLAGS)" \
	      bash tests/arm64emu/chroot/run_tests.sh $(ARM64EMU_CHROOT_TESTS)/jit32_emu.sh; \
	else \
	    echo "SKIP $@: no runnable 32-bit host toolchain ($(ARM64EMU_M32CC) $(ARM64EMU_M32FLAGS))"; \
	fi

# Android-behavior simulation on the dev host: the statx ENOSYS fallback, the
# Bionic keyring gate and the emulated-hardlink scheme, under the full
# differential suite (ARM64EMU_L2S_BUILD=1 tells it the scheme is compiled in).
test-arm64emu-chroot-android-sim:
	@$(MAKE) --no-print-directory $(BUILD)/arm64emu_asim
	ARM64EMU_L2S_BUILD=1 bash tests/arm64emu/chroot/run_tests.sh $(BUILD)/arm64emu_asim

# Android-seccomp regression gate: the ENTIRE differential suite with the
# emulator under a SECCOMP_RET_TRAP filter for the full Oreo-blocked set
# (tests/arm64emu/chroot/seccomp_wrap.c). Proves no handler forwards a blocked
# syscall and the SIGSYS net covers the rest. Needs host seccomp (any normal
# Linux).
test-arm64emu-chroot-seccomp:
	@$(MAKE) --no-print-directory $(BUILD)/arm64emu
	@mkdir -p $(ARM64EMU_CHROOT_TESTS)
	$(CC) $(ARM64EMU_FLAGS) -o $(ARM64EMU_CHROOT_TESTS)/seccomp_wrap.bin tests/arm64emu/chroot/seccomp_wrap.c
	printf '#!%s\nexec %s %s "$$@"\n' "$$(command -v sh)" "$(CURDIR)/$(ARM64EMU_CHROOT_TESTS)/seccomp_wrap.bin" \
	    "$(CURDIR)/$(BUILD)/arm64emu" > $(ARM64EMU_CHROOT_TESTS)/seccomp_emu.sh
	chmod +x $(ARM64EMU_CHROOT_TESTS)/seccomp_emu.sh
	ARM64EMU_EMU_WRAP=$(ARM64EMU_CHROOT_TESTS)/seccomp_wrap.bin bash tests/arm64emu/chroot/run_tests.sh \
	    $(ARM64EMU_CHROOT_TESTS)/seccomp_emu.sh

# Record a portable test pack: one full suite run under the qemu oracle with
# every oracle answer captured (ARM64EMU_RECORD=1), then the built guest
# binaries, the recordings and the small glibc test rootfs bundled into
# build/arm64emu-chroot-testpack.tar.gz. A host that can neither build the
# guests nor run any oracle — a 32-bit ARM device: its CPU cannot execute
# AArch64, and qemu-user cannot map a 64-bit guest into a 32-bit address space
# at all — unpacks it in the repository root (at the SAME commit; the pack
# carries the id and the replay warns on mismatch) and replays with
# `ARM64EMU_ORACLE=recorded make test-arm64emu-chroot`. The Alpine-backed
# self-checking sections additionally want an aarch64 rootfs with busybox at
# build/cache/arm64emu/chroot/rootfs/alpine — a symlink to any existing rootfs
# is fine. fpconsist is built here rather than left to
# test-arm64emu-chroot-jit, because a replay host has no compiler and the
# check needs no oracle — it referees the two engines against each other,
# which is the whole point of running it on silicon the code generator has
# never been validated against.
#
# Only the guest programs travel: the host-side helpers the suite also builds
# into build/tests/arm64emu/chroot (seccomp_wrap, maxrss, ...) are for this host.
ARM64EMU_CHROOT_PACKBINS = $$(find $(addprefix $(ARM64EMU_CHROOT_TESTS)/,asm c fixtures ptrace) -name '*.bin') \
                           $$(ls $(ARM64EMU_CHROOT_TESTS)/fpconsist.bin 2>/dev/null)
ARM64EMU_CHROOT_CACHE := $(BUILD)/cache/arm64emu/chroot
test-arm64emu-chroot-pack:
	@$(MAKE) --no-print-directory $(BUILD)/arm64emu
	rm -rf $(ARM64EMU_CHROOT_CACHE)/recorded
	ARM64EMU_ORACLE=qemu ARM64EMU_RECORD=1 bash tests/arm64emu/chroot/run_tests.sh $(BUILD)/arm64emu
	sh tests/arm64emu/chroot/run_consist.sh $(BUILD)/arm64emu
	git rev-parse HEAD > $(ARM64EMU_CHROOT_CACHE)/recorded/COMMIT
	md5sum $(ARM64EMU_CHROOT_PACKBINS) < /dev/null > $(ARM64EMU_CHROOT_CACHE)/recorded/BINSUMS
	tar czf $(BUILD)/arm64emu-chroot-testpack.tar.gz $(ARM64EMU_CHROOT_PACKBINS) \
	    $(ARM64EMU_CHROOT_CACHE)/recorded $(ARM64EMU_CHROOT_CACHE)/rootfs/glibc
	@ls -lh $(BUILD)/arm64emu-chroot-testpack.tar.gz

# ---- chroot-ng ----

.PHONY: test-chroot-ng run-chroot-ng

# The suite runs chroot-ng natively on an AArch64 host and under arm64emu's
# chroot mode elsewhere; its differential legs (M10/M17/M19) take arm64emu's
# chroot mode as their oracle too, so that is built first.
ifneq ($(CHROOTNG_OK),)
test-chroot-ng:
	@$(MAKE) --no-print-directory $(BUILD)/chroot-ng $(BUILD)/arm64emu
	@sh tests/chroot-ng/run.sh

# Run the tool itself, natively or under arm64emu:
# `make run-chroot-ng CHROOTNG_ARGS="--version"`.
run-chroot-ng: $(BUILD)/chroot-ng $(if $(CHROOTNG_RUN),$(BUILD)/arm64emu)
	$(CHROOTNG_RUN) $(BUILD)/chroot-ng $(CHROOTNG_ARGS)
else
test-chroot-ng run-chroot-ng:
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
	@echo '  arm64emu                 the emulator: boot and chroot modes (boot on 64-bit hosts)'
	@echo '  arm64emu32               its ILP32-host build (chroot mode)'
	@echo '  arm64emu_asim            its Android-behavior build'
	@echo '  arm64emu-embed           its embedded (-DANDROID_JNI) configuration'
	@echo '  arm64emu-a64             its static AArch64 cross build (clang + lld)'
	@echo '  chroot-ng                in-process chroot (needs an AArch64 compiler)'
	@echo 'Tests (one suite at a time):'
	@echo '  test                     the main suites: boot, chroot, chroot-ng'
	@echo '  test-arm64emu-boot       boot mode: asm suite, plain interpreter'
	@echo '  test-arm64emu-boot-pd    predecoded tier + consistency'
	@echo '  test-arm64emu-boot-jit   --jit + consistency'
	@echo '  test-arm64emu-boot-{pd,jit}-full  the same plus the full-boot log gate'
	@echo '  test-arm64emu-boot-jit-a64  AArch64 JIT backend under qemu-user'
	@echo '  test-arm64emu-embed      the embedded configuration'
	@echo '  fuzz-arm64emu            cross-engine differential fuzzing'
	@echo '  test-arm64emu-chroot     chroot mode: differential suite, interpreter'
	@echo '  test-arm64emu-chroot-jit FP consistency + suite with --jit'
	@echo '  test-arm64emu-chroot32[-jit]  the same on the ILP32-host build'
	@echo '  test-arm64emu-chroot-android-sim  Android-behavior build'
	@echo '  test-arm64emu-chroot-seccomp  suite under an Android-Oreo seccomp filter'
	@echo '  test-arm64emu-chroot-pack  record a replayable test pack'
	@echo '  test-arm64emu-chroot-env  provision the test rootfs'
	@echo '  test-chroot-ng           chroot-ng suite'
	@echo '  run-chroot-ng            run it with CHROOTNG_ARGS'
	@echo 'Housekeeping:'
	@echo '  clean                    remove build/ but the test cache'
	@echo '  clean-testenv            remove the test cache (rootfs images, recordings)'
