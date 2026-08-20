# Experiment log

Every behavioural change, what it was tested against, and what it measured.

The point of this file is to make it impossible to lose track of *why* the
engine is the way it is. An engine accumulates dozens of small decisions, and
without a record of which ones were measured — and which were merely believed —
there is no way to tell a tuned parameter from a guess that nobody ever
revisited. When a change is later suspected of being wrong, the entry here is
what says whether it was ever actually tested and at what time control.

Read [TESTING.md](TESTING.md) first for the methodology. The short version:
behavioural changes need an SPRT, `bench` node counts are the fingerprint that
proves a "pure speedup" changed nothing, and one test measures one change.

---

## How to read an entry

| Field | Meaning |
|---|---|
| **Bench** | Node count at the default depth. Changing it proves the search changed. |
| **Baseline** | What it was measured against, by name in `external\baselines\`. |
| **TC / bounds** | Time control and SPRT bounds. STC is 8+0.08, bounds [0, 5]. |
| **Result** | `H1` = gains Elo, `H0` = does not, `capped` = no verdict within the game limit. |

`Elo` figures carry a 95% confidence interval. A result whose interval spans
zero has not shown anything, however good the point estimate looks.

---

## How the ablations below were built

For the duration of these experiments every tunable in `search.c` was wrapped
in `#ifndef`, so a variant was a build flag rather than a source edit — which is
what lets an ablation change exactly one thing:

```sh
make ARCH=popcnt EXE=abl-see CC="gcc -DSEE_CAPTURE_DEPTH=0 -DSEE_QUIET_DEPTH=0"
```

`CC=` rather than `CFLAGS=`, because a command-line `CFLAGS` overrides the
Makefile's own assignment and would silently drop `-O3` and the arch defines.

**That scaffolding has since been removed** — the constants are plain
`#define`s again. It served the experiments and then became a second way to
express the same values, which is a way for the shipped defaults and the tested
defaults to drift apart. To run another ablation, wrap the one constant you are
testing, measure it, and unwrap it again.

Two habits from this round worth keeping:

- **Diff the bench before spending an SPRT.** A variant whose node count equals
  the unmodified build is not testing anything, because the feature never
  fired. That is how E9 was found without playing a game.
- **Check the bench at more than one depth.** Singular extensions are invisible
  at the default depth 7 and only appear at depth 12, so a single shallow bench
  diff would have wrongly called them dead too.

---

## Results

### E1 — Search completeness batch

**Date** 2026-08-18 · **Baseline** `s10-conthist` · **Bench** 326193 → 244317

Singular extensions with multi-cut and negative extensions; SEE pruning of
captures and quiets; capture history; continuation history at 2 and 4 plies as
well as 1; `cutNode` propagated and used to reduce harder; razoring; quiescence
delta pruning; continuation-history pruning; `seldepth` reporting. Plus the
en-passant hashing fix (`board.c`) and time management by game phase and
best-move stability (`timeman.c`).

| | |
|---|---|
| TC / bounds | STC 8+0.08, [0, 5] normalized |
| **Result** | **H1 accepted** — LLR 2.96 at 774 games |
| Elo | **+75.69 ± 18.80** (nElo +101.79 ± 24.48) |
| Record | 336W / 170L / 268D, 60.72%, Ptnml [11, 60, 135, 114, 67] |

Ten changes in one test, which is against the rules in TESTING.md and is why
the ablations below exist. It establishes that the batch is worth keeping; it
attributes nothing to any individual part of it.

---

### Ablations of E1

Each measures one feature by removing it from the current build and playing the
full build against it. **H1 means the feature earns its place.**

All at STC 8+0.08, bounds [0, 5] normalized, capped at 2000 games. **Elo is
stated for the feature**, i.e. how much the full build beat the build without
it. Positive means the feature earns its place.

| # | Feature removed | Bench (d7 / d12) | Games | Elo for the feature | Read |
|---|---|---|---|---|---|
| E2 | SEE pruning | 298987 / — | 2000 | **+18.61 ± 11.63** | keep — interval excludes zero |
| E3 | Capture history | 244192 / 4563899 | 2000 | +6.95 ± 11.37 | keep, unproven |
| E4 | Quiescence delta pruning | 273455 / — | 2000 | +9.73 ± 11.27 | keep, unproven |
| E5 | Singular extensions | 244317 / 4385171 | 2000 | +10.77 ± 11.45 | keep, unproven |
| E6 | Cut-node extra reduction | 254099 / — | 1405 | **−16.09** [−34.3, +2.1] | **REMOVED — see below** |
| E7 | Razoring | 249177 / — | — | not run | |
| E8 | Continuation history beyond 1 ply | 246119 / — | — | not run | |

None of E2–E5 reached an SPRT verdict; all four ran to the 2000-game cap. The
test design was underpowered for effects of this size — resolving a ±5 Elo
question needs several times 2000 games, and each of these cost about 75
minutes. Their point estimates are all positive and none is individually
certified. E7 and E8 were cancelled for time.

