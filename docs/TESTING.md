# Testing methodology

The single thing that separates a chess engine that gets stronger from one that
drifts sideways is testing discipline. Chess engine changes are famously
counter-intuitive: roughly half of all "obviously better" patches measure as
neutral or worse. Intuition is not evidence, and neither is winning a few games.

There are three distinct kinds of test here, and they answer different questions.

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

Perft counts the leaf nodes of the legal move tree. The correct counts are
published and exact, so any disagreement is a bug in your code — never in the
reference.

**Nothing else matters until perft is exact.** A movegen bug means the engine
plays or accepts illegal moves, which surfaces as forfeited tournament games and
as noise that makes every Elo measurement meaningless.

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
./chessengine bench      # prints "<nodes> nodes <nps> nps"
```

Bench serves two purposes:

- **Speed.** `nps` is the number to watch when optimising.
- **Identity.** The node count is a fingerprint of the search. Two builds with
  the same count search identically.

That second property is what makes a **pure speedup** provable: if you rewrite
something to be faster without intending to change what it searches, the node
count must be *identical*. If it changed, you changed behaviour too — and now
you need an SPRT, not a stopwatch.

**Determinism is a hard requirement.** The count must be identical across runs
and machines. Zobrist keys use a fixed seed for exactly this reason. OpenBench
rejects engines that fail this.

---

## 3. SPRT — Elo

```powershell
pwsh tools\snapshot-baseline.ps1 -Name v0.1   # freeze the current build
# ... make your change, rebuild ...
pwsh tools\sprt.ps1                            # dev vs baseline, STC
pwsh tools\sprt.ps1 -Tc LTC                    # confirm at long time control
```

SPRT plays games until it can accept or reject a hypothesis, then stops. A
clearly good or clearly bad patch resolves in a few hundred games; only
genuinely marginal ones cost tens of thousands.

### Reading the result

fastchess prints a running verdict:

- **H1 accepted** — the patch gains Elo. Commit it.
- **H0 accepted** — the patch does not gain Elo. Discard it, however good the
  idea seemed. This will happen often. It is the system working.
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
pwsh tools\gauntlet.ps1 -Games 200
pwsh tools\gauntlet.ps1 -IncludeStockfish -SkillLevel 3
```

Self-play SPRT answers "better than before". A gauntlet answers "how strong,
really". It also catches regressions that self-play hides — an engine can beat
its previous self while getting worse against different opponents.

Full-strength Stockfish will win 100% and teach you nothing; use `-SkillLevel`
to pick a useful opponent and raise it as you improve.

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

The perft and sanitizer-perft jobs are hard failures. They are the most
valuable jobs in the file: a wrong count means the engine plays or accepts
illegal moves, and nothing measured after that point means anything.
