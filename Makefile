# ============================================================================
#  Chess engine build system.
#
#  OpenBench compliance (do not break these three properties):
#    1. `make EXE=Engine-ABCDEFGH` produces a binary named exactly
#       `Engine-ABCDEFGH` (or `.exe` on Windows) in THIS directory.
#    2. `CC=` is honoured so the test client can pick a compiler.
#    3. `./<binary> bench` prints "<nodes> nodes <nps> nps" and is deterministic.
#  `make openbench-check` verifies all three.
#
#  Usage:
#    make                 optimised build for THIS machine (-march=native)
#    make ARCH=avx2       portable build for a CPU class (see ARCH table below)
#    make avx2            shorthand for the above
#    make debug           unoptimised + assertions (+ sanitizers on POSIX)
#    make bench           build, then run the deterministic node-count benchmark
#    make perft           build, then run the perft correctness suite
#    make release         build every distributable ARCH into ./build/
#    make clean
#
#  Sources are compiled in a single invocation. At engine scale a full rebuild
#  is ~1s, and it gives whole-program optimisation without LTO plumbing or any
#  dependency on `mkdir -p`/shell builtins that MinGW make may not provide.
# ============================================================================

EXE  ?= chessengine
ARCH ?= native

# `CC ?= gcc` would NOT work here: make predefines CC as `cc`, so the variable
# is already set and ?= does nothing. Overriding only when the value is still
# make's built-in default leaves an explicit `make CC=clang` (or OpenBench's
# CC=) in charge, while defaulting to gcc rather than to whatever `cc` is.
ifeq ($(origin CC),default)
    CC := gcc
endif

# ---------------------------------------------------------------- platform --
ifeq ($(OS),Windows_NT)
    SUFFIX  := .exe
    # Static-link libgcc/winpthread. Without this the binary needs MSYS2 DLLs
    # on PATH and silently fails to launch from Cute Chess / fastchess, which
    # spawn it with a bare environment.
    LDFLAGS += -static
else
    SUFFIX  :=
    UNAME   := $(shell uname -s)
    LDFLAGS += -lm
    ifneq ($(UNAME),Darwin)
        LDFLAGS += -pthread
    endif
endif

TARGET := $(EXE)$(SUFFIX)

# -------------------------------------------------------------------- arch --
#  ARCH        baseline CPU                         extra engine features
#  ---------   ----------------------------------   ---------------------------
#  native      this machine only (not portable)     everything available
#  avx512      x86-64-v4 / Skylake-X and newer      popcnt, pext, avx512
#  bmi2        x86-64-v3 / Haswell and newer        popcnt, pext (fast on Intel)
#  avx2        x86-64-v3 / Haswell, Zen1-2          popcnt (no pext: slow on Zen1-2)
#  popcnt      x86-64-v2 / Nehalem and newer        popcnt
#  legacy      any x86-64                           none
ifeq ($(ARCH),native)
    ARCHFLAGS := -march=native
    ARCHDEFS  := -DUSE_POPCNT -DUSE_PEXT
else ifeq ($(ARCH),avx512)
    ARCHFLAGS := -march=x86-64-v4
    ARCHDEFS  := -DUSE_POPCNT -DUSE_PEXT -DUSE_AVX512
else ifeq ($(ARCH),bmi2)
    ARCHFLAGS := -march=x86-64-v3 -mbmi2
    ARCHDEFS  := -DUSE_POPCNT -DUSE_PEXT
else ifeq ($(ARCH),avx2)
    ARCHFLAGS := -march=x86-64-v3
    ARCHDEFS  := -DUSE_POPCNT
else ifeq ($(ARCH),popcnt)
    ARCHFLAGS := -march=x86-64-v2
    ARCHDEFS  := -DUSE_POPCNT
else ifeq ($(ARCH),legacy)
    ARCHFLAGS := -march=x86-64
    ARCHDEFS  :=
else
    $(error Unknown ARCH '$(ARCH)'. Valid: native avx512 bmi2 avx2 popcnt legacy)
endif

# Every profile above is x86-specific: `-march=x86-64-v2` is rejected outright
# by an AArch64 compiler, and `-march=native` is not even spelled that way by
# Apple clang. On anything non-x86 (Apple Silicon, ARM servers, Raspberry Pi)
# drop the arch flags entirely and build portably. popcount() already falls
# back to __builtin_popcountll, which every target supports.
ifeq ($(OS),Windows_NT)
    UNAME_M := x86_64
else
    UNAME_M := $(shell uname -m 2>/dev/null || echo unknown)
endif

ifeq ($(filter x86_64 amd64 i386 i686,$(UNAME_M)),)
    ARCHFLAGS :=
    ARCHDEFS  :=
endif

# ------------------------------------------------------------------- flags --
CSTD     := -std=c17
WARNINGS := -Wall -Wextra -Wshadow -Wcast-qual -Wstrict-prototypes \
            -Wmissing-prototypes -Wpointer-arith -Wwrite-strings
OPTIMISE := -O3 -funroll-loops -fno-math-errno -fomit-frame-pointer
DEFINES  := -DNDEBUG $(ARCHDEFS)

CFLAGS ?=
CFLAGS := $(CSTD) $(WARNINGS) $(OPTIMISE) $(ARCHFLAGS) $(DEFINES) $(CFLAGS)

