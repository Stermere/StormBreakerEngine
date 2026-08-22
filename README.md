# StormBreaker

A UCI chess engine written in C, built for competitive strength.

The engine plays legal chess. Move generation is verified exact against the
published perft suites, and the search is a principal variation search with the
standard modern apparatus: transposition table, null move, late move
reductions, singular extensions, SEE-based pruning and a set of history
heuristics.

The evaluation is a 13,684-parameter linear model — material, piece-square
tables, placement conditioned on king position, mobility, pawn structure, king
safety and threats — with every weight fitted by logistic regression to 22.6
million quiet positions from 3.4 million human games. See
[docs/TUNING.md](docs/TUNING.md).

Every change from here is measured with SPRT rather than argued for.

---

## Status

| Component | State |
|---|---|
| Build system (multi-arch, OpenBench-compatible) | complete |
| UCI protocol layer | complete |
| Threading (async search, `stop`, `ponderhit`) | complete |
| Bitboard attack tables, Zobrist hashing, FEN I/O | complete |
| Transposition table allocation | complete |
| Perft harness + test suites | complete |
| Bench harness | complete |
| SPRT / gauntlet tooling, GUI integration, CI | complete |
| Move generation (perft-exact, magic/PEXT sliders) | complete |
| Make / unmake move | complete |
| Time management | complete |
| Search: iterative deepening, alpha-beta, quiescence | complete |
| Transposition table probe / store | complete |
| Killers, history, counter-moves, null move, LMR, PVS | complete |
| Aspiration windows, IIR, reverse futility, LMP, futility | complete |
| Razoring, SEE pruning, quiescence delta pruning | complete |
| Singular extensions, multi-cut, negative extensions | complete |
| Capture history, multi-ply continuation history | complete |
| Time management: phase curve + best-move stability | complete |
| Evaluation: material + tapered piece-square tables | complete |
| Evaluation: pawn structure, mobility, king safety, threats | complete |
| Evaluation: king-relative placement (factorised HalfKA, 6144 weights) | complete |
| Evaluation tuning on real game data (`make tuner`) | complete |
| NNUE data generation, record format, shuffler (`make datagen`) | complete |
| NNUE trainer: features, dataset, model, training loop (`trainer/`) | complete |
| NNUE export + C inference, bit-exact against the reference (`make nnue-test`) | complete |
| NNUE inference: int16 accumulator, AVX2, SCReLU, output buckets | complete |
| **Lazy SMP (`Threads` is capped at 1), staged move generation** | **TODO** |
| **Correction history** | **TODO** |
| **NNUE integration: incremental accumulator, re-tuned margins, SPRT** | **TODO** |
| **NN informed search (policy head for move ordering)** | **TODO** |
| **Chess960 (encoding ready; castling geometry is standard-only)** | **TODO** |

`make perft` and `make perft-all` pass exactly; `make openbench-check` passes,
so the engine can be registered with a distributed testing cluster.

---

## Quick start

```sh
make                     # build for this machine
make bench               # deterministic node-count benchmark
make perft               # move generation correctness suite
make openbench-check     # verify OpenBench compliance
make tuner               # build the evaluation fitter
make datagen             # build the NNUE training-data generator
make datagen-test        # datagen round-trip + label reproducibility gate
make nnue-test           # C network inference == the quantised reference
make help                # all targets
```

First-time environment setup (Windows):

```powershell
pwsh tools\setup.ps1              # install fastchess, GUIs, Stockfish; fetch the book
pwsh tools\register-engines.ps1   # register the engine with Cute Chess
pwsh tools\sprt.ps1 -Smoke        # verify the match pipeline end to end
pwsh tools\trainer-setup.ps1      # create trainer\.venv and install PyTorch
```

---

## Using the GUIs

```powershell
pwsh tools\gui.ps1                    # En Croissant (default)
pwsh tools\gui.ps1 -App cutechess   # Cute Chess
pwsh tools\gui.ps1 -App both
```

`gui.ps1` re-registers the engine against your current build before launching,
so the GUI never runs a stale binary. It starts the app detached — your terminal
stays usable.

`bestmove 0000` means the engine found no legal move — checkmate, stalemate, or
a `go searchmoves` list with nothing legal in it. Anywhere else it is a bug.

### Command-line matches

For anything you want to measure rather than watch, use the scripts — they set
the book, concurrency and PGN output correctly:

```powershell
pwsh tools\sprt.ps1 -Smoke      # verify the pipeline
pwsh tools\sprt.ps1             # dev vs baseline
pwsh tools\gauntlet.ps1         # vs a field of opponents
```

A quick ad-hoc match (fastchess is on PATH as `fast-chess`):

```powershell
fast-chess -engine cmd=.\stormbreaker.exe name=dev `
           -engine cmd=stockfish name=sf `
           -each tc=10+0.1 -rounds 2 -pgnout file=external\games\quick.pgn
```

---

## Build targets