**E2 detail.** 697W / 590L / 713D, 52.68%, Ptnml [69, 216, 363, 243, 109]. LLR
reached 1.81 of the 2.94 needed. The 95% interval [+7.0, +30.2] excludes zero,
so this is a real gain that simply needed more games to certify.

A lesson about reading tests early, worth keeping: at 280 games E2 showed
+6.20 ± 32.26 and looked neutral. It finished at +18.61. Rule 4 in TESTING.md
exists for a reason.

---

### E6 — the cut-node extra reduction looks actively harmful

**Date** 2026-08-19 · Stopped at 1405 games, so this is a partial result
recovered from the PGN rather than a completed test.

```
dev vs no-cutnode:  411W / 476L / 518D over 1405 games
score 47.69%   Elo -16.09   approx 95% CI [-34.3, +2.1]
```

This is the only ablation whose point estimate is negative, and it is the
largest magnitude of any of them. Reading it in the direction that matters:
**removing `r += CUTNODE_REDUCTION` appears to gain about 16 Elo.**

The interval still grazes zero, so it is not proof. But it is 1405 games of
evidence pointing one way, which is more than most engine changes ever get, and
it is the one result that contradicts the assumption that everything in the E1
batch was pulling its weight.

Adding two plies of reduction at expected-cut nodes is standard practice in
stronger engines, but they surround it with far more machinery — verification
re-searches, depth- and history-dependent reduction curves, better move
ordering underneath it. Dropped straight onto this LMR formula, two plies is
plausibly just too aggressive.

**Action:** removed. Bench moved 244317 → **254099**, exactly matching the
`abl-cutnode` binary this was measured against, which confirms the change
shipped is precisely the change tested and nothing else rode along with it.

`cutNode` itself is untouched and still earns its place — it is propagated to
the child searches, where it decides which of them are searched as expected
fail-highs. Only the extra `r += 2` in the LMR formula is gone.

**Caveat worth keeping in view:** this rests on a stopped test whose interval
grazes zero, not on a completed SPRT. The honest reading is "two plies was too
aggressive here", not "cut-node reductions do not work". A reduction of 1, or 2
behind a verification re-search, is untested and may well beat both.

---

### E9 — Continuation-history pruning is a no-op

**Date** 2026-08-18 · **No SPRT was run, and none is needed.**

Disabling it (`-DCONTHIST_PRUNE_DEPTH=0`) leaves the bench node count
*identical* at both depth 7 (244317) and depth 12 (4099394). A feature that
cannot change a single node in a 4-million-node search is not doing anything.

The code path is reachable — building with `-DCONTHIST_PRUNE_MARGIN=1` moves
the bench to 238973 / 4470691 — so this is the margin never being met, not
unreachable code. The reason is ordering: a quiet move whose continuation
history is bad enough to trip `contScore < -4096 * depth` also scores low
enough to sort near the end of the move list, where late move pruning has
already discarded it. LMP subsumes the rule entirely at depths 1–4.

Worth noting from the same diagnostic: at margin 1 the depth-12 tree gets
*larger* (4470691 vs 4099394), because pruning moves that mattered causes fail
lows and re-searches. Aggressive continuation-history pruning is not obviously
free.

**Action:** removed. A version with a margin loose enough to fire before LMP
does is a legitimate future experiment, but it is a different change and needs
its own test.

---

### E10 — The evaluation: new terms, and weights fitted to human games

**Date** 2026-08-19 · **Baseline** `eval-base` (commit 1ad1c42) · **Bench** 254099 -> 287826

The largest single change the engine has had, and deliberately tested as one
feature rather than as a dozen. Everything below shipped together:

- **New terms.** Pawn structure (isolated, doubled, backward, connected,
  phalanx, passed by rank with blocked/defended/king-distance variants,
  candidate passers), mobility per piece type, king safety (pawn shelter and
  storm, king-ring attacker count and weight, safe checks, king on an open
  file), threats by pawn/minor/rook/king, hanging and restricted squares,
  bishop pair, bad bishop, outposts, rook on open and semi-open files, tempo.
- **King-relative placement.** 6144 weights: piece placement conditioned on
  which of 8 buckets each king stands in, for the own king and the enemy king
  separately. A deliberately NNUE-shaped feature set (factorised HalfKA), so
  the extraction transfers to the network's input layer later.
- **All 13,684 weights fitted** to 22,578,820 quiet positions drawn from
  3,360,336 human games (12 months of the Lichess Elite database, CC0),
  labelled with the result of the game they came from. See
  [TUNING.md](TUNING.md).

