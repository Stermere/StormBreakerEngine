# StormBreaker

A UCI chess engine written in C, built for competitive strength built entirely
from scratch with a mission to see how far a near fully AI generated engine can go,
with some human help along the way of course.

The current build measures **~3500 Elo on the CCRL Blitz scale**, from a
4,200-game gauntlet at 40+0.4 (LTC) against seven CCRL-rated engines.

| Opponent | CCRL | W-L-D | Score |
|---|---:|---:|---:|
| koivisto-8.0 | 3593 | 17-73-60 | 31.3% |
| carp-3.0.1 | 3528 | 27-53-70 | 41.3% |
| ethereal-12.75 | 3426 | 66-34-50 | 60.7% |
| clover-3.0 | 3340 | 94-10-46 | 78.0% |
| weiss-1.4 | 3256 | 109-4-37 | 85.0% |
| berserk-4.1.0 | 3133 | 117-7-26 | 86.7% |
| halogen-8.1 | 3008 | 139-0-11 | 96.3% |

1,050 games per seat. Fitting all seven anchors at once puts the engine at
**3500 ± 17** This is not a direct CCRL rating but it should give a good idea of where the engine sits
---

## The uncertainty head

The network carries two output heads over one accumulator. The value head
scores the position. The **uncertainty head** predicts the magnitude of the
residual `|search score − value|` — how far the evaluation is likely to be from
what a search would return — trained by L1 against that residual, detached, so
the value loss is untouched. The label was already in every training record, so
the head cost a retrain rather than a regeneration, and it costs the search one
extra output-row pass over the accumulator the evaluation already keeps.

A net without the head is still a valid net: `unc_scale()` then runs the same
mapping off correction-history magnitude, a running measurement in place of a
position-only prior. Both were tested, in that order, each against the build
before it:

| | Measured, STC 8+0.08 |
|---|---|
| Corrhist-magnitude probe, no network change (E20) | **+25.61 ± 9.85** |
| The trained σ head vs Corrhist-magnitude probe (E21) | **+21.29 ± 9.22** |

---

## How it works

**Board and move generation.** Bitboards with magic/PEXT sliding-piece
attacks, Zobrist hashing, and a move generator that is perft-exact against
the standard test suites (`make perft`). Three helper functions handle board
mutation and keep the bitboards, mailbox and piece counts synchronized.

**Search.** A principal variation search with iterative deepening, aspiration
windows and a lockless transposition table. Pruning and move ordering include
null move, late move reductions, reverse futility,
futility, razoring, ProbCut, SEE pruning, late move pruning, singular
extensions with multi-cut, killers, counter-moves, butterfly / capture /
continuation history, and a pawn-structure-keyed correction history. The
search runs on a worker thread so the UCI loop never blocks.

**Evaluation.** A HalfKA-style NNUE — 24576 features over 32 mirrored king
squares, SCReLU activation, piece-count output buckets, plus the uncertainty
head above — run with an incremental int16 accumulator and AVX2. The net file
describes its own architecture in its header, so a retrain at a different
width or bucket count is a drop-in, and the uncertainty head sits behind a
header flag so a headless net stays loadable by every build.

**Training pipeline.** `tools/datagen.c` generates and labels positions with
fixed-node searches into a packed 32-byte format; `trainer/` (PyTorch) fits
the net; `tools/export_net.py` quantises it and writes test vectors; and
`make nnue-test` requires the C inference to reproduce those vectors
**exactly**.

**Testing discipline.** Correctness changes must pass perft exactly. Pure
speedups must leave the bench node count unchanged. Everything else is a
behavioural change and needs a passing SPRT before it ships — roughly half of
"obviously good" engine patches measure neutral or worse, and
[docs/EXPERIMENTS.md](docs/EXPERIMENTS.md) records every result either way.

I've been quite lazy and not let all the sprt's run to completion, that worked out when the engine was weak but
it is now too strong to do that and end up gaining in the gauntlet.
---


## Make commands

### Building

| Command | Purpose |
|---|---|
| `make` | `-march=native`, fastest on this machine, **not portable** |
| `make ARCH=avx512\|bmi2\|avx2\|popcnt\|legacy` | portable arch profiles (`popcnt` is what CI uses) |
| `make EVAL=nnue\|classical` | pick the evaluation at compile time; **nnue is the default** |
| `make EVALFILE=<path.nnue>` | embed a specific net instead of the pinned one |
| `make classical` | the hand-written evaluation under its own name, `stormbreaker-classical` |
| `make EXE=<name>` | name the output binary (OpenBench requirement); `CC=` is honoured too |
| `make debug` | assertions on, sanitizers on POSIX |
| `make release` | every distributable ARCH into `build/` |
| `make TUNE_SEARCH=on` | expose the search constants as UCI spin options for a sweep |
| `make clean` / `make format` | clean; apply `.clang-format` |

There is no runtime evaluation switch, and `EVAL`/`ARCH` changes rebuild
correctly — a `.buildflags` stamp is a prerequisite of every binary.

