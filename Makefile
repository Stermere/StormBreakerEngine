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
#    make EVAL=classical  build with the hand-written eval instead of the net
#    make classical       the same, under its own name, beside the default build
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
#    nnue       the network in src/nnue.c, embedded from EVALFILE (default)
#    classical  the tuned 13,684-parameter linear model in src/eval.c
#
#  Both build the same engine otherwise, and `make EVAL=classical` is
#  byte-for-byte the engine that existed before the network did. The default is
#  the network because it was MEASURED to be: +238.05 +/- 35.48 Elo at STC,
#  docs/EXPERIMENTS.md E11, which is Task 4's gate in docs/NNUE.md. Nothing in
#  that deprecates the classical model - it is what tools/tuner.c fits, it
#  needs no net, and keeping it buildable is what keeps the two comparable.
#
#  Because the default embeds a net, EVALFILE is a build INPUT now. A clean
#  clone has none - external/ is gitignored, and a net is far too large to
#  commit - so the $(EVALFILE) rule below fetches the pinned one rather than
#  failing the build. `make EVAL=classical` never looks at it.
EVAL             ?= nnue
DEFAULT_EVALFILE := external/nets/net.nnue
EVALFILE         ?= $(DEFAULT_EVALFILE)
NET              ?= external/nets/net.pt

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
# `make release EVAL=classical` does not overwrite the default build of the
# same ARCH with a binary that plays differently. The DEFAULT evaluation takes
# no suffix: an unsuffixed name means "what `make` builds", and that is the
# network now.
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
    # A missing net is fetched rather than diagnosed - see the $(EVALFILE) rule
    # below. A net whose PATH holds a space can only be diagnosed: it is
    # expanded into an assembler string, and no quoting survives the trip.
    ifneq ($(words $(EVALFILE)),1)
        $(error EVALFILE '$(EVALFILE)' contains a space. It is embedded with \
.incbin as an assembler string, which cannot be quoted through the compiler \
driver - put the net somewhere whose path has no spaces in it.)
    endif
else ifeq ($(EVAL),classical)
    EVALSUFFIX := -classical
else
    $(error Unknown EVAL '$(EVAL)'. Valid: nnue classical)
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

# src/test/ includes "board.h" and uci.c includes "test/syzygytest.h", so both
# directions need src/ on the search path. A quoted include only searches the
# INCLUDING file's directory, which was enough while every source sat in src/.
INCLUDES := -Isrc

CFLAGS ?=
CFLAGS := $(CSTD) $(WARNINGS) $(OPTIMISE) $(ARCHFLAGS) $(DEFINES) $(INCLUDES) $(CFLAGS)