| | STC | LTC |
|---|---|---|
| TC / bounds | 8+0.08, [0, 5] normalized | 40+0.4, [0.5, 4.5] normalized |
| **Result** | **H1** — LLR 2.94 at 372 games | **H1** — LLR 2.96 at 428 games |
| Elo | **+269.69 ± 42.72** | **+327.16 ± 43.28** |
| nElo | +327.54 ± 35.31 | +430.03 ± 32.92 |
| Record | 294W / 52L / 26D, 82.53% | 359W / 44L / 25D, 86.80% |
| Ptnml | [3, 2, 45, 22, 114] | [0, 2, 43, 21, 148] |

**The gain is larger at LTC than at STC** (+327 vs +270), which is the
direction that matters. An evaluation change that only helps at shallow depth
is usually an artefact — it is scoring positions the search would have resolved
anyway. Growing with depth means the opposite: the search is being pointed
somewhere better, and it has further to run once pointed. It also fits the
mechanism, since a king-safety blindness gets more punishing the deeper the
opponent can see.

**Why it is this large.** The baseline evaluation was material and piece-square
tables and nothing else — it had no notion of king safety whatsoever. 750 of
the 800 games ended in mate. An engine that cannot see its own king being
surrounded loses to one that can, and it loses by getting mated, which is
exactly the shape of this result. Do not read +270 as evidence that the terms
are individually well tuned; read it as evidence of how much was missing.

Verified against the obvious artefact: every one of the 800 games across both
time controls terminated normally — 750 by mate, 50 by a draw rule. Zero time
forfeits, zero crashes, zero illegal moves. Both binaries were built
`ARCH=native` from the same compiler with the same hash and thread settings.

**Cost.** The evaluation is about five times the work it was: nps fell from
1,568,512 to 1,256,882 on the bench, a 20% loss, which is worth roughly -20
Elo on its own. The terms paid for that many times over, but the figure is
worth keeping in view — a future pawn hash table would recover most of it as a
pure speedup, provable by an unchanged bench node count.

**What this does NOT establish.** Ten-plus changes went in at once, exactly as
in E1, and for the same reason: the harness had to exist before any of it could
be measured at all. The batch is worth keeping. Nothing here attributes any of
the gain to any individual term, and with an effect this large it is entirely
possible some component is neutral or negative and is being carried by the
rest. The ablations worth running, in rough order of how much is riding on an
untested assumption:

| Ablation | Question it answers |
|---|---|
| King-relative tables zeroed | Are the 6144 capacity weights earning their cache pressure, or is the classical tier doing all the work? |
| Untuned weights, terms only | How much of +270 is the terms and how much is the fitting? |
| Self-play data instead of human | The label-source argument in TUNING.md, measured rather than argued |
| King safety removed | Almost certainly the largest single component; worth sizing |

The self-play arm already exists: `external/games/*.pgn` extracts to 205,734
positions and fits with the same command.

**A caution about the fitted numbers.** `Material[]` came out at pawn 43,
rook 386/677, which looks broken and is not. Material is collinear with the
three placement tables — the tuner can move value between them because only
their sum appears in a score. Measured end to end, by deleting a piece from
real positions and diffing the evaluation, the engine prices a pawn at 107 and
a rook at 550. Read the sums, never the individual tables.

---


## Absolute strength

Every Elo figure above is relative to another build in `externalaselines`,
none of which is itself rated. This anchors them. `toolsating.ps1` plays a
ladder of Stockfish `UCI_Elo` rungs and fits one rating by inverse-variance
weighting.

**2026-08-19**, after E10, at STC 8+0.08, 100 games per rung:

| SF rung | W-L-D | score | implied rating |
|---|---|---|---|
| 2600 | 70-20-10 | 75.0% | 2791 ± 79 |
| 2800 | 38-41-21 | 48.5% | 2790 ± 68 |
| 3000 | 20-63-17 | 28.5% | 2840 ± 75 |
| 3190 | 8-74-18 | 17.0% | 2915 ± 91 |

**Combined: 2826 ± 38.**

Read the 2600 and 2800 rungs, not the combined figure. Those two bracket the
50% point, so they need the least extrapolation — and they agree to within one
Elo (2791 and 2790) from positions 400 nominal points apart, which is about as
strong an internal consistency check as this method offers. The drift upward at
3000 and 3190 is `UCI_Elo` compressing near full strength, not the engine
outperforming there.

**The honest number is ~2790 at blitz, ±100 or so.** The ±38 is statistical
only and badly understates the real uncertainty: `UCI_Elo` is Stockfish's own
calibration rather than the CCRL scale, a strength-limited engine makes
occasional deliberate errors instead of being uniformly weaker, and ratings are
time-control specific. Do not quote it as a rating; use it to decide whether a
milestone has been passed.

Verification that the ladder engages at all, rather than silently running a
full-strength Stockfish in every seat: SF-1320 lost 0-22 to SF-2600, and the
score above falls monotonically as the rung rises. `UCI_Elo` is ignored unless
`UCI_LimitStrength` is also set, and setting one without the other fails
silently - which is exactly the mistake that would make an engine look 400
points weaker than it is.
