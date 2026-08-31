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
#    make EVAL=nnue       build with the network instead of the classical eval
#    make EVAL=classical  the hand-written evaluation (the default)
#    make TUNE_SEARCH=on  expose the search margins as UCI spin options
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

EXE  ?= stormbreaker
ARCH ?= native

# --------------------------------------------------------------- evaluation --
#  Which evaluation gets linked. Exactly one is compiled in - there is no
#  runtime switch, because an evaluation that tested a flag at every node
#  would pay for the flexibility in the only currency that matters.
#
#    classical  the tuned 13,684-parameter linear model in src/eval.c
#    nnue       the network in src/nnue.c, embedded from EVALFILE
#
#  Both build the same engine otherwise, and `make EVAL=classical` is
#  byte-for-byte the engine that existed before the network did. The default
#  stays classical until an NNUE build has passed its own SPRT (docs/NNUE.md,
#  Task 4) - the switch exists so the two can be compared, and a default
#  flipped ahead of the measurement is how an untested change ships.
EVAL     ?= classical
EVALFILE ?= external/nets/net.nnue
NET      ?= external/nets/net.pt

# ------------------------------------------------------------- net pinning --
#  Which net a build fetches when it has none. `make net-fetch` downloads
#  NET_TAG's asset into EVALFILE and refuses anything whose SHA-256 is not
#  NET_SHA256 - a net that is silently the wrong one scores plausibly and
#  loses Elo, which is the failure mode invariant 8 exists to prevent.
#
#  Published nets are content-addressed: the tag is `net-` plus the first 12
#  hex digits of the hash, so a tag can never come to mean a different file.
#  These two lines are the only place a build learns which net is current;
#  docs/EXPERIMENTS.md records WHY that one was adopted, beside the SPRT that
#  adopted it. Bump them together. tools/publish-net.ps1 uploads a net and
#  prints the replacement lines.
NET_TAG    ?= net-0ba56166ba9c
NET_SHA256 ?= 0ba56166ba9c49da7399eef3a8f6ed5e5e12c6faa9a84885dd4453cbd25a3cae
NET_URL    ?= https://github.com/Stermere/StormBreakerEngine/releases/download/$(NET_TAG)/net.nnue

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

# Names a distributable binary after the evaluation it carries, so that
# `make release EVAL=nnue` does not overwrite the classical build of the same
# ARCH with a binary that plays differently.
EVALSUFFIX :=

# EVAL_NNUE switches the evaluation; NNUE_EVALFILE is the path .incbin embeds,
# resolved relative to the directory make was run from - which is where the
# assembler looks too. Kept in their own variable so `make tuner` can filter
# them back out: the tuner fits the classical model and must never link the
# network over the top of it.
NNUEDEFS :=
ifeq ($(EVAL),nnue)
    NNUEDEFS := -DEVAL_NNUE -DNNUE_EVALFILE='"$(EVALFILE)"'
    EVALDEP  := $(EVALFILE)
    EVALSUFFIX := -nnue
    ifeq ($(wildcard $(EVALFILE)),)
        $(error EVAL=nnue needs a net at '$(EVALFILE)'. Download the pinned \
one with 'make net-fetch', export your own with 'make nnue-export', or point \
EVALFILE somewhere else. Note that the path must not contain spaces - it is \
embedded as an assembler string.)
    endif
else ifneq ($(EVAL),classical)
    $(error Unknown EVAL '$(EVAL)'. Valid: classical nnue)
endif

# TUNE_SEARCH exposes every pruning margin in search.c as a UCI spin option, so
# a parameter sweep sets them per game instead of rebuilding per candidate. Off
# by default and free when off: TUNABLE() expands to an enum constant and the
# compiler folds it exactly as it folded the #define it replaced.
#
# Spelled in full rather than as TUNE because -DTUNE already belongs to the
# EVALUATION fitter below, where it switches on the parameter trace in eval.c.
# One flag doing both jobs would mean `make tuner` silently shipped the search
# options, and a tuning build silently carried the eval trace on its hot path.
TUNE_SEARCH ?= off
TUNEDEFS    :=
ifeq ($(TUNE_SEARCH),on)
    TUNEDEFS := -DTUNE_SEARCH
else ifneq ($(TUNE_SEARCH),off)
    $(error Unknown TUNE_SEARCH '$(TUNE_SEARCH)'. Valid: on off)
endif

DEFINES  := -DNDEBUG $(ARCHDEFS) $(NNUEDEFS) $(TUNEDEFS)

