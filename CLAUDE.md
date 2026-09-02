# CLAUDE.md

Guidance for Claude Code when working in this repository.

## What this is

A UCI chess engine in **C** (C17), built for competitive strength.

Scaffolding, move generation, make/unmake, search and evaluation are all
complete and verified. The search is a PVS with a transposition table, null
move, LMR, singular extensions, SEE pruning and history heuristics. Chess960 is
supported in full — castling geometry is derived per position, so both variants
share one generator — though nothing in the *evaluation* has been tuned for it. The
evaluation is a 13,684-parameter linear model — material, piece-square tables,
king-relative placement, mobility, pawn structure, king safety and threats —
fitted to 22.6M positions from human games by `tools/tuner.c`.

A network exists alongside it: `trainer/` fits it, `tools/export_net.py`
quantises it, and `src/nnue.c` runs it, bit-exactly against the Python
reference. It is **not** the default evaluation — `make EVAL=nnue` builds it,
`make` still builds the classical one, and it stays that way until the
integration passes an SPRT (docs/NNUE.md, Task 4). What remains is ablating the
eval batch, Lazy SMP, and NNUE Tasks 4-5 — see the status table in
docs/STATUS.md.

## Build and test

```sh
make                    # build (-march=native)
make ARCH=popcnt        # portable build, what CI uses
make debug              # assertions; sanitizers on POSIX
make bench              # deterministic node-count benchmark
make perft              # movegen correctness suite
make openbench-check    # verify OpenBench compliance
make format             # apply .clang-format
make tuner              # build the evaluation fitter (docs/TUNING.md)

make EVAL=nnue          # any build, with the network instead of eval.c
make net-fetch          # fetch the pinned net; a clean clone has none
make nnue-test          # C inference == the quantised reference, exactly
make datagen-test       # datagen round-trips and its labels reproduce
make trainer-test       # the trainer's pytest suite

make syzygy-fetch       # 3-4-5-man tablebases (~939 MB into external/)
make syzygy-test        # known endgames, plus the sealed probe manifest

make chess960-test      # the Chess960 checks a node count cannot make
make chess960-campaign  # differential perft vs ORACLE= (default stockfish)
```

Tablebases are **off unless pointed at** — `SyzygyPath` over UCI, `-syzygy`
for datagen — and `make bench` never points at them, so a build with tables on
disk benches identically to one without. That is not a convenience: node
counts that depended on which files a machine happened to have would break
invariant 1 outright.

`EVAL` picks the evaluation at compile time; `classical` is the default and
there is no runtime switch. Switching `EVAL` or `ARCH` rebuilds correctly - a
`.buildflags` stamp is a prerequisite of every binary, because otherwise make
sees the same sources and the same output name and hands back the binary built
with the *other* flags.

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

8. **A net is described by its own file, and every rejection names the field.**
   `src/nnue.c` reads the architecture out of the net's header rather than from
   constants compiled beside it, so retraining wider is a drop-in. Anything the
   loader cannot handle must fail loudly at load with the field and both values
   - a net that is misread scores plausibly and loses Elo silently, which is the
   worst failure mode in this repository. `make nnue-test` is the gate: the C
   inference must match the quantised Python reference EXACTLY on 10,000
   positions, never approximately.

9. **Every evaluation weight goes through `TERM()` in `eval.c`, and every
   table is registered once in `EVAL_PARAM_TABLES`.** That macro emits the
   score contribution and the tuner's gradient coefficient from the same
   expression. A term that adds to a score by hand is invisible to the tuner,
   which then optimises a model that is not the one being run — and nothing
   crashes, the engine just quietly gets worse while the fit reports success.

10. **The classical evaluation must stay linear in its weights.** The tuner is
    plain gradient descent on a dot product. A term whose *weight* changes which
    *coefficients* get produced (a threshold on the running score, a lazy exit)
    silently breaks that assumption. This governs `eval.c` only; the network is
    nonlinear by design, and `eval_classical()` stays reachable by name in every
    build precisely so the tuner keeps fitting the model it thinks it is.

## Testing discipline

Read [docs/TESTING.md](docs/TESTING.md) for the method and
[docs/EXPERIMENTS.md](docs/EXPERIMENTS.md) for what has already been measured —
record every new result there. The short version:

- **Correctness changes** (movegen, make/unmake): must pass `make perft`
  exactly. Not "close" — exactly. That includes the two Chess960 suites: the
  castling geometry is one code path for both variants, so a standard-chess
  change can break Chess960 and the reverse.
- **Pure speedups**: bench node count must be *unchanged*. If it changed, the
  change is behavioural and needs an SPRT.
- **Behavioural changes** (search, eval, time management): require a passing
  SPRT via `make sprt`. Do not commit an Elo claim without one, and do not
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
  `src/syzygy.c` is this engine's own Syzygy prober rather than a vendored
  one for exactly that reason — but it is *derived* from Fathom and de Man's
  reference, and CREDITS.md says so and retains the MIT notice. Treat the
  constant index tables in it as the file format, not as code: they cannot be
  written differently and must never be "tidied".

## Where things live

| Path | Contents |
|---|---|
| `src/` | engine sources |
| `tests/perft/` | correctness suites (EPD) |
| `tools/` | SPRT, SPSA tuning, gauntlet, baselines (Python, stdlib only). Setup, GUI launch and engine registration stay PowerShell - they are Windows integration. Invoke the Python ones through `make sprt` / `make tune` / `make gauntlet` / `make snapshot`, never by naming an interpreter |
| `tools/tuner.c` | the evaluation fitter; **not** part of the engine binary |
| `tools/export_net.py` | quantises a checkpoint into `.nnue` + the equivalence vectors |
| `trainer/` | PyTorch NNUE trainer, its own venv, not subject to the C style rules |
| `docs/` | architecture, testing, tuning, releasing, UCI reference |
| `external/` | **gitignored** — books, opponents, baselines, PGNs, training data |

Never commit anything under `external/`, and never commit binaries or PGNs.
