# StormBreaker

A UCI chess engine written in C, built for competitive strength built entirely
from scratch with a mission to see how far a fully AI generated engine can go,
with some human help along the way of course.

The initial engine was built with a classical evaluation function and tuned with a large set of human games and the outcome of the games.
This engine landed at a strength of ~2800 Elo when playing against Stockfish with UCI_Elo set to 3000.

Using that engine as a teacher, a NNUE was trained to reproduce the evaluation of the classical engine at 10k nodes using 200 million positions pulled from human games.
The trained NNUE achived a strength of ~3000 Elo when playing against Stockfish with UCI_Elo set to 3000.

Adding on a correction history and an SPSA tuned search brought this to ~3095 Elo at 40+0.4, beating Stockfish with UCI_Elo set to 3000 (60.3%) and losing to it at 3190 (39.5%).

Retraining the net on self-play data brought the next step, and on top of that came an **uncertainty head**:
a second output on the network that predicts the evaluation's own error, which the search uses to widen its
pruning margins where the evaluation is unreliable and tighten them where it is not. That idea went in as two
SPRT-gated steps worth ~+47 Elo together (E20, E21), and as far as we can find it is the first measured instance
of a learned uncertainty head paying its way in an alpha-beta engine.

The current build scores ~3410 on a gauntlet against five CCRL-rated engines at short time control, dead even
head-to-head with Ethereal 12.75 (CCRL 3426). Ratings do not transfer perfectly across time controls, so we
claim **~3300 CCRL Blitz** until a long-time-control gauntlet confirms it — the anchoring math and its caveats
are in [docs/EXPERIMENTS.md](docs/EXPERIMENTS.md) under *Absolute strength*.

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

**Uncertainty-scaled margins.** `unc_scale()` in
[src/search.c](src/search.c) adjusts five pruning margins using the expected
evaluation error. It uses the network's uncertainty head when available and
falls back to correction-history magnitude otherwise. Both variants passed
their SPRTs (E20 and E21). The constants are centred to preserve the average
margin fitted by SPSA.

**Evaluation.** A HalfKA-style NNUE — 24576 features over 32 mirrored king
squares, SCReLU activation, piece-count output buckets, plus the uncertainty
head — run with an incremental int16 accumulator and AVX2. The net file
describes its own architecture in its header, so a retrain at a different
width or bucket count is a drop-in. The classical evaluation still builds
(`make`, without `EVAL=nnue`) and remains the tuner's model.

**Training pipeline.** `tools/datagen.c` generates and labels positions with
fixed-node searches into a packed 32-byte format; `trainer/` (PyTorch) fits
the net; `tools/export_net.py` quantises it and writes test vectors; and
`make nnue-test` requires the C inference to reproduce those vectors
**exactly** — integer arithmetic, no tolerance. A net that is misread scores
plausibly and loses Elo silently, which is the worst failure mode in this
repository; the bit-exactness gate is what rules it out.

**Testing discipline.** Correctness changes must pass perft exactly. Pure
speedups must leave the bench node count unchanged. Everything else is a
behavioural change and needs a passing SPRT before it ships — roughly half of
"obviously good" engine patches measure neutral or worse, and
[docs/EXPERIMENTS.md](docs/EXPERIMENTS.md) records every result either way.

---

## Quick start

```sh
make                     # build for this machine (classical eval)
make nnue                # build with the network, as stormbreaker-nnue
make bench               # deterministic node-count benchmark
make perft               # move generation correctness suite
```

First-time environment setup (Windows):

```powershell
powershell -File tools\setup.ps1             # fastchess, GUIs, Stockfish; the book
powershell -File tools\register-engines.ps1  # register with Cute Chess
make sprt ARGS=--smoke                       # verify the match pipeline
make trainer-setup                           # trainer\.venv, PyTorch
```

To watch it play:

```powershell
powershell -File tools\gui.ps1                  # En Croissant (default)
powershell -File tools\gui.ps1 -App cutechess   # Cute Chess
```

`gui.ps1` re-registers the engine against your current build before
launching, so the GUI never runs a stale binary. `bestmove 0000` means the
engine found no legal move — checkmate, stalemate, or an empty `go
searchmoves` list; anywhere else it is a bug.

---

## Make commands

### Building

| Command | Purpose |
|---|---|
| `make` | `-march=native`, fastest on this machine, **not portable** |
| `make ARCH=avx512\|bmi2\|avx2\|popcnt\|legacy` | portable arch profiles (`popcnt` is what CI uses) |
| `make EVAL=classical\|nnue` | pick the evaluation at compile time; classical is the default |
| `make EVAL=nnue EVALFILE=<path.nnue>` | embed a specific net |
| `make nnue` | the network build under its own name, `stormbreaker-nnue` |
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
| `make nnue-test` | C inference == quantised Python reference, exactly. **Re-exports `net.nnue` from the local checkpoint** — run `make net-fetch` afterwards if you want the pinned net back |
| `make datagen-test` | datagen round-trip + label reproducibility gate |
| `make trainer-test` | the trainer's pytest suite |

### Measuring (the Python tools, always via `make <tool> ARGS="..."`)

| Command | Purpose and main flags |
|---|---|
| `make sprt` | SPRT dev vs baseline. `--dev <exe>` `--base <exe>` (default: newest in `external\baselines`), `--tc VSTC\|STC\|LTC\|10+0.1`, `--bounds "0,5"`, `--smoke`, `--dry-run` |
| `make gauntlet` | Elo table vs a field. `--engine`, `--field all\|baselines\|engines\|stockfish`, `--games N`, `--tc`, `--include-stockfish`, `--skill-level N`, `--dry-run` |
| `make tune` | SPSA over the `TUNE_SEARCH=on` parameters. `--params`, `--exclude`, `--iterations`, `--tc`, `--list` (inspect state; `--dry-run` rotates it), `--resume` |
| `make snapshot ARGS="--name v0.x"` | freeze the current build into `external\baselines` |
| `make engines-fetch` | download the CCRL-rated opponent ladder into `external\engines` |

### The network

| Command | Purpose |
|---|---|
| `make nnue-export` | quantise `NET` (default `external/nets/net.pt`) into `EVALFILE` + test vectors |
| `make net-fetch` | download the pinned net (`NET_SHA256` in the Makefile), hash-checked |
| `make nnue-info` | report the embedded net and its hash |
| `make net-publish` | upload `EVALFILE` as a content-addressed release |

Training itself runs from `trainer/` — see [trainer/README.md](trainer/README.md);
`--uncertainty` adds the error-predicting head ([docs/NNUE.md](docs/NNUE.md),
Task 5b).

An ad-hoc match, if you want one without the scripts (fastchess is on PATH as
`fast-chess`):

```powershell
fast-chess -engine cmd=.\stormbreaker.exe name=dev `
           -engine cmd=stockfish name=sf `
           -each tc=10+0.1 -rounds 2 -pgnout file=external\games\quick.pgn
```

For measured results, use `make sprt` or `make gauntlet`; these commands set
the book, concurrency and PGN output consistently.

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