CFLAGS ?=
CFLAGS := $(CSTD) $(WARNINGS) $(OPTIMISE) $(ARCHFLAGS) $(DEFINES) $(CFLAGS)

SOURCES := $(wildcard src/*.c)
HEADERS := $(wildcard src/*.h)

# ------------------------------------------------------------ rebuilding --
#  `make EVAL=nnue` and `make EVAL=classical` build the same file name from
#  the same sources, and so do `make` and `make ARCH=popcnt`. Left alone, make
#  compares timestamps, finds nothing newer, and hands back the binary built
#  with the OTHER flags - so an ablation sweep silently re-benches the previous
#  build and reports that the flag made no difference.
#
#  The fix is a stamp file holding the flags the current binary was built with,
#  rewritten only when they change, and named as a prerequisite. $(file ...) is
#  used rather than a shell so this works with or without a POSIX shell on
#  PATH, which the top-level build otherwise does not need.
#
#  $(file ...) needs GNU make 4.0, and macOS still ships 3.81, where it is
#  not a function at all - it parses as a reference to an undefined variable
#  and expands to nothing, without a word of warning. The stamp is then never
#  written and the build dies on a prerequisite that has no rule, so old make
#  gets a shell instead. That branch never runs on Windows, whose make is
#  MSYS2's 4.x, so the no-shell guarantee above still holds where it matters.
FLAGSTAMP   := .buildflags
BUILDID     := $(CC) $(CFLAGS) $(LDFLAGS)
HAS_FILE_FN := $(filter file,$(.FEATURES))

ifeq ($(HAS_FILE_FN),)
PREVBUILD := $(strip $(shell cat $(FLAGSTAMP) 2>/dev/null))
else
PREVBUILD := $(strip $(if $(wildcard $(FLAGSTAMP)),$(file < $(FLAGSTAMP))))
endif

ifneq ($(strip $(BUILDID)),$(PREVBUILD))
ifeq ($(HAS_FILE_FN),)
# Handed over through the environment rather than pasted into the command:
# EVAL=nnue puts -DNNUE_EVALFILE='"..."' in the flags, and those quotes would
# not survive being expanded into a shell word.
export BUILDID
STAMP_WRITE := $(shell printf '%s\n' "$$BUILDID" > $(FLAGSTAMP))
else
$(file > $(FLAGSTAMP),$(BUILDID))
endif
endif

# ----------------------------------------------------------------- targets --
.PHONY: all native avx512 bmi2 avx2 popcnt legacy debug release \
        bench perft perft-all openbench-check format format-check clean help \
        tuner datagen datagen-test trainer-setup trainer-test sprt tune rating gauntlet snapshot \
        nnue nnue-export nnue-test nnue-info net-fetch net-publish engines-fetch

all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS) $(EVALDEP) $(FLAGSTAMP)
	$(CC) $(CFLAGS) $(SOURCES) -o $(TARGET) $(LDFLAGS)

native avx512 bmi2 avx2 popcnt legacy:
	@$(MAKE) --no-print-directory ARCH=$@ EXE=$(EXE) CC=$(CC)

# Assertions on, optimiser off. Sanitizers are POSIX-only: MinGW GCC ships no
# ASan runtime, so on Windows this is a plain assert+debuginfo build.
debug: CFLAGS := $(CSTD) $(WARNINGS) -O1 -g3 -fno-omit-frame-pointer $(ARCHFLAGS) \
                 $(ARCHDEFS) $(NNUEDEFS)
ifneq ($(OS),Windows_NT)
debug: CFLAGS += -fsanitize=address,undefined -fno-sanitize-recover=all
debug: LDFLAGS += -fsanitize=address,undefined
endif
debug:
	$(CC) $(CFLAGS) $(SOURCES) -o $(EXE)-debug$(SUFFIX) $(LDFLAGS)
	@echo "built $(EXE)-debug$(SUFFIX)"

# Every binary a release would ship. `native` is deliberately excluded: it is
# not portable and must never be published.
#
# Honours EVAL: `make release EVAL=nnue` needs a net (see net-fetch) and names
# what it builds -nnue, because the two evaluations play differently.
#
# The mkdir is not optional: the compiler is asked to write straight into
# build/, and ld does not create the directory - it fails the link outright.
release:
	@mkdir -p build
	@for arch in legacy popcnt avx2 bmi2 avx512; do \
	    echo "==> $$arch $(EVAL)"; \
	    $(MAKE) --no-print-directory ARCH=$$arch EVAL=$(EVAL) \
	        EXE=build/$(EXE)$(EVALSUFFIX)-$$arch CC=$(CC) || exit 1; \
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

# ------------------------------------------------------------------ tuner --
# The evaluation fitter (tools/tuner.c). Links every engine source except
# main.c - it supplies its own - and builds with -DTUNE, which switches on the
# parameter trace eval.c emits. Deliberately NOT part of `all`: it is developer
# tooling, it is not shipped, and the engine binary must not carry it.
TUNER_SOURCES := $(filter-out src/main.c,$(SOURCES)) tools/tuner.c

# The NNUE defines are filtered out rather than merely not added: `make tuner
# EVAL=nnue` would otherwise link the network over eval_evaluate and fit a
# model that is not the one being measured. tools/tuner.c calls
# eval_classical() by name for the same reason.
tuner: $(TUNER_SOURCES) $(HEADERS)
	$(CC) $(filter-out $(NNUEDEFS),$(CFLAGS)) -DTUNE -Isrc $(TUNER_SOURCES) \
	    -o tuner$(SUFFIX) $(LDFLAGS)
	@echo "built tuner$(SUFFIX) - see docs/TUNING.md"

# ---------------------------------------------------------------- datagen --
# The NNUE training-data generator (tools/datagen.c). Links the engine exactly
# as the tuner does, so a label is produced by the search in the working tree.
#
# -DDATAGEN switches on the node visitor in search.c that the tree sampler
# needs. It is a compile-time hook on purpose: the shipped engine carries
# neither the hook nor the branch that tests it, so the bench node count is
# identical either way. Never build the engine itself with it.
#
# GIT_COMMIT is stamped into every shard's .json sidecar. A dataset that cannot
# be attributed to a specific engine build cannot be compared to another one.
DATAGEN_SOURCES := $(filter-out src/main.c,$(SOURCES)) tools/datagen.c
GIT_COMMIT      := $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)
GIT_DIRTY       := $(shell git status --porcelain 2>/dev/null | head -1)
ifneq ($(GIT_DIRTY),)
    GIT_COMMIT := $(GIT_COMMIT)-dirty
endif

# Deliberately honours EVAL: `make datagen EVAL=nnue` is how the pipeline
# bootstraps, since net n+1 trains on labels from searches that used net n.
datagen: $(DATAGEN_SOURCES) $(HEADERS) $(EVALDEP) $(FLAGSTAMP)
	$(CC) $(CFLAGS) -DDATAGEN -DDATAGEN_COMMIT='"$(GIT_COMMIT)"' -Isrc \
	    $(DATAGEN_SOURCES) -o datagen$(SUFFIX) $(LDFLAGS)
	@echo "built datagen$(SUFFIX) - see docs/NNUE.md"

# The Task 1 acceptance gate, small enough to run on every change: generate a
# handful of games, prove every record round-trips to identical bytes, and
# prove a label re-searched from scratch reproduces the score it was stored
# with. A failure here means the dataset cannot be trusted, which is worse than
# a crash, because nothing downstream will notice.
DATAGEN_TEST_DIR := external/datagen-test

datagen-test: datagen
	@rm -rf $(DATAGEN_TEST_DIR)
	@mkdir -p $(DATAGEN_TEST_DIR)
	./datagen$(SUFFIX) selfplay -o $(DATAGEN_TEST_DIR)/shard%02d.cnn \
	    -games 6 -nodes 2000 -threads 2 -seed 7 -quiet
	./datagen$(SUFFIX) shuffle $(DATAGEN_TEST_DIR)/shard00.cnn \
	    $(DATAGEN_TEST_DIR)/shard01.cnn -o $(DATAGEN_TEST_DIR)/all.cnn -seed 7 -quiet
	./datagen$(SUFFIX) verify $(DATAGEN_TEST_DIR)/all.cnn -relabel 32 -nodes 2000
	./datagen$(SUFFIX) stats $(DATAGEN_TEST_DIR)/all.cnn | head -12
	@echo "PASS: datagen round-trips and its labels reproduce"

# And that -book is actually where the games start. One book entry played with
# -opening 0 makes the first record of the shard the book position itself,
# which is the only assertion that separates "the book was read" from "the book
# was opened and ignored" - and a book silently ignored is a generation of
# start-position self-play wearing the manifest of something else.
	@echo 'r1bq1rk1/pp2ppbp/2np1np1/8/2PNP3/2N1B3/PP2BPPP/R2QK2R w KQ - 0 9' > $(DATAGEN_TEST_DIR)/book.epd
	./datagen$(SUFFIX) selfplay -o $(DATAGEN_TEST_DIR)/book.cnn \
	    -book $(DATAGEN_TEST_DIR)/book.epd -opening 0 -noquiet \
	    -games 1 -nodes 2000 -seed 9 -quiet
	./datagen$(SUFFIX) verify $(DATAGEN_TEST_DIR)/book.cnn -relabel 16 -nodes 2000
	@./datagen$(SUFFIX) dump $(DATAGEN_TEST_DIR)/book.cnn -n 1 \
	    | grep -q '^r1bq1rk1/pp2ppbp/2np1np1/8/2PNP3/2N1B3/PP2BPPP/R2QK2R w KQ - 0 9;' \
	    || { echo 'FAIL: -book did not supply the start position'; exit 1; }
	@echo "PASS: -book starts games from the book"

# ---------------------------------------------------------------- trainer --
# PyTorch, its own virtualenv, not part of the C build and not subject to the C
# style rules. See trainer/README.md.
ifeq ($(OS),Windows_NT)
    PYTHON ?= trainer/.venv/Scripts/python.exe
else
    PYTHON ?= trainer/.venv/bin/python
endif

trainer-setup:
	powershell -ExecutionPolicy Bypass -File tools/trainer-setup.ps1

trainer-test: datagen-test
	$(PYTHON) -m pytest trainer/tests -q --shard $(DATAGEN_TEST_DIR)/all.cnn

# ---------------------------------------------------------------- matches --
#
# The match tools are Python and stdlib-only: no venv, no pip, nothing to
# install. Deliberately NOT $(PYTHON), which points into trainer/.venv and
# exists to carry PyTorch - gating an SPRT behind a virtualenv is how a tool
# stops being run.
#
# They are invoked through make so the docs can name a TARGET rather than an
# interpreter. The old `pwsh tools/sprt.ps1` line in the docs was wrong on
# every machine that never installed PowerShell 7, which is most of them,
# including this one.
#
#   make sprt ARGS="--tc LTC"
#   make tune ARGS="--engine ./stormbreaker-tune-nnue.exe --iterations 800"
#   make rating ARGS="--levels 2600,2800,3000"
TOOLPY ?= python
ARGS ?=

sprt:
	@$(TOOLPY) tools/sprt.py $(ARGS)

tune:
	@$(TOOLPY) tools/tune.py $(ARGS)

rating:
	@$(TOOLPY) tools/rating.py $(ARGS)

gauntlet:
	@$(TOOLPY) tools/gauntlet.py $(ARGS)

# Downloads the rated opponent ladder into external/engines, which the gauntlet
# then plays by default. external/ is gitignored, so a fresh clone has none of
# them and this is how they come back. Re-running skips what is already there.
engines-fetch:
	@$(TOOLPY) tools/fetch-engines.py $(ARGS)

snapshot:
	@$(TOOLPY) tools/snapshot-baseline.py $(ARGS)

# ------------------------------------------------------------------- nnue --
# Quantise a trained checkpoint into the file the engine embeds, plus the
# test vectors and the SHA-256 sidecar. Needs the trainer's venv (torch reads
# the checkpoint); the engine itself needs none of it.
nnue-export:
	$(PYTHON) tools/export_net.py $(NET) -o $(EVALFILE)

# The engine, built with the network. A separate name from the classical
# binary on purpose: comparing the two is the entire point, and that is hard
# to do when the second build overwrites the first.
nnue:
	@$(MAKE) --no-print-directory EVAL=nnue EXE=$(EXE)-nnue ARCH=$(ARCH) CC=$(CC)

# The Task 3 acceptance gate: export, then require the C inference to
# reproduce the quantised Python reference EXACTLY on every vector. Exact,
# not close - see the note at the top of tools/export_net.py. A mismatch
# exits non-zero, so this works as a CI gate.
nnue-test:
	@$(MAKE) --no-print-directory nnue-export
	@$(MAKE) --no-print-directory nnue
	./$(EXE)-nnue$(SUFFIX) nnue verify $(EVALFILE).vectors

# Which net a build is actually carrying, by hash. `make bench EVAL=nnue`
# prints the same hash in its header.
nnue-info: nnue
	@./$(EXE)-nnue$(SUFFIX) nnue

# Download the pinned net (NET_TAG / NET_SHA256 near the top of this file) into
# EVALFILE. This is how a machine that cannot run the trainer - a CI runner, an
# OpenBench worker, a fresh clone - gets a net: `make net-fetch && make nnue`.
#
# The hash is checked before the file is put in place, and a mismatch names
# both values and leaves EVALFILE untouched. Re-running is free: a net that
# already hashes correctly is not downloaded again, which matters at 50 MB.
net-fetch:
	@set -e; \
	sha256_of() { \
	    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$$1" | cut -d' ' -f1; \
	    elif command -v shasum >/dev/null 2>&1; then shasum -a 256 "$$1" | cut -d' ' -f1; \
	    else echo "net-fetch: need sha256sum or shasum on PATH" >&2; exit 1; fi; \
	}; \
	if [ -f "$(EVALFILE)" ] && [ "$$(sha256_of "$(EVALFILE)")" = "$(NET_SHA256)" ]; then \
	    echo "net-fetch: $(EVALFILE) is already $(NET_TAG)"; \
	    exit 0; \
	fi; \
	mkdir -p "$$(dirname "$(EVALFILE)")"; \
	echo "net-fetch: $(NET_TAG) -> $(EVALFILE)"; \
	curl -fL --retry 3 --retry-delay 2 -o "$(EVALFILE).part" "$(NET_URL)"; \
	got=$$(sha256_of "$(EVALFILE).part"); \
	if [ "$$got" != "$(NET_SHA256)" ]; then \
	    rm -f "$(EVALFILE).part"; \
	    echo "net-fetch: sha256 mismatch for $(NET_TAG), refusing it" >&2; \
	    echo "  expected $(NET_SHA256)" >&2; \
	    echo "  got      $$got" >&2; \
	    exit 1; \
	fi; \
	mv -f "$(EVALFILE).part" "$(EVALFILE)"; \
	printf '%s  %s\n' "$(NET_SHA256)" "$$(basename $(EVALFILE))" > "$(EVALFILE).sha256"; \
	echo "net-fetch: ok, sha256 $(NET_SHA256)"


# Upload the current net so a build that is not this machine can fetch it.
# Prints the NET_TAG / NET_SHA256 to pin it by; -UpdateMakefile writes them
# here for you. Windows-only, like the rest of tools/*.ps1.
net-publish:
	powershell -ExecutionPolicy Bypass -File tools/publish-net.ps1 -Net $(EVALFILE)

# tools/tuner.c and tools/datagen.c are included: they are C in this
# repository's style, and leaving them out of the check is how they stop being
# that.
FORMAT_FILES := $(SOURCES) $(HEADERS) tools/tuner.c tools/datagen.c

format:
	clang-format -i $(FORMAT_FILES)

format-check:
	clang-format --dry-run --Werror $(FORMAT_FILES)

clean:
	rm -f $(EXE) $(EXE).exe $(EXE)-debug $(EXE)-debug.exe Engine-* *.o *.d .ob*.txt
	rm -f tuner tuner.exe datagen datagen.exe
	rm -f $(EXE)-nnue $(EXE)-nnue.exe $(FLAGSTAMP)
	rm -rf build

help:
	@echo "make [ARCH=native|avx512|bmi2|avx2|popcnt|legacy]  optimised build"
	@echo "make [EVAL=classical|nnue]                        pick the evaluation"
	@echo "make debug              assertions (+ sanitizers on POSIX)"
	@echo "make bench              deterministic node-count benchmark"
	@echo "make perft              movegen correctness suite (fast, depth-capped)"
	@echo "make perft-all          movegen correctness suite at full depth"
	@echo "make release            build all distributable ARCHs into build/"
	@echo "make openbench-check    verify OpenBench compliance"
	@echo "make tuner              build the evaluation fitter (docs/TUNING.md)"
	@echo "make datagen            build the NNUE data generator (docs/NNUE.md)"
	@echo "make datagen-test       datagen round-trip + label reproducibility gate"
	@echo "make trainer-setup      create trainer/.venv and install PyTorch"
	@echo "make trainer-test       run the trainer's test suite"
	@echo "make sprt               SPRT this build against a baseline"
	@echo "make tune               SPSA-tune the TUNE_SEARCH=on parameters"
	@echo "make rating             absolute rating vs a Stockfish ladder"
	@echo "make gauntlet           play a field of baselines, Elo table"
	@echo "make snapshot           freeze this build as a baseline"
	@echo "make nnue-export        quantise NET into EVALFILE (+ test vectors)"
	@echo "make nnue               build the engine with the network evaluation"
	@echo "make nnue-test          C inference == quantised Python reference"
	@echo "make nnue-info          which net a build is carrying, by hash"
	@echo "make net-fetch          download the pinned net into EVALFILE"
	@echo "make net-publish        upload EVALFILE as a content-addressed release"
	@echo "make format[-check]     apply / verify .clang-format"
	@echo "make clean"
