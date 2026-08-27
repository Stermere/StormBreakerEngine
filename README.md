# StormBreaker

A UCI chess engine written in C, built for competitive strength built entirely
from scratch with a mission to see how far a fully AI generated engine can go.

The initial engine was built with a classical evaluation function and tuned with a large set of human games and the outcome of the games.
This engine landed at a strength of ~2800 Elo when playing against Stockfish with UCI_Elo set to 3000.

Using that engine as a teacher, a NNUE was trained to reproduce the evaluation of the classical engine at 10k nodes using 200 million positions pulled from human games.
The trained NNUE achived a strength of ~3000 Elo when playing against Stockfish with UCI_Elo set to 3000.


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
| NNUE integration: incremental accumulator, per-ply stack, refresh on king bucket | complete |
| Correction history (pawn-structure keyed, +25.8 Elo) | complete |
| Correction history: minor / non-pawn / continuation keys | tried, neutral (E16), reverted |
| **NNUE: re-tuned search margins, then the default switches** | **TODO** |
| **Next data generation, labelled by the network rather than by `eval.c`** | **TODO** |
| **Lazy SMP (`Threads` is capped at 1)** | **TODO** |
| Staged movegen 1: try the TT move before generating anything | tried, neutral (E15), reverted |
| **Staged movegen 2: full staged picker, captures and quiets deferred** | **TODO** |
| **Staged movegen 3: the ordering changes staging enables, one SPRT each** | **TODO** |
| **NN informed search (policy head for move ordering)** | **TODO** |
| **Chess960 (encoding ready; castling geometry is standard-only)** | **TODO** |

`make perft` and `make perft-all` pass exactly; `make openbench-check` passes,
so the engine can be registered with a distributed testing cluster.

Staged move generation is listed after the NNUE integration deliberately: a more
expensive evaluation shrinks whatever share generation occupies, so it should be
measured against the eval that ships. At bench depth 12, generation and ordering
are ~25% of search cycles, 61% of generated moves are never picked, and 52% of
generating nodes cut on the first move tried - 15% on the TT move, which need not
generate at all.

Those four figures are profile measurements. The ~5-9 Elo once attached to them
was not a measurement, and step 1 went looking for it and did not find it.

Steps 1 and 2 were planned as **pure speedups gated on an unchanged bench node
count**, on the reasoning that the ordering bands in `src/search.c` are strictly
separated and so a staged picker can reproduce today's order exactly. **That is
measurably wrong, and each step needs an SPRT.** Band separation governs the
order in which a *scored* list is walked, but staging also moves *when* the
scoring happens: `score_moves()` currently runs before any child is searched,
whereas a staged picker runs it after the table move's subtree has already
updated `History`, `ContHist`, `CaptureHist` and `CounterMoves`. The remaining
moves are then scored against different tables and can reorder.

**Step 1 was then built, measured and reverted - see E15.** It benched 277624
against 299634, a 7.3% node reduction, with a control isolating that entirely to
scoring time. But nps was flat (the only nodes that skip generating are the ones
that cut on the table move, ~3.75% of cycles gross, and the picker spends part of
it back), and the node reduction did not convert: +0.47 ± 8.52 Elo, stopped
undecided at 2958 games.

That is evidence against the ~10%-of-search-time estimate itself, not only
against step 1, since step 1 took the cheapest and most certain part of that
share. **Do not build steps 2 or 3 on the 5-9 Elo figure.** Measure first, on a
profile, what a full staged picker actually saves in nps - rather than inferring
it from the share of cycles generation occupies.

Promotions are a second, independent reason step 2 cannot reproduce today's
order for free. They straddle the bands three ways: a quiet underpromotion is
generated as a quiet but scores in the winning-capture band, and an SEE-losing
capturing promotion scores down into the quiet band. A tactical stage therefore
has to generate promotions and spill whatever scores below `SCORE_KILLER_1` into
the quiet stage - and unlike the scoring-time effect above, this one is at least
fixable by construction.

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
powershell -File tools\setup.ps1             # fastchess, GUIs, Stockfish; the book
powershell -File tools\register-engines.ps1  # register with Cute Chess
make sprt ARGS=--smoke                          # verify the match pipeline
make trainer-setup                              # trainer\.venv, PyTorch
```

---

## Using the GUIs

```powershell
powershell -File tools\gui.ps1                  # En Croissant (default)
powershell -File tools\gui.ps1 -App cutechess   # Cute Chess
powershell -File tools\gui.ps1 -App both
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
make sprt ARGS=--smoke          # verify the pipeline
make sprt                       # dev vs baseline
make gauntlet                   # vs a field of opponents
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
| `make TUNE_SEARCH=on` | expose the pruning margins as UCI spin options, for a sweep |
| `make nnue-export` | quantise `NET` into `EVALFILE` + test vectors |
| `make nnue` | the engine with the network, as `stormbreaker-nnue` |
| `make nnue-test` | C inference == the quantised Python reference, exactly |
| `make nnue-info` | which net a build is carrying, by hash |
| `make net-fetch` | download the pinned net into `EVALFILE`, hash-checked |
| `make net-publish` | upload this net as a content-addressed release ([docs/RELEASING.md](docs/RELEASING.md)) |

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
tools/              SPRT, SPSA tuning, gauntlet, rating, baselines (Python,
                    stdlib only); setup / GUI / engine registration (PowerShell,
                    because they are Windows integration and nothing else)
                    plus tuner.c (evaluation fitter) and datagen.c (NNUE data)
