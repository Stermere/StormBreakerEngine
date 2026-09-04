# Testing methodology

Engine changes are checked with three kinds of test, each answering a different
question.

| Test | Question | When |
|---|---|---|
| **perft** | Is move generation legal and complete? | Before any search work |
| **bench** | Did this change alter the search, and how fast is it? | Every build |
| **SPRT** | Does this change actually gain Elo? | Every behavioural change |

---

## 1. perft — correctness

```sh
make perft         # standard + Chess960, depth-capped, plus all edge cases
make perft-all     # full depths (slow: ~4.8 billion nodes)
make chess960-test # the Chess960 checks a node count cannot make
```

Perft counts the leaf nodes of the legal move tree. The reference counts are
exact. A mismatch indicates a move-generation or position-state bug and must be
resolved before Elo testing.

Four suites, and `make perft` runs all of them:

| Suite | What it is for |
|---|---|
| `standard.epd` | the six positions everyone benchmarks against |
| `tricky.epd` | one line per standard-chess rule that is easy to get wrong |
| `chess960.epd` | one line per Chess960 castling rule, several as pairs |
| `chess960-startpos.epd` | all 960 start positions - the broad net |

The Chess960 suites are part of the same gate rather than an optional extra:
castling geometry is shared code, derived per position for both variants, so a
change made for standard chess can break Chess960 and the reverse.

### Debugging a mismatch

```
position fen <the failing fen>
perft 4
```

That prints a per-move breakdown. Run `go perft 4` on the same position in
Stockfish, diff the two lists, then recurse into the first move whose subtotal
differs. A few iterations isolates the exact position and move that is wrong.

`tests/perft/tricky.epd` and `tests/perft/chess960.epd` are organised so each
line names the rule it tests — when one fails, the comment above it tells you
what you broke. Several Chess960 lines come in **pairs**: the same geometry with
and without one enemy piece, or the same board in both FEN spellings. A pair
says which of the two rules broke; a single count only says it broke.

For Chess960, use `setoption name UCI_Chess960 value true` in Stockfish before
comparing, or the two engines will spell castling differently and every divide
will look wrong.

### Where the Chess960 counts came from

There are no published perft numbers for Chess960 beyond the standard array, so
the counts were **sealed from an independent engine** rather than written from
memory — the same method the Syzygy prober was verified with (E24), and for the
same reason: a suite written from recollection certifies whatever the
implementation happened to do.

```sh
make chess960-campaign                     # re-run the differential campaign
python tools/chess960diff.py reseal tests/perft/chess960.epd
```

Re-seal after any deliberate change to castling, and only after the campaign
passes. Re-sealing a suite to make it pass is how the numbers stop meaning
anything.

### What perft cannot see

The gates live in `src/test/`, compiled into the engine rather than into a
separate binary — so `make chess960-test` and `make syzygy-test` always test
the build that actually plays, and `engine bench` and the UCI `bench` command
can never diverge.


`make chess960-test` covers four things a node count is blind to, because each
leaves every count correct:

- **The SP numbering.** Perft proves the position it was handed is played
  right; it cannot know that position was the one asked for. SP 518 being the
  standard array is the external anchor.
- **FEN round-trips.** A suite that only reads FENs never exercises the writer,
  and `KQkq` provably cannot describe some Chess960 boards.
- **Notation ambiguity.** On a board with the king on f8, castling short and
  stepping to g8 are both `f8g8` in standard spelling. Perft counts two moves
  and is content; a GUI plays the wrong one.
- **do/undo.** Counts come out right if the two are wrong in exactly opposite
  ways, which castling makes easy — it moves two pieces, and they can swap.

### Playing actual Chess960 games

Worth doing once after any castling change: perft proves the move list, a game
proves the engine survives its own bestmove coming back through `position ...
moves` for eighty moves. fast-chess validates every move against its own board,
so an illegal one is reported rather than played.

```sh
# fast-chess refuses to start a 960 match without a book, and the engine
# can emit one - all 960 arrays, in the spelling it will be sent them in.
./stormbreaker chess960 sp | sed 's/^ *[0-9]* *//' \
    > external/books/chess960-startpos.epd

fast-chess -engine cmd=./stormbreaker name=A -engine cmd=./stormbreaker name=B \
    -each tc=2+0.02 option.Hash=16 -variant fischerandom \
    -openings file=external/books/chess960-startpos.epd format=epd order=random \
    -rounds 40 -games 2 -concurrency 4 -pgnout file=external/games/c960.pgn
```

Then check the games actually castled, or the run proved nothing about the
feature under test:

```sh
grep -c "O-O" external/games/c960.pgn
```

---

## 2. bench — speed and identity

```sh
make bench
./stormbreaker bench      # prints "<nodes> nodes <nps> nps"
```

Bench serves two purposes:

- **Speed.** `nps` is the number to watch when optimising.
- **Regression detection.** The deterministic node count is used as a search
  fingerprint for the benchmark positions.

For a change intended only to improve speed, the node count must remain
identical. A changed count indicates changed search behaviour and requires an
SPRT rather than a timing comparison alone.