# src/ is the engine; src/test/ is the acceptance gates it links in but never
# reaches while playing (see the note in CLAUDE.md). Both are compiled into the
# one binary on purpose: `make syzygy-test` and `make chess960-test` invoke the
# engine itself, so a gate can never be testing a different build.
SOURCES := $(wildcard src/*.c) $(wildcard src/test/*.c)
HEADERS := $(wildcard src/*.h) $(wildcard src/test/*.h)

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
        tuner datagen datagen-test trainer-setup trainer-test sprt tune gauntlet snapshot \
        classical nnue-export nnue-test nnue-info net-fetch net-publish engines-fetch \
        syzygy-fetch syzygy-test chess960-test chess960-campaign

all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS) $(EVALDEP) $(FLAGSTAMP)
	$(CC) $(CFLAGS) $(SOURCES) -o $(TARGET) $(LDFLAGS)

# The net the default evaluation embeds. It is a build input the repository
# does not carry - external/ is gitignored - so a clean clone, a CI runner and an
# OpenBench worker would all otherwise fail the default build on a file they had
# no way to know about. Fetching the PINNED net instead keeps `make` working out
# of a fresh clone without loosening anything:
# net-fetch verifies the SHA-256 before the file is put in place, so this can
# never quietly embed a different net than the one this commit names.
#
# Only the default path is fetched. If EVALFILE was pointed at a particular
# net, downloading the pinned one over that name would answer a question nobody
# asked - that case is diagnosed.
$(EVALFILE):
ifeq ($(EVALFILE),$(DEFAULT_EVALFILE))
	@echo "no net at $(EVALFILE) - fetching the one this build pins"
	@$(MAKE) --no-print-directory net-fetch
else
	@echo "EVALFILE '$(EVALFILE)' does not exist. Either:" >&2
	@echo "  quantise one:   make nnue-export NET=<checkpoint> EVALFILE=$(EVALFILE)" >&2
	@echo "  the pinned net: make net-fetch, and drop EVALFILE" >&2
	@echo "  no net at all:  make EVAL=classical" >&2
	@exit 1
endif

native avx512 bmi2 avx2 popcnt legacy:
	@$(MAKE) --no-print-directory ARCH=$@ EXE=$(EXE) CC=$(CC)

# The engine with the hand-written evaluation, under its own name. A separate
# binary from the default one on purpose: comparing the two is the entire
# point, and that is hard to do when the second build overwrites the first.
classical:
	@$(MAKE) --no-print-directory EVAL=classical EXE=$(EXE)-classical \
	    ARCH=$(ARCH) CC=$(CC)

# Assertions on, optimiser off. Sanitizers are POSIX-only: MinGW GCC ships no
# ASan runtime, so on Windows this is a plain assert+debuginfo build.
debug: CFLAGS := $(CSTD) $(WARNINGS) -O1 -g3 -fno-omit-frame-pointer $(ARCHFLAGS) \
                 $(ARCHDEFS) $(NNUEDEFS) $(INCLUDES)
ifneq ($(OS),Windows_NT)
debug: CFLAGS += -fsanitize=address,undefined -fno-sanitize-recover=all
debug: LDFLAGS += -fsanitize=address,undefined
endif
debug: $(EVALDEP)
	$(CC) $(CFLAGS) $(SOURCES) -o $(EXE)-debug$(SUFFIX) $(LDFLAGS)
	@echo "built $(EXE)-debug$(SUFFIX)"

# Every binary a release would ship. `native` is deliberately excluded: it is
# not portable and must never be published.
#
# Honours EVAL: `make release EVAL=classical` names what it builds -classical,
# because the two evaluations play differently. The default needs a net, which
# it fetches if the tree has none (see the $(EVALFILE) rule).
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
#
# The Chess960 suites are part of the same gate rather than an optional extra.
# Castling geometry is shared code now - board.c derives it for both variants -
# so a change made for standard chess can break Chess960 and vice versa, and
# only running one of them would let that through.
perft: $(TARGET)
	./$(TARGET) perft suite tests/perft/standard.epd 4
	./$(TARGET) perft suite tests/perft/tricky.epd
	./$(TARGET) perft suite tests/perft/chess960.epd 4
	./$(TARGET) perft suite tests/perft/chess960-startpos.epd 4

# Full published depths. Slow (billions of nodes) - run before a release, not
# on every commit.
perft-all: $(TARGET)
	./$(TARGET) perft suite tests/perft/standard.epd
	./$(TARGET) perft suite tests/perft/tricky.epd
	./$(TARGET) perft suite tests/perft/chess960.epd
	./$(TARGET) perft suite tests/perft/chess960-startpos.epd

# The Chess960 checks a node count cannot make: the SP numbering, FEN
# round-trips, castling notation being unambiguous, and do/undo restoring
# everything. See src/chess960test.c for why each is here.
chess960-test: $(TARGET)
	./$(TARGET) chess960 selftest
	./$(TARGET) perft suite tests/perft/chess960.epd
	./$(TARGET) perft suite tests/perft/chess960-startpos.epd 4

# Differential campaign against an independent engine, the way the Syzygy
# prober was verified (docs/EXPERIMENTS.md E24). Needs a Chess960-capable UCI
# engine on PATH; this is what SEALED the .epd counts, and re-running it is how
# you re-earn them after changing castling.
ORACLE ?= stockfish
CAMPAIGN_GAMES ?= 400

chess960-campaign: $(TARGET)
	$(TOOLPY) tools/chess960diff.py campaign -engine ./$(TARGET) -oracle $(ORACLE) \
	    -games $(CAMPAIGN_GAMES) -plies 12 -depth 4

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

# The NNUE defines are filtered out rather than merely not added, which matters
# more now that they are on by default: a plain `make tuner` would otherwise
# link the network over eval_evaluate and fit a model that is not the one being
# measured. tools/tuner.c calls eval_classical() by name for the same reason.
# Filtering them out also means the tuner needs no net to build.
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

# Deliberately honours EVAL, and the default carries the network: that is how
# the pipeline bootstraps, since net n+1 trains on labels from searches that
# used net n. `make datagen EVAL=classical` gets the pre-network labeller back.
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

# And that `-opening MIN-MAX` really draws per game. A book cut at one ply is
# one side to move on every line of it, so a FIXED count starts every game in a
# generation on the same side and no amount of sampling downstream recovers the
# other half. -maxplies 4 makes the draw observable: a game that drew 2 plies
# contributes two records and one that drew 3 contributes one, so a shard
# strictly between one and two records per game is a shard that drew both.
	./datagen$(SUFFIX) selfplay -o $(DATAGEN_TEST_DIR)/range.cnn \
	    -book $(DATAGEN_TEST_DIR)/book.epd -opening 2-3 -maxplies 4 \
	    -games 40 -nodes 2000 -seed 3 -nodedup -noquiet -maxscore 0 -quiet
	@n=$$(( $$(wc -c < $(DATAGEN_TEST_DIR)/range.cnn) / 32 )); \
	    if [ $$n -le 40 ] || [ $$n -ge 80 ]; then \
	        echo "FAIL: -opening 2-3 drew one count for all 40 games ($$n records)"; \
	        exit 1; \
	    fi; \
	    echo "PASS: -opening 2-3 splits the game start ($$n records over 40 games)"

# And that a game stopped by the ply cap records NO result rather than a
# fabricated draw. Every game in range.cnn was cut off by -maxplies 4, so
# every record must carry WDL 3 (unknown) - the value the trainer treats as
# "score only, no result term". A draw here would be the label-poisoning bug
# the adjudication removal exists to prevent.
	@./datagen$(SUFFIX) dump $(DATAGEN_TEST_DIR)/range.cnn -n 100 \
	    | tail -n +2 | awk -F';' '$$3 != 3 { bad = 1 } END { exit bad }' \
	    || { echo "FAIL: a ply-capped game was given a result instead of UNKNOWN"; exit 1; }
	@echo "PASS: ply-capped games are labelled UNKNOWN, never draw"

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
#   make gauntlet ARGS="--field stockfish --games 200"
TOOLPY ?= python
ARGS ?=

sprt:
	@$(TOOLPY) tools/sprt.py $(ARGS)

tune:
	@$(TOOLPY) tools/tune.py $(ARGS)

gauntlet:
	@$(TOOLPY) tools/gauntlet.py $(ARGS)

# Downloads the rated opponent ladder into external/engines, which the gauntlet
# then plays by default. external/ is gitignored, so a fresh clone has none of
# them and this is how they come back. Re-running skips what is already there.
engines-fetch:
	@$(TOOLPY) tools/fetch-engines.py $(ARGS)

# The 3-4-5-man Syzygy tablebases (~939 MB into external/, gitignored). Not a
# build dependency: the engine probes them only when pointed at them.
syzygy-fetch:
	@$(TOOLPY) tools/fetch-syzygy.py $(ARGS)

snapshot:
	@$(TOOLPY) tools/snapshot-baseline.py $(ARGS)

# ------------------------------------------------------------------- nnue --
# Quantise a trained checkpoint into the file the engine embeds, plus the
# test vectors and the SHA-256 sidecar. Needs the trainer's venv (torch reads
# the checkpoint); the engine itself needs none of it.
nnue-export:
	$(PYTHON) tools/export_net.py $(NET) -o $(EVALFILE)

# The Task 3 acceptance gate: export, then require the C inference to
# reproduce the quantised Python reference EXACTLY on every vector. Exact,
# not close - see the note at the top of tools/export_net.py. A mismatch
# exits non-zero, so this works as a CI gate.
#
# EVAL=nnue explicitly, not just by default: this gate must test the network
# even when it is invoked from a shell that has EVAL=classical in the
# environment. It rebuilds $(TARGET), and it re-exports EVALFILE from NET - run
# `make net-fetch` afterwards to put the pinned net back.
nnue-test:
	@$(MAKE) --no-print-directory nnue-export
	@$(MAKE) --no-print-directory all EVAL=nnue
	./$(TARGET) nnue verify $(EVALFILE).vectors

# The tablebase acceptance gate: known endgames, each probed as given and
# mirrored, against tables fetched by `make syzygy-fetch`. A probe that
# silently never fires fails here rather than quietly mislabelling every
# low-piece position the generator writes.
SYZYGY_PATH ?= external/syzygy/3-4-5

SYZYGY_MANIFEST ?= tests/syzygy/probe.manifest

syzygy-test: $(TARGET)
	./$(TARGET) syzygy verify $(SYZYGY_PATH)
# And the differential campaign's verdict, re-checked. The manifest holds one
# checksum per material configuration, sealed from Fathom while it was still in
# the tree (docs/EXPERIMENTS.md E24), so this keeps comparing against an
# independent implementation long after that implementation was deleted.
	./$(TARGET) syzygy manifest $(SYZYGY_PATH) $(SYZYGY_MANIFEST)

# Which net a build is actually carrying, by hash. `make bench` prints the same
# hash in its header.
nnue-info:
	@$(MAKE) --no-print-directory all EVAL=nnue
	@./$(TARGET) nnue

# Download the pinned net (NET_TAG / NET_SHA256 near the top of this file) into
# EVALFILE. This is how a machine that cannot run the trainer - a CI runner, an
# OpenBench worker, a fresh clone - gets a net, and the default build runs it
# for you when EVALFILE is missing. Run it by hand to refresh a net that
# `make nnue-test` re-exported, or to pre-fetch before building offline.
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
	rm -f $(EXE)-classical $(EXE)-classical.exe $(FLAGSTAMP)
	rm -f $(EXE)-nnue $(EXE)-nnue.exe
	rm -rf build

help:
	@echo "make [ARCH=native|avx512|bmi2|avx2|popcnt|legacy]  optimised build"
	@echo "make [EVAL=nnue|classical]                        pick the evaluation"
	@echo "make classical          the hand-written evaluation, under its own name"
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
	@echo "make gauntlet           play a field (or Stockfish alone), Elo table"
	@echo "make snapshot           freeze this build as a baseline"
	@echo "make nnue-export        quantise NET into EVALFILE (+ test vectors)"
	@echo "make nnue-test          C inference == quantised Python reference"
	@echo "make nnue-info          which net a build is carrying, by hash"
	@echo "make net-fetch          download the pinned net into EVALFILE"
	@echo "make syzygy-fetch       download the 3-4-5-man Syzygy tablebases (~939 MB)"
	@echo "make syzygy-test        probe known endgames against the fetched tables"
	@echo "make chess960-test      Chess960 structural gate + its perft suites"
	@echo "make chess960-campaign  differential perft vs ORACLE= (default stockfish)"
	@echo "make net-publish        upload EVALFILE as a content-addressed release"
	@echo "make format[-check]     apply / verify .clang-format"
	@echo "make clean"
