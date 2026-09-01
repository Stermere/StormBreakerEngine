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
make perft         # standard positions to depth 4, plus all edge cases
make perft-all     # full published depths (slow)
```

Perft counts the leaf nodes of the legal move tree. The reference counts are
published and exact. A mismatch indicates a move-generation or position-state
bug and must be resolved before Elo testing.

### Debugging a mismatch

```
position fen <the failing fen>
perft 4
```

That prints a per-move breakdown. Run `go perft 4` on the same position in
Stockfish, diff the two lists, then recurse into the first move whose subtotal
differs. A few iterations isolates the exact position and move that is wrong.

`tests/perft/tricky.epd` is organised so each line names the rule it tests —
when one fails, the comment above it tells you what you broke.

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
