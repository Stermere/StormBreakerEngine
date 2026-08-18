# ChessEngine

A UCI chess engine written in C, built for competitive strength.

The engine plays legal chess. Move generation is verified exact against the
published perft suites, and the search is the deliberate minimum: negamax with
alpha-beta, iterative deepening, quiescence and MVV-LVA ordering over a
material-only evaluation. That combination is the **baseline** — every change
from here is measured against it with SPRT rather than argued for.

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
| Search: iterative deepening, alpha-beta, quiescence | baseline |
| Evaluation: material only | baseline |
| **Transposition table probe / store** | **TODO** |
| **Killers, history, null move, LMR** | **TODO** |
| **Piece-square tables and beyond** | **TODO** |
| **Remaining optimizations and performance upticks** | **TODO** |
| **NNUE experiments and training pipeline. NN informed search** | **TODO** |

`make perft` and `make perft-all` pass exactly; `make openbench-check` passes,
so the engine can be registered with a distributed testing cluster.

---

## Quick start

```sh
make                     # build for this machine
make bench               # deterministic node-count benchmark
make perft               # move generation correctness suite
make openbench-check     # verify OpenBench compliance
make help                # all targets
```

First-time environment setup (Windows):

```powershell
pwsh tools\setup.ps1              # install fastchess, GUIs, Stockfish; fetch the book
pwsh tools\register-engines.ps1   # register the engine with Cute Chess
pwsh tools\sprt.ps1 -Smoke        # verify the match pipeline end to end
```

---

## Using the GUIs

```powershell
pwsh tools\gui.ps1                    # En Croissant (default)
pwsh tools\gui.ps1 -App encroissant   # Cute Chess
pwsh tools\gui.ps1 -App both
```

`gui.ps1` re-registers the engine against your current build before launching,
so the GUI never runs a stale binary. It starts the app detached — your terminal
stays usable.

> Until move generation exists the engine connects and handshakes correctly,
> then replies `bestmove 0000`. That is the expected behaviour, not a
> misconfiguration.

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
fast-chess -engine cmd=.\chessengine.exe name=dev `
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

On non-x86 targets (Apple Silicon, ARM) the arch profiles are ignored and a
portable build is produced automatically.

---

## Repository layout

```
src/                engine sources (flat, as in most strong engines)
tests/perft/        move generation correctness suites (EPD)
tools/              setup, GUI launch/registration, SPRT, gauntlet (PowerShell)
docs/               architecture, testing methodology, UCI reference
external/           gitignored: books, opponent engines, baselines, PGNs
.github/workflows/  CI
```

---

## Where to start building

The intended order, because each step is verifiable before the next:

1. **`src/movegen.c`** — pseudo-legal move generation.
2. **`board_do_move` / `board_undo_move`** in `src/board.c`.
3. **`make perft`** until every position in `tests/perft/` matches exactly.
   Do not proceed while a single count is wrong.
4. **`src/eval.c`** — material only, to begin with.
5. **`src/search.c`** — negamax, then quiescence, then the transposition table,
   then move ordering.
6. From here on, **every change is measured with `tools/sprt.ps1`**.

Read [docs/TESTING.md](docs/TESTING.md) before step 6 — the testing
methodology is what separates an engine that improves from one that drifts.

---

## Documentation

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — how the modules fit together
- [docs/TESTING.md](docs/TESTING.md) — perft, bench, SPRT, OpenBench
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