**Determinism is a hard requirement.** The count must be identical across runs
and machines. Zobrist keys use a fixed seed for exactly this reason. OpenBench
rejects engines that fail this.

---

## 3. SPRT — Elo

```powershell
make snapshot ARGS="--name v0.1"   # freeze the current build
# ... make your change, rebuild ...
make sprt                          # dev vs baseline, STC
make sprt ARGS="--tc LTC"          # confirm at long time control
```

SPRT plays games until it can accept or reject a hypothesis, then stops. A
clearly good or clearly bad patch resolves in a few hundred games; only
genuinely marginal ones cost tens of thousands.

### Reading the result

fastchess prints a running verdict:

- **H1 accepted** — the result supports the configured Elo-gain hypothesis.
- **H0 accepted** — the result does not support that hypothesis.
- **continuing** — not enough evidence yet.

Also watch `Ptnml(0-2)`, the pentanomial breakdown of game *pairs*. Pairing
games from the same opening with reversed colours removes most opening luck,
which is why fastchess reports results this way and why `-repeat` is always on.

### Time controls and bounds

| Preset | Time control | Bounds | Use |
|---|---|---|---|
| `VSTC` | 2+0.02 | [0, 5] | quick sanity check |
| `STC` | 8+0.08 | [0, 5] | the default working test |
| `LTC` | 40+0.4 | [0.5, 4.5] | confirmation before committing |

Some patches only help at depth. A patch that passes STC should be confirmed at
LTC before it is committed; a patch that fails STC but you believe helps at long
time control needs LTC evidence, not an argument.

### Rules that keep results meaningful

1. **One change at a time.** Two changes in one test tell you nothing about
   either.
2. **Never run other heavy work during a match.** The concurrency default
   already leaves two cores free. Contention distorts time-based results.
3. **Use the opening book.** Without it, games repeat and the test is
   dramatically less informative per game.
4. **Do not stop a test early because it looks good.** That is p-hacking, and
   SPRT's error bounds assume you did not do it.
5. **Re-baseline deliberately.** Always testing "new vs previous" lets small
   errors compound in one direction. Keep a fixed baseline, promote it on
   purpose, and periodically run a gauntlet against older versions to confirm
   real cumulative progress.

---

## 4. Gauntlets — absolute strength

```powershell
make gauntlet ARGS="--games 200"
make gauntlet ARGS="--field engines --games 200"
make gauntlet ARGS="--field stockfish --games 200"
make ratings ARGS="external/games/<stamp>-gauntlet.pgn"
```

Self-play SPRT compares a change with its baseline. A gauntlet estimates
strength against a broader field and can expose opponent-specific regressions.

`--field stockfish` plays Stockfish and nothing else, at full strength — the
engine is now close enough for that to be informative rather than a guaranteed
100% loss. `--skill-level N` handicaps it if a weaker seat is wanted, but a
handicapped Stockfish plays mostly full-strength moves with occasional
deliberate errors, which is not how a genuinely weaker opponent errs, and the
level is not an Elo scale. For an absolute reading, `--field engines` is the
ladder to use — see "Absolute strength" in [EXPERIMENTS.md](EXPERIMENTS.md).

The Elo column fastchess prints is relative to the field mean, so it moves when
the field does and two gauntlets with different opponents cannot be compared.
`make ratings` fixes that: it re-reads the PGN, prints the full W-L-D
cross-table, and fits every seat against the seven rated rungs at once to put
the whole table on the CCRL Blitz scale. `make gauntlet` runs it automatically
when the match finishes; the target is for re-reading an older or interrupted
PGN.

Read the residual column before the estimate. A rung the engine sweeps locates
almost nothing — 93% of the points against a 3008 engine pins its rating to
about ± 126 — so an estimate above the top rung is an extrapolation off the
least-constrained end of the ladder, however tight its bar looks. When the
engine outgrows the top rung, add a rung, do not trust the extrapolation:
the ladder lives in `CCRL_LADDER` in [tools/common.py](../tools/common.py).

---

## 5. OpenBench — distributed testing

```sh
make openbench-check
```

Once the engine is playing, throughput becomes the bottleneck: a 16-core machine
takes hours per LTC test. [OpenBench](https://github.com/AndyGrant/OpenBench)
distributes games across many machines.

This repository already satisfies the compliance requirements:

- `make EXE=<name>` produces exactly `<name>`, and `CC=` is honoured
- `./<binary> bench` prints `<nodes> nodes <nps> nps`
- `Hash` and `Threads` UCI options are exposed
- bench node counts are deterministic (fixed-seed Zobrist keys)

`make openbench-check` verifies all four. Keep it passing.

---

## Continuous integration

`.github/workflows/ci.yml` runs on every push:

- builds on Linux, Windows and macOS with gcc and clang
- verifies the UCI handshake and required options
- verifies bench output format and determinism
- verifies the `make EXE=` contract
- runs an ASan/UBSan build
- checks `clang-format`

The perft and sanitizer-perft jobs are hard failures because a wrong count can
indicate illegal move generation and invalidate later measurements.