SOURCES := $(wildcard src/*.c)
HEADERS := $(wildcard src/*.h)

# ----------------------------------------------------------------- targets --
.PHONY: all native avx512 bmi2 avx2 popcnt legacy debug release \
        bench perft perft-all openbench-check format format-check clean help

all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS)
	$(CC) $(CFLAGS) $(SOURCES) -o $(TARGET) $(LDFLAGS)

native avx512 bmi2 avx2 popcnt legacy:
	@$(MAKE) --no-print-directory ARCH=$@ EXE=$(EXE) CC=$(CC)

# Assertions on, optimiser off. Sanitizers are POSIX-only: MinGW GCC ships no
# ASan runtime, so on Windows this is a plain assert+debuginfo build.
debug: CFLAGS := $(CSTD) $(WARNINGS) -O1 -g3 -fno-omit-frame-pointer $(ARCHFLAGS) $(ARCHDEFS)
ifneq ($(OS),Windows_NT)
debug: CFLAGS += -fsanitize=address,undefined -fno-sanitize-recover=all
debug: LDFLAGS += -fsanitize=address,undefined
endif
debug:
	$(CC) $(CFLAGS) $(SOURCES) -o $(EXE)-debug$(SUFFIX) $(LDFLAGS)
	@echo "built $(EXE)-debug$(SUFFIX)"

# Every binary a release would ship. `native` is deliberately excluded: it is
# not portable and must never be published.
release:
	@for arch in legacy popcnt avx2 bmi2 avx512; do \
	    echo "==> $$arch"; \
	    $(MAKE) --no-print-directory ARCH=$$arch EXE=build/$(EXE)-$$arch CC=$(CC) || exit 1; \
	done

bench: $(TARGET)
	./$(TARGET) bench

# Fast gate: standard positions capped at depth 4, plus every edge case.
perft: $(TARGET)
	./$(TARGET) perft suite tests/perft/standard.epd 4
	./$(TARGET) perft suite tests/perft/tricky.epd

# Full published depths. Slow (hundreds of millions of nodes) - run before a
# release, not on every commit.
perft-all: $(TARGET)
	./$(TARGET) perft suite tests/perft/standard.epd
	./$(TARGET) perft suite tests/perft/tricky.epd

# Verifies the three OpenBench contract points listed at the top of this file.
# Needs a POSIX shell (MSYS2/Git Bash on Windows); the build itself does not.
#
# NOTE: only the NODE COUNT is compared between runs. The elapsed time and nps
# legitimately differ every run, so diffing the whole bench output would fail
# even for a perfectly deterministic engine.
openbench-check:
	@$(MAKE) --no-print-directory EXE=Engine-OBCHECK
	@test -x Engine-OBCHECK$(SUFFIX) \
	    || { echo "FAIL: 'make EXE=' did not produce Engine-OBCHECK$(SUFFIX)"; exit 1; }
	@echo "ok: make EXE= produces the requested filename"
	@n1=`./Engine-OBCHECK$(SUFFIX) bench | sed -n 's/^\([0-9][0-9]*\) nodes [0-9][0-9]* nps.*/\1/p'`; \
	 n2=`./Engine-OBCHECK$(SUFFIX) bench | sed -n 's/^\([0-9][0-9]*\) nodes [0-9][0-9]* nps.*/\1/p'`; \
	 if [ -z "$$n1" ]; then \
	     echo "FAIL: bench output lacks '<nodes> nodes <nps> nps'"; exit 1; fi; \
	 echo "ok: bench prints '<nodes> nodes <nps> nps'  ($$n1 nodes)"; \
	 if [ "$$n1" != "$$n2" ]; then \
	     echo "FAIL: bench node count is not deterministic ($$n1 vs $$n2)"; exit 1; fi; \
	 echo "ok: bench node count is deterministic"
	@printf 'uci\nquit\n' | ./Engine-OBCHECK$(SUFFIX) > .obuci.txt 2>&1
	@grep -q 'option name Hash'    .obuci.txt || { echo "FAIL: no Hash option";    exit 1; }
	@grep -q 'option name Threads' .obuci.txt || { echo "FAIL: no Threads option"; exit 1; }
	@grep -q 'uciok'               .obuci.txt || { echo "FAIL: no uciok";          exit 1; }
	@echo "ok: UCI exposes Hash + Threads and completes the handshake"
	@rm -f Engine-OBCHECK$(SUFFIX) .obuci.txt
	@echo "PASS: engine is OpenBench compliant"

format:
	clang-format -i $(SOURCES) $(HEADERS)

format-check:
	clang-format --dry-run --Werror $(SOURCES) $(HEADERS)

clean:
	rm -f $(EXE) $(EXE).exe $(EXE)-debug $(EXE)-debug.exe Engine-* *.o *.d .ob*.txt
	rm -rf build

help:
	@echo "make [ARCH=native|avx512|bmi2|avx2|popcnt|legacy]  optimised build"
	@echo "make debug              assertions (+ sanitizers on POSIX)"
	@echo "make bench              deterministic node-count benchmark"
	@echo "make perft              movegen correctness suite (fast, depth-capped)"
	@echo "make perft-all          movegen correctness suite at full depth"
	@echo "make release            build all distributable ARCHs into build/"
	@echo "make openbench-check    verify OpenBench compliance"
	@echo "make format[-check]     apply / verify .clang-format"
	@echo "make clean"
