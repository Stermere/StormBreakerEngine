# CLAUDE.md

Guidance for Claude Code when working in this repository.

## What this is

A UCI chess engine in **C** (C17), built for competitive strength.

Scaffolding, move generation, make/unmake, search and evaluation are all
complete and verified. The search is a PVS with a transposition table, null
move, LMR, singular extensions, SEE pruning and history heuristics; the
evaluation is material plus tapered piece-square tables. What remains is
tuning, Lazy SMP, and eventually NNUE — see the status table in README.md.

## Build and test

```sh
make                    # build (-march=native)
make ARCH=popcnt        # portable build, what CI uses
make debug              # assertions; sanitizers on POSIX
make bench              # deterministic node-count benchmark
make perft              # movegen correctness suite
make openbench-check    # verify OpenBench compliance
make format             # apply .clang-format
```

On Windows, `make` is MSYS2's (`C:\msys64\usr\bin\make.exe`) and the compiler is
MSYS2 UCRT64 gcc. PowerShell needs those on PATH.

## Non-negotiable invariants

Breaking any of these silently destroys the ability to measure progress, which
is worse than a bug — it makes every subsequent result untrustworthy.

1. **Bench must stay deterministic.** Identical node counts across runs and
   machines. Never seed Zobrist keys from `time()`, `rand()`, or any entropy
   source. Never let thread count or hash size affect the count.

2. **Bench must keep printing `<nodes> nodes <nps> nps`** as its final line.
   OpenBench parses exactly that.

3. **`make EXE=<name>`** must produce exactly `<name>` (or `<name>.exe`) beside
   the Makefile, and `CC=` must be honoured.

4. **The UCI loop must never block on the search.** `isready`, `stop` and
   `quit` are answered while thinking. The search runs on a worker thread; keep
   it that way.

5. **Board mutation goes through `board_put_piece` / `board_remove_piece` /
   `board_move_piece` only.** The bitboards, mailbox and piece counts must never
   be updated independently.

6. **Moves that did not come from the generator must be validated** with
   `movegen_is_pseudo_legal()` before being played. Transposition table hits can
   be stale or collided; playing one unchecked corrupts the board.

7. **`search_clear()` must reset everything that carries between searches** —
   transposition table, history, killers. Otherwise results depend on what was
   searched before, and reproducibility is gone.

## Testing discipline

Read [docs/TESTING.md](docs/TESTING.md) for the method and
[docs/EXPERIMENTS.md](docs/EXPERIMENTS.md) for what has already been measured —
record every new result there. The short version:

- **Correctness changes** (movegen, make/unmake): must pass `make perft`
  exactly. Not "close" — exactly.
- **Pure speedups**: bench node count must be *unchanged*. If it changed, the
  change is behavioural and needs an SPRT.
- **Behavioural changes** (search, eval, time management): require a passing
  SPRT via `tools\sprt.ps1`. Do not commit an Elo claim without one, and do not
  argue a patch is good because it seems obviously good — roughly half of
  "obviously better" chess engine patches measure neutral or worse.
- **One change per test.** Two at once tells you nothing about either.

Do not weaken a failing test to make it pass. If `perft` reports zero nodes, the
correct response is to implement movegen, not to relax the check.

## Code style

- C17, 4-space indent, 100-column limit, enforced by `.clang-format`.
  Run `make format` before committing.
- Comments explain **why**, not what. Prefer one good comment on a non-obvious
  decision over narrating obvious code.
- Mark unfinished work `TODO(engine):` with enough context to act on.
- Keep `src/` flat — it matches how strong engines are organised.
- No new dependencies. The engine links only against libc and the platform
  threading API, and that is a feature: it builds anywhere OpenBench runs.

## Where things live

| Path | Contents |
|---|---|
| `src/` | engine sources |
| `tests/perft/` | correctness suites (EPD) |
| `tools/` | setup, GUI launch/registration, SPRT, gauntlet (PowerShell) |
| `docs/` | architecture, testing, UCI reference |
| `external/` | **gitignored** — books, opponents, baselines, PGNs |

Never commit anything under `external/`, and never commit binaries or PGNs.