### Verifying

| Command | Purpose |
|---|---|
| `make bench` | deterministic node count; final line is the OpenBench contract |
| `make perft` / `make perft-all` | movegen correctness, depth-capped / full depth |
| `make openbench-check` | verify OpenBench compliance |
| `make nnue-test` | C inference == quantised Python reference, exactly. **Re-exports `net.nnue` from the local checkpoint and rebuilds `stormbreaker`** — run `make net-fetch` and `make` afterwards if you want the pinned net back |
| `make datagen-test` | datagen round-trip + label reproducibility gate |
| `make trainer-test` | the trainer's pytest suite |

### Measuring (the Python tools, always via `make <tool> ARGS="..."`)

| Command | Purpose and main flags |
|---|---|
| `make sprt` | SPRT dev vs baseline. `--dev <exe>` `--base <exe>` (default: newest in `external\baselines`), `--tc VSTC\|STC\|LTC\|10+0.1`, `--bounds "0,5"`, `--smoke`, `--dry-run` |
| `make gauntlet` | Elo table vs a field. `--engine`, `--field all\|baselines\|engines\|stockfish`, `--games N`, `--tc`, `--include-stockfish`, `--skill-level N`, `--dry-run` |
| `make tune` | SPSA over the `TUNE_SEARCH=on` parameters. `--params`, `--exclude`, `--iterations`, `--tc`, `--list` (inspect state; `--dry-run` rotates it), `--resume` |
| `make snapshot ARGS="--name v0.x"` | freeze the current build into `external\baselines` |
| `make ratings` | re-read a gauntlet PGN: cross-table, plus every seat on the CCRL scale. `<pgn>` (default: newest), `--focus <name>`, `--anchor NAME=ELO`, `--no-crosstable`. `make gauntlet` runs this itself when its match ends |
| `make engines-fetch` | download the seven-rung CCRL-rated opponent ladder into `external\engines` (3008 to 3593) |

### The network

| Command | Purpose |
|---|---|
| `make nnue-export` | quantise a checkpoint into `external/nets/net.nnue` + test vectors. `ARGS="<checkpoint> [-o <net>] [-f]"`, where a bare name means `external/nets/<name>`; defaults to `net.pt` -> `net.nnue` |
| `make net-fetch` | download the pinned net (`NET_SHA256` in the Makefile), hash-checked. The default build runs this for you when `EVALFILE` is missing |
| `make nnue-info` | report the embedded net and its hash |
| `make unc-probe` | build `stormbreaker-uncprobe` and measure the σ distribution `unc_scale()` is centred on, scaling held neutral. `PROBE_ARGS="-o <csv>"` records it, `-ref <csv>` solves for the constants that reproduce an older net's, `-live` measures the tree the mapping shapes instead |
| `make net-publish` | upload `EVALFILE` as a content-addressed release |

Training itself runs from `trainer/` — see [trainer/README.md](trainer/README.md).
---

## Repository layout

```
src/                engine sources
tests/perft/        move generation correctness suites (EPD)
tools/              SPRT, SPSA tuning, gauntlet, baselines (Python,
                    stdlib only); setup / GUI / engine registration (PowerShell);
                    tuner.c (evaluation fitter) and datagen.c (NNUE data)
trainer/            the NNUE trainer: PyTorch, its own venv, its own tests
docs/               architecture, status, testing methodology, UCI reference
external/           gitignored: books, opponent engines, baselines, PGNs, nets
.github/workflows/  CI
```

---

## Documentation

- [docs/STATUS.md](docs/STATUS.md) — what is built, what it measured, what comes next
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — how the modules fit together
- [docs/TESTING.md](docs/TESTING.md) — perft, bench, SPRT, OpenBench
- [docs/TUNING.md](docs/TUNING.md) — fitting the classical evaluation to real games
- [docs/NNUE.md](docs/NNUE.md) — the network: data schema, trainer, export, search integration
- [trainer/README.md](trainer/README.md) — running the trainer: setup, the pipeline, the sanity table
- [docs/EXPERIMENTS.md](docs/EXPERIMENTS.md) — every measured change and what it scored
- [docs/UCI.md](docs/UCI.md) — supported commands and options
- [docs/RELEASING.md](docs/RELEASING.md) — cutting a release, and how the net gets published
- [CREDITS.md](CREDITS.md) — prior art, attribution, training data provenance, and how this engine was built

---

## Tooling

| Tool | Role |
|---|---|
| [fastchess](https://github.com/Disservin/fastchess) | match runner and SPRT driver |
| [Cute Chess](https://cutechess.com/) | GUI + `cutechess-cli` |
| [En Croissant](https://encroissant.org/) | modern analysis GUI |
| [Stockfish](https://stockfishchess.org/) | reference opponent, perft cross-check |
| [OpenBench](https://github.com/AndyGrant/OpenBench) | distributed SPRT testing |

---

## License

GPL-3.0. See [LICENSE](LICENSE).