| Target | Purpose |
|---|---|
| `make` | `-march=native`, fastest on this machine, **not portable** |
| `make ARCH=avx2` | portable x86-64-v3 build |
| `make ARCH=popcnt` | portable x86-64-v2 build (used by CI) |
| `make ARCH=legacy` | any x86-64 |
| `make release` | every distributable ARCH into `build/` |
| `make debug` | assertions on, sanitizers on POSIX |
| `make EXE=name` | name the output binary (OpenBench requirement) |
| `make tuner` | the evaluation fitter ([docs/TUNING.md](docs/TUNING.md)) |
| `make datagen` | the NNUE data generator ([docs/NNUE.md](docs/NNUE.md)) |
| `make datagen-test` | datagen round-trip + label reproducibility gate |
| `make trainer-setup` | create `trainer/.venv` and install PyTorch |
| `make trainer-test` | the trainer's test suite |
| `make EVAL=nnue` | build with the network instead of the classical evaluation |
| `make nnue-export` | quantise `NET` into `EVALFILE` + test vectors |
| `make nnue` | the engine with the network, as `stormbreaker-nnue` |
| `make nnue-test` | C inference == the quantised Python reference, exactly |
| `make nnue-info` | which net a build is carrying, by hash |

On non-x86 targets (Apple Silicon, ARM) the arch profiles are ignored and a
portable build is produced automatically.

### Which evaluation gets built

`EVAL` picks one, at compile time, and there is no runtime switch - an
evaluation that tested a flag at every node would pay for the flexibility in
the only currency that matters.

```sh
make                      # classical: the tuned 13,684-parameter linear model
make EVAL=nnue            # the network, embedded from EVALFILE
make EVAL=nnue EVALFILE=external/nets/candidate.nnue
```

The default is classical and stays classical until an NNUE build has passed
its own SPRT ([docs/NNUE.md](docs/NNUE.md), Task 4). The switch exists so the
two can be compared; a default flipped ahead of the measurement is how an
untested change ships.

A net is embedded with `.incbin`, so an NNUE binary is self-contained and its
bench prints the net's SHA-256 in the header - a node count that cannot name
its net is not a measurement. `setoption name EvalFile value <path>` swaps the
net at runtime, which is how a candidate is tried before it is worth
embedding.

---

## Repository layout

```
src/                engine sources (flat, as in most strong engines)
tests/perft/        move generation correctness suites (EPD)
tools/              setup, GUI launch/registration, SPRT, gauntlet (PowerShell)
                    plus tuner.c (evaluation fitter) and datagen.c (NNUE data)
trainer/            the NNUE trainer: PyTorch, its own venv, its own tests
docs/               architecture, testing methodology, UCI reference
external/           gitignored: books, opponent engines, baselines, PGNs
.github/workflows/  CI
```

---

## Where to go next

Steps 1–5 of the original bring-up order (movegen, make/unmake, perft, eval,
search) are done. **Every change from here is measured with
`tools/sprt.ps1`** — read [docs/TESTING.md](docs/TESTING.md) first, because the
testing methodology is what separates an engine that improves from one that
drifts.

The open work, roughly in order of Elo per unit of effort:

1. **Ablate the evaluation batch.** E10 went in as one feature and gained
   +270 Elo, which establishes the batch and attributes nothing. The
   king-relative tables, the tuning itself, and king safety each want their
   own measurement — see the table at the end of
   [docs/EXPERIMENTS.md](docs/EXPERIMENTS.md).
2. **Search parameter tuning.** Every margin in `search.c` is a plausible
   first guess, not a measured optimum — the singular margin, the SEE
   thresholds, the razoring and futility curves, the LMR formula. Each one
   SPRTs independently.
3. **A pawn hash table.** The evaluation costs 20% of the old nps. Most of
   that is pawn structure, recomputed every node for a structure that rarely
   changes. This is a *pure speedup*: the bench node count must come back
   unchanged, which is what makes it provable without an SPRT.
4. **Lazy SMP.** `Threads` is capped at 1. The ordering tables in `search.c`
   are file-scope and must move into a per-thread block first.
5. **Staged move generation**, so a node that cuts on the table move never
   generates or scores the rest of the list.
6. **NNUE.** Tasks 1-3 of [docs/NNUE.md](docs/NNUE.md) are built and their
   gates pass: `tools/datagen.c` generates and labels positions with the search
   in the working tree, `trainer/` fits a 24576 -> 1024x2 -> 8 SCReLU network to
   them, and `src/nnue.c` reproduces the quantised reference to the integer on
   10,000 positions — the acceptance criterion is exact equality, not "close".
   Next is Task 4: the incremental accumulator, which is most of the remaining
   speed, then re-tuned margins and an SPRT before the default can change.
   Generating the data in-house is what keeps the network clear of other
   engines' licensing.

---

## Documentation

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — how the modules fit together
- [docs/TESTING.md](docs/TESTING.md) — perft, bench, SPRT, OpenBench
- [docs/TUNING.md](docs/TUNING.md) — fitting the evaluation to real games
- [docs/NNUE.md](docs/NNUE.md) — the network plan: data schema, trainer, export, search integration
- [trainer/README.md](trainer/README.md) — running the trainer: setup, the pipeline, the sanity table
- [docs/EXPERIMENTS.md](docs/EXPERIMENTS.md) — every measured change and what it scored
- [docs/UCI.md](docs/UCI.md) — supported commands and options

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

GPL is the norm for chess engines, and it is a deliberate choice here: it keeps
the door open to studying and adapting ideas from the many strong GPL engines,
which is standard practice in this field.