trainer/            the NNUE trainer: PyTorch, its own venv, its own tests
docs/               architecture, testing methodology, UCI reference
external/           gitignored: books, opponent engines, baselines, PGNs
.github/workflows/  CI
```

---

## Where to go next

Steps 1–5 of the original bring-up order (movegen, make/unmake, perft, eval,
search) are done, and so is the network: it is measurably stronger than the
tuned linear evaluation and the remaining work is about collecting the rest of
what it is worth. **Every change from here is measured with
`make sprt`** — read [docs/TESTING.md](docs/TESTING.md) first, because the
testing methodology is what separates an engine that improves from one that
drifts.

The open work, roughly in order of Elo per unit of effort:

1. **Re-tune the search parameters against the network.** Every threshold and
   formula constant in `search.c` was fitted against an evaluation with a
   particular scale and a particular noise profile, and the network has
   neither. [docs/NNUE.md](docs/NNUE.md) Task 4 calls this out as where a
   meaningful fraction of the total NNUE gain actually lives.

   `make TUNE_SEARCH=on` exposes **21** of them as UCI spin options — the seven
   margins, `CORR_W_PAWN`, and a second tier that had never been fitted at all:
   the LMR curve itself (`LmrBase`, `LmrDivisor`), the history divisors, the
   null-move reduction formula, the history bonus curve, the LMP constant and
   the aspiration delta. Fitting 21 interacting parameters one SPRT at a time
   is not possible, so `make tune` runs SPSA over the set and emits a
   *candidate*, which then needs its own SPRT against the values it replaced —
   SPSA has no null hypothesis and will report a walk as a result.

   Two of them are deliberately narrower than they look: `HistBonusDepthMax`
   above 20 and `NmpEvalMax` above 5 were measured to be indistinguishable from
   their bounds at bench depth 12, so a wider range would only give the sweep
   somewhere to random-walk. The seven *depth* thresholds are excluded for a
   related reason — SPSA perturbs continuously and the engine takes an int, so
   both sides of a gradient estimate round to the same small integer and the
   measurement is noise. Those want a sweep of their own.
2. **The next generation of training data, labelled by the network.** Every
   label in `gen-001` came from a 10,000-node search using the *classical*
   evaluation, because that was the only evaluation that existed when it was
   generated. That teacher is now the weaker of the two. Re-labelling at the
   same node count with `EVAL=nnue` costs nothing but machine time and raises
   the ceiling on every net trained afterwards — which is the bootstrap loop
   [docs/NNUE.md](docs/NNUE.md) is built around, and the single largest item
   on this list. `gen-001` is also 100% `human`-sourced; the self-play arm is
   configured in `tools/cloud/job.env` and has never been run.
3. **Widening past 1024 is now gated on data, not on time.** The item that sat
   here said the net stopped at epoch 3 and 512 hidden units, half the designed
   width, and called both free Elo. Both are done: the shipped net is
   `24576 -> 1024x2 -> 4`, tag `epoch20-h1024`, hash `1f36c07f4507`. It is
   rewritten rather than deleted because that claim outlived the net it
   described by many epochs, and a stale "free Elo" line is worse than no line
   — someone reads it and goes looking for Elo that has already been spent.
   `--hidden` accepts up to 2048, but the binding constraint has changed: 24576
   feature rows need a dataset where each row is seen more than a handful of
   times, so further width waits on step 2. Check the net a build is carrying
   with `make nnue-info` before trusting any width written down here.
4. **Lazy SMP.** `Threads` is capped at 1. The ordering tables in `search.c`
   and the accumulator stack in `src/nnue.c` are file-scope and must move into
   a per-thread block first. Worth nothing in a single-threaded SPRT and worth
   a great deal to anyone actually playing the engine — and it makes every
   future data generation run cheaper.
5. **Staged move generation**, so a node that cuts on the table move never
   generates or scores the rest of the list — in the three steps the status
   table breaks it into, each with its own SPRT.
6. **A policy head for move ordering** — [docs/NNUE.md](docs/NNUE.md) Task 5b.
   The cutoff-move sidecars are already written (`gen-001.pol`), so the data
   cost is sunk. Read the honest expectations in that section before starting:
   the track record in alpha-beta engines is much thinner than in MCTS ones,
   and history heuristics are already very good.

### Deliberately not on that list

Two items that were on it and have been retired, so they do not get picked up
again by accident:

- **A pawn hash table.** It accelerates `eval.c` and nothing else. With the
  network becoming the default evaluation, the code it speeds up is on its way
  out of the hot path, and the work would be spent the day before it stopped
  mattering.
- **Ablating the E1 and E10 batches.** Both went in as batches and neither
  attributes its gain to any individual term, which is a real gap in the
  record — but closing it *explains* Elo already banked rather than adding
  any. The pending table at the end of
  [docs/EXPERIMENTS.md](docs/EXPERIMENTS.md) is kept for the day the queue is
  empty; it should not compete with anything above.

---

## Documentation

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — how the modules fit together
- [docs/TESTING.md](docs/TESTING.md) — perft, bench, SPRT, OpenBench
- [docs/TUNING.md](docs/TUNING.md) — fitting the evaluation to real games
- [docs/NNUE.md](docs/NNUE.md) — the network plan: data schema, trainer, export, search integration
- [trainer/README.md](trainer/README.md) — running the trainer: setup, the pipeline, the sanity table
- [docs/EXPERIMENTS.md](docs/EXPERIMENTS.md) — every measured change and what it scored
- [docs/UCI.md](docs/UCI.md) — supported commands and options
- [docs/RELEASING.md](docs/RELEASING.md) — cutting a release, and how the net gets published

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
