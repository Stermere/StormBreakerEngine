# Experiment log

This log records behavioural changes, their baselines, test conditions, and
measured results. It includes accepted, rejected, and inconclusive changes so
past decisions can be reviewed against the evidence available at the time.

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

### E11 — The network replaces the classical evaluation

**Date** 2026-08-23 · **Baseline** `stormbreaker.exe`, classical (bench 287826) ·
**Dev** `stormbreaker-nnue.exe`, net `848d2b21e0d0`, commit 74301a7 (bench 269601)

Task 4 of [NNUE.md](NNUE.md): the network, called by the search, against the
13,684-parameter linear model E10 fitted. The dev build carries the incremental
accumulator, so the network is evaluated at roughly the speed of the classical
one rather than at two thirds of it.

| | STC |
|---|---|
| TC / bounds | 8+0.08, [0, 5] normalized |
| **Result** | **H1** — LLR 2.95 at 390 games |
| Elo | **+238.05 ± 35.48** |
| nElo | +313.06 ± 34.48 |
| Record | 272W / 40L / 78D, 79.74% |
| Ptnml | [3, 7, 31, 63, 91] |
| PGN | `external/games/20260823-161051-STC.pgn` |

**Cost, and why it is nearly zero.** The from-scratch accumulator evaluated the
net at 940k nps against the classical evaluation's 1184k — a 21% deficit paid
at every node. The incremental accumulator (E12 below) removes almost all of
it: 1133k nps, within 4% of classical, at an identical bench node count. The
network is now approximately free relative to the evaluation it replaces, which
is why this measures as eval quality rather than as a trade.
---


### E13 — Trainer throughput, and where the bottleneck actually is

**Date** 2026-08-26 · **Not an Elo test.** No engine change, no SPRT. The
network this produces is the same network; what moved is how long it takes to
fit one, so the measurement is positions/s and the gate is that the loss curve
is unchanged.

Preparing a 500M-position dataset. The trainer gained a chunked loader above
~2 GB, int32 feature indices, fused AdamW, virtual epochs and `--resume`.

**End to end**, `--hidden 1024 --output-buckets 4 --batch-size 16384
--workers 6`, on `gen-012.cnn` (8.14 GB, 254M records), RTX 3070. Steady-state
rate between batch 200 and batch 1200, so startup is excluded; run in both
orders to control for the page cache:

| | before | after |
|---|---|---|
| steady state | 349k pos/s | **392k pos/s** (+12%) |
| 1200 batches, wall clock | 67s | **53s** |
| time to batch 200 | 19.7s | **10.5s** |
| loss at batch 1200 | 0.008727 | 0.008720 |

The loss column is the point: the two curves agree at every logged batch
(0.019059/0.019162, 0.013404/0.013445, 0.011245/0.011256, ...), which is what
"no functional change" has to mean for a change that touches the optimiser and
the loader.

**Where the time actually goes**, measured separately rather than assumed:

| component | before | after |
|---|---|---|
| loader alone, 6 workers, 16.3 GB / 509M records | 1.11M pos/s | 1.23M pos/s |
| model step alone, synthetic batches, batch 16384 | — | 323k pos/s |
| model step alone, batch 32768 | — | 346k pos/s |

**The loader was never the bottleneck, and this file and trainer/README.md both
said it was.** At 1.2M pos/s the loader is moving 38 MB/s of records — it is
CPU-bound in `unpack`, not IO-bound — while training consumes 390k pos/s. Three
times headroom. The README advice to reach for `--workers` when a run is slow
was wrong past about six workers, and has been corrected.

Attribution of the +12%, from the component runs:

- **fused AdamW: +7%** (323k vs 302k on the model step alone). The single
  largest item, and it is the same arithmetic in one kernel launch over 25M
  parameters.
- **int32 indices**: the rest. Halves the (B, 32) index matrices, which are
  memset in a worker, copied into pinned memory and pushed over PCIe.
- **removing the per-step `.item()`: ~0.5%, not the win it looked like.** A
  saturated GPU makes that sync nearly free, because it waits on work that had
  to finish anyway. Kept because it costs nothing and stops being free on the
  configurations where the GPU is not saturated, but it should not be cited as
  a speedup.
- **the chunked loader**: little of the throughput. It is worth ~11% of loader
  throughput that training cannot spend, and half the time to steady state.

**What the chunked loader is actually for**, since it is not throughput. On an
NVMe the memmap path did *not* collapse at 16.3 GB — 1.11M pos/s, because
scattered 512 KB reads at 38 MB/s do not trouble a drive that can seek. It
earns its place on two other grounds: it holds one 64 MB buffer per worker
rather than mapping 16 GB and leaving residency to the OS, and it recomposes
every batch each epoch, where the memmap path freezes each batch at whatever
grouping the file's record order gave it and reuses it every epoch. On a
spinning disk, or a dataset well past RAM, the read pattern would matter too.

`--batch-size 32768` is worth a further ~7% and was not adopted as the default:
it changes the optimisation, so it is a hyperparameter for a run to choose, not
a speedup to take for free.
---


### E14 — Correction history

**Date** 2026-08-26 · **Baseline** commit c009931, built two ways:
`base-classical` (bench 287826) and `base-nnue`, net `1f36c07f4507` (bench
229281) · **Dev** the same two builds with correction history (bench 299634
classical, 213141 nnue)

Task 5a of [NNUE.md](NNUE.md), which asks for it "first regardless of NNUE
progress" — it needs no network and it composes with one rather than competing.
The search records how far apart the static evaluation and the value the search
actually returned have been running for a given pawn structure, and shifts the
next static evaluation in that structure by the running average.

| | classical | NNUE |
|---|---|---|
| TC / bounds | 8+0.08, [0, 5] normalized | 8+0.08, [0, 5] normalized |
| **Result** | **H1** — LLR 2.96 at 2050 games | **H1** — LLR 2.95 at 3164 games |
| Elo | **+25.81 ± 10.34** | **+17.25 ± 8.33** |
| nElo | +37.69 ± 15.04 | +25.13 ± 12.11 |
| Record | 663W / 511L / 876D, 53.71% | 1028W / 871L / 1265D, 52.48% |
| Ptnml | [39, 219, 396, 293, 78] | [77, 333, 638, 424, 110] |
| LOS | 100.00% | 100.00% |
| PGN | `external/games/20260826-161029-STC.pgn` | `external/games/20260826-171852-STC.pgn` |

**It is worth less on the network, and that is the expected direction.** The
classical evaluation is a linear model fitted to a corpus; whole classes of
position are systematically mis-scored by it in a way no choice of weights can
fix, and that is exactly the residual correction history learns. A trained net
has already absorbed most of that structure, so there is less left over — +17.3
against +25.8, and 3164 games to resolve rather than 2050. Both intervals sit
clear of zero and neither result depends on the other; the change is worth
keeping under either evaluation.

**The bench moves in opposite directions on the two evaluations, and that is
the interesting part.** Classical goes up 4.1% (287826 → 299634) and NNUE goes
down 7.0% (229281 → 213141). Correction history does not prune or extend
anything by itself; it only changes what the static evaluation says, and every
margin in the search is measured against that. A correction that pushes an
evaluation further from beta costs nodes, one that pushes it past beta saves
them. The classical evaluation and the network are wrong in different
directions often enough for the net effect to flip sign between them.

**What is corrected, and what is not.** The transposition table stores the raw
evaluation and the search reasons with the corrected one. Storing the corrected
value would bake a stale adjustment into every later probe of that entry, and
the whole point is that the correction is re-derived from whatever the table
has learned since. Nothing the search *reports* is corrected either — the value
is clamped out of mate range, because a correction is evidence about an
evaluation and must never be able to manufacture a mate nothing proved.

**Update rule, and the three exclusions that matter more than the arithmetic.**
The entry is an exponential moving average weighted by depth. A node teaches it
nothing when the side to move is in check (there is no static evaluation to be
wrong about), when the score is a mate (a different kind of fact, not an
evaluation error), or when the best move was tactical (the gap was material
quiescence found, not a standing bias — crediting it would teach the table that
every structure which once contained a hanging piece is worth a pawn more than
it is). A bound also only counts in the direction it bounds: a fail high proves
the truth is at least `best`, which says nothing if the evaluation was already
above that.

**Cost.** A pawn key had to be maintained to key the table, and it lives in
`board_put_piece` / `board_remove_piece` / `board_move_piece` rather than in
`do_move`, because it is a function of where the pawns are and undo therefore
restores it for free. The measured nps difference is ~1% and does not cleanly
exceed run-to-run noise at this bench duration — the Elo above is a search
quality result, not a trade.

**Gates.** `perft` standard and tricky pass exactly under a debug build, which
asserts the incremental pawn key against `board_compute_pawn_key()` on every
make and unmake — 25.2M nodes of it. `datagen-test` still passes, because
`search_clear()` resets the new table and datagen clears before every label
search; had it not, every label would have depended on which position was
labelled before it.
---


### E15 — Staged move generation, step 1: no speedup, no Elo, reverted

**Date** 2026-08-26 · **Baseline** commit 981fc22, `base-corrhist` (bench
299634) · **Dev** the same with a staged move picker (bench 277624) ·
**Reverted — not in the tree**

The plan in README.md was: hand the search the transposition table move before
generating anything, as a **pure speedup gated on an unchanged bench node
count, not an SPRT**, worth part of the ~5-9 Elo attributed to staged movegen.
All three of those claims came out wrong, in an instructive order.

**1. It cannot be bench-exact, and the reason is not the one the plan
considered.** The plan argued from the ordering bands: `SCORE_TT` sits a whole
band above anything `score_moves()` can produce, so the table move was always
picked first and handing it back early reproduces the order. True, and
irrelevant. Staging also moves *when* the scoring happens. `score_moves()` used
to run before any child was searched; a staged picker runs it after the table
move's subtree has already updated `History`, `ContHist`, `CaptureHist` and
`CounterMoves`. The rest of the list is then scored against different tables
and reorders.

Attributed rather than asserted:

| build | bench |
|---|---|
| baseline | 299634 |
| staged picker | **277624** (−7.3%) |
| control: picker and table-move-first, but scored eagerly | **299634** (exact) |

The control keeps every part of the change except the deferral, and reproduces
the baseline to the node. So the whole difference is scoring time, not the set
of moves tried — and separately, a debug build asserts on every table move
handed back before generation that the generator would have produced it, which
passes the full bench. In check the picker deliberately generates first:
`movegen_is_pseudo_legal()` does not know about check, so a table move that
does not answer one validates but never appears in the `GEN_EVASIONS` list, and
handing it back early would search a move the engine previously skipped.

**2. There is no speedup.** Three alternating runs each, cores otherwise idle:

| | nps |
|---|---|
| baseline | 1193760 / 1189023 / 1161372 |
| staged picker | 1166487 / 1142485 / 1181378 |

Flat, marginally negative. In hindsight the ceiling was always small: only
nodes that *cut on the table move* skip generating at all, the plan's own
figures put that at 15% of generating nodes against ~25% of cycles spent
generating and ordering, so ~3.75% gross — and the picker's per-node state
machine spends part of that back. Bench wall time still fell ~6%, but from the
node reduction, not from the generation that was skipped.

**3. The node reduction did not convert into Elo.**

| | STC |
|---|---|
| TC / bounds | 8+0.08, [0, 5] normalized |
| **Result** | **stopped at 2958 games**, LLR −0.22 and drifting toward H0 |
| Elo | **+0.47 ± 8.52** |
| nElo | +0.69 ± 12.52 |
| Record | 856W / 852L / 1250D, 50.07% |
| Ptnml | [85, 349, 601, 365, 79] |
| LOS | 54.30% |
| PGN | `external/games/20260826-190703-STC.pgn` |

The test did not reject; it was stopped while undecided, because a true value
sitting between the bounds is the regime an SPRT resolves most slowly and the
interval was already informative. Fewer nodes at a fixed *depth* is what a
bench measures; a game is played at a fixed *clock*, and with nps flat there
was nothing to spend the saving on.

**What this means for steps 2 and 3.** They rest on the same premise — that
generation and ordering are ~25% of search cycles and staging reclaims a useful
share of it. Step 1 reclaimed the cheapest, most certain part of that share and
returned nothing measurable. That is evidence against the estimate itself, not
just against step 1, and the remaining steps are considerably larger changes.
Neither should be built on the ~5-9 Elo figure without first measuring what a
full staged picker actually saves in nps on a profile, rather than inferring it
from the share of cycles generation occupies.

**Kept from the attempt:** nothing in `src/`. The value is this entry and the
corrected plan in README.md.
---


### E16 — Correction history: three more keys, measured twice, reverted

**Date** 2026-08-26/27 · **Baseline** `corrhist-981fc22-nnue` (bench 213141),
commit 981fc22 · **Reverted — not in the tree**

E14 shipped one correction history table, keyed on pawn structure, and measured
it at +17.25 on the network. This added three more keys for the same mechanism
— minor pieces (knights, bishops and kings), non-pawn material per colour, and
the move that led to the node — on the argument that pawn structure is not the
only structure an evaluation is systematically wrong about.

Tested as one batch rather than four changes: the mechanism was already
measured in E14, and what was unproven was only whether additional keys carry
additional information.

**The weights were the experiment, and both settings failed.** Each table is
fitted to the same residual conditioned differently, so summing them at full
weight double-counts. Both configurations gave the pawn table unit weight —
unchanged from E14, so each was strictly additive to the proven term — and
differed only in how much the newcomers were believed.

| | A: quarter weight, 48cp cap | B: eighth weight, 32cp cap |
|---|---|---|
| Bench | 249039 (+16.8%) | 218965 (+2.7%) |
| Stopped at | 914 games | 1162 games |
| Elo | **−6.84 ± 15.42** | **−6.28 ± 12.96** |
| nElo | −10.01 ± 22.52 | −9.69 ± 19.98 |
| Record | 256W / 274L / 384D | 333W / 354L / 475D |
| Ptnml | [30, 109, 191, 103, 24] | [27, 145, 260, 120, 29] |
| LOS | 19.19% | 17.10% |
| LLR | −0.47 | −0.59 |
| PGN | `20260826-210225-STC.pgn` | `20260826-231315-STC.pgn` |

Both at STC 8+0.08, bounds [0, 5] normalized. Neither rejected; both were
stopped while undecided, on the same reasoning as E15 — a true value between
the bounds is what an SPRT resolves most slowly.

**Combined, with the caveat that they are different treatments**, the two
weightings give roughly **−6.5 ± 9.9 over 2076 games**. The interval still
spans zero. What it excludes is the +15 to +25 this was predicted to be worth.

**The hypothesis that justified configuration B was falsified, and that is the
part worth keeping.** Configuration A grew the bench 16.8% against a baseline
the same mechanism had previously *shrunk* by 7.0% on this evaluation. Since
correction history prunes and extends nothing directly — it only moves the
static evaluation, and E14 established that a correction pushing the evaluation
away from beta costs nodes — that looked like over-correction, and predicted
that a smaller correction would recover the Elo.

Configuration B removed 85% of the bench difference and **none** of the Elo
deficit: −6.28 against −6.84. A magnitude problem would have responded to a
magnitude fix. This one did not, so the extra keys are not mis-scaled — they
are not carrying information.

**Why that is the expected answer in hindsight.** A trained network has already
absorbed most of the structure these keys name; that is exactly why E14 was
worth less on the network than on `eval.c` (+17.3 against +25.8). Pawn
structure survives as a correction key because it is the residual the net still
mis-prices. Minor-piece and non-pawn-material configurations are precisely what
a 24576-row HalfKA input layer already encodes directly.

**Why reverted rather than left at zero weight.** The three tables could have
been kept and tuned to nothing, and the arithmetic would then match E14
exactly. But the keys are not free at zero weight: `board_put_piece`,
`board_remove_piece` and `board_move_piece` each carried two extra XORs to
maintain `minorKey` and `nonPawnKey`, in the hottest functions in make/unmake,
for tables nothing would read. The best available outcome from keeping them was
"baseline, minus a small nps cost", so there was nothing to win.

**Kept from the attempt:**

- `CORR_W_PAWN`, a TUNABLE for how far the surviving correction is believed. At
  the default of 128/128 it is arithmetically identical to what E14 shipped,
  which the unchanged bench proves; it exists because "how much to trust a
  learned evaluation bias" was chosen rather than fitted.
- The revert is verified by node count, not by inspection: nnue returns to
  **213141** and classical to **299634**, both exactly the committed E14
  baselines.

**Gates.** `perft` standard and tricky pass exactly under a debug build — 25.2M
nodes. `datagen-test` passes.
---

### E17 — SPSA tuning of 21 search parameters against the network

**Date** 2026-08-27/28 · **Baseline** the same TUNE_SEARCH build at its default
options · **Bench** 213141 -> 204156 nnue, 299634 -> 252945 classical

Task 4 of [NNUE.md](NNUE.md), and the largest single gain since the network
itself. Every threshold and formula constant in `search.c` was fitted against
the classical evaluation's scale and noise profile; the network has neither.
Twenty-one of them were exposed as UCI options under `TUNE_SEARCH=on` and fitted
jointly by SPSA (`tools/tune.py`, `make tune`).

| | STC 8+0.08 | LTC 40+0.4 |
|---|---|---|
| Bounds | [0, 5] normalized | [0.5, 4.5] normalized |
| **Result** | **H1 accepted** — LLR 2.95 at 830 games | positive, not run to a verdict |
| Elo | **+66.09 ± 16.36** | **+54.03 ± 29.30** |
| nElo | +97.89 ± 23.64 | +93.28 ± 49.66 |
| Record | 331W / 175L / 324D, 59.40% | 63W / 34L / 91D, 57.71% |
| Ptnml | [12, 57, 152, 151, 43] | [1, 14, 38, 37, 4] |
| LOS | 100.00% | 99.99% |

The LTC column is 188 games and LLR 0.51, so it is corroboration rather than a
second verdict. It is recorded because the specific risk this candidate carried
was fast-time-control overfitting - the values were derived at 2+0.02 - and
`RFP_MARGIN` moving *down* means pruning **more**, which is the classic change
that wins at blitz and regresses at depth. The point estimates agree inside
their intervals and no decay appeared.

**What moved.** Eighteen of twenty-one defaults changed:

| | from | to | | | from | to |
|---|---|---|---|---|---|---|
| `LMP_BASE` | 3 | **7** | | `LMR_BASE` | 10 | 12 |
| `DELTA_MARGIN` | 200 | **302** | | `SINGULAR_MARGIN` | 32 | 38 |
| `NMP_BASE` | 3 | 4 | | `ASPIRATION_DELTA` | 18 | 20 |
| `HIST_BONUS_MUL` | 4 | 5 | | `LMR_HIST_DIVISOR` | 8192 | 7828 |
| `FUTILITY_MARGIN` | 40 | 52 | | `LMR_CONT_DIVISOR` | 8192 | 7819 |
| `RFP_MARGIN` | 80 | **59** | | `NMP_EVAL_DIVISOR` | 200 | 190 |
| `NMP_DEPTH_DIVISOR` | 4 | 3 | | `SEE_CAPTURE_MARGIN` | 100 | 94 |
| `RAZOR_MARGIN` | 240 | 292 | | `SEE_QUIET_MARGIN` | 28 | 26 |
| `LMR_DIVISOR` | 24 | 23 | | `CORR_W_PAWN` | 128 | 132 |

`CAPHIST_DIVISOR`, `NMP_EVAL_MAX` and `HIST_BONUS_DEPTH_MAX` were unchanged.
**All twenty-one shipped as a set**, including the six whose drift was
statistically indistinguishable from noise: the SPRT measured the set as a
unit, and adopting a subset would ship a configuration nothing tested.

**The first run measured nothing, and why is the useful part.** 2300 iterations
at `r_end = 0.002` moved `RfpMargin` by 4.9 units out of a 230-unit range and
looked like "the defaults are already right". They were not. A drift test -
total displacement over the random-walk scale of the same increments - showed
fourteen of twenty-one parameters drifting **coherently**, several past 10
sigma, `DeltaMargin` at +15.7 and `RfpMargin` at -13.3. The gradient was
measured precisely and then not acted on, because `r_end = 0.002` is the value
the engine-tuning community uses for runs of 20000-40000 iterations and it was
carried onto a 2300-iteration run without rescaling. Travel scales as
`iterations x r_end`. At 0.02 the same 2300 iterations produced the result
above.

**The tuner was validated before it was trusted, with a sign-flip control.** At
a fixed depth, searching more nodes is free, so every pruning parameter should
walk toward pruning *less* - a direction known in advance and opposite to what a
clock rewards. Run at `--depth 6`, three parameters reversed exactly as
predicted: `RfpMargin` -4.9 at VSTC became **+33**, `NmpBase` +0.5 became -2,
`LmrBase` +0.2 became -3, with `LmpBase` pinned at its ceiling and `RazorMargin`
+58%. An implementation artefact cannot produce a sign flip that tracks the
objective function. Those fixed-depth values are meaningless for play and were
discarded; the control is what they were for.

**An intermediate set was measured on the way.** At iteration 188 of 2300 -
eight percent of the run, `RfpMargin` -8 and `DeltaMargin` +22 - the partial
values already scored **+12.99 ± 9.33** (LOS 99.68%, 2302 games) at STC. That
answered the open question of whether VSTC-derived values transfer, before the
full run had finished.

**It had not converged when it stopped.** `DeltaMargin` (z +13.2), `LmpBase`
(+13.2), `RazorMargin` (+7.2), `FutilityMargin` (+6.1), `LmrBase` (+5.4) and
`HistBonusMul` (+4.4) were all still climbing at iteration 2300, while
`RfpMargin` genuinely settled at 59. There is more here. `LMP_BASE` is the one
to watch: it tripled and was still rising against a ceiling of 10, so a
continuation run wants that range widened before it pins.

**Verification.** The 21 values were written into `search.c` programmatically
from the tuner's checkpoint, using the UCI-name-to-C-identifier mapping parsed
out of `search.c`'s own `Tunables[]` table rather than retyped. The gate was a
node count: the new default build benches **204156**, identical to the
TUNE_SEARCH build driven with the same 21 options by `setoption`. A single
mistyped digit moves the tree and would have shown immediately. `datagen-test`
passes.

**Cost.** About 15 hours of machine time for the tuning run, 64,400 games at
2+0.02, plus the SPRTs. No idea was required, only the harness - which is the
argument for building the harness.
---

### E18 — Re-labelling the human corpus: marginal, and the corpus is the ceiling

**Date** 2026-08-28 · **Not a completed SPRT.** Recorded because a negative
result that never gets written down is a result that gets re-derived.

The bootstrap loop [NNUE.md](NNUE.md) is built around says net *n+1* is trained
on searches that used net *n*. README listed the first turn of it - re-labelling
the human corpus with the network instead of `eval.c` - as "the single largest
item on this list". It was run and it did not pay.

| dataset | records | sources | labelling engine |
|---|---|---|---|
| `gen-001` | 175,876,025 | 100% human | 0.1.0-dev |
| `gen-002` | 78,548,872 | 100% self-play | 0.1.0-dev |
| `gen-012` | 254,424,897 | human + self-play | 0.1.0-dev |
| **`gen-003`** | **178,808,618** | **100% human** | **0.2.0-dev** |

`gen-003` is the re-label: the same corpus `gen-001` drew from, labelled by a
much stronger engine. The net trained on it - `1f36c07f4507`, tag
`epoch20-h1024` - is the net every measurement from E14 onward was made
against, and it is now the pinned net.

**It is not a regression.** Against the `gen-012` hybrid net it replaced
(`0e35d891b25a`, tag `epoch3-h1024`, the ~3050 net in `trainer/README.md`) the
point estimate favours it by 49 Elo. What it is not is a *step change*: the
gap does not separate from noise at the sample sizes run, and the README
predicted this would be "the single largest item on this list".

**Three things confound the reading, and all are worth recording.**

*Epoch selection, and it cuts in gen-003's favour.* The `gen-003` run's
validation loss bottomed at **epoch 4** (0.014153) and rose monotonically to
**epoch 20** (0.014521), +2.6% over 16 epochs while train loss kept falling -
textbook overfitting. The exported candidate was epoch 20, sixteen epochs past
its own minimum, and it *still* matched or beat a net exported at epoch 3.
**An epoch-4 export from `gen-003` has never been tried**, and on this evidence
it is the cheapest untested thing on the board - four epochs of GPU time.

*The ladder cannot separate them.* Measured against SF-3190 at 40+0.4:

| net | tag | score | implied | games |
|---|---|---|---|---|
| `1f36c07f4507` | epoch20-h1024 | 39.50% | 3116 ± 40 | 300 |
| `0e35d891b25a` | epoch3-h1024 | 33.04% | 3067 ± 97 | 56 |

A 49 Elo gap at **z = 0.91** - not significant. Two ladder runs differenced is
also the wrong instrument for comparing two nets: a head-to-head with the same
binary and `setoption name EvalFile` removes the Stockfish variance entirely
and resolves far faster. It was not run.

*The search parameters were fitted against the winner, and this one has no
clean fix.* All 21 tunables from E17 were tuned by SPSA with `1f36c07f4507`
loaded, so every comparison above runs the challenger on the incumbent's home
ground. That biases the +49 **in gen-003's favour** - it is an upper bound on
the net's own contribution, not a neutral estimate. The only honest way to
remove it is to re-tune against each net before comparing, which costs ~15
hours per net and is why the number stands as it is.

**What is worth taking from this.** Not "the re-label failed" and not "human
data is useless". The supported claim is narrower and more useful: **a much
better labeller on the same corpus bought at most a marginal gain**, where the
step from `gen-001` to the `gen-012` hybrid had been worth ~50 Elo by the
trainer's own notes. Label quality is no longer the binding constraint;
coverage is. A better label on a position the net already predicts well teaches
it nothing, and a human game corpus is fixed in what it covers however good the
labeller becomes.

**What that makes the next lever:** self-play with deliberate variation -
opening spread, randomised early plies, and the tree sampling `-tree` already
supports. None of those has been swept.

**A trap this exposed, unrelated to the result.** `external/nets/net.json`
describes whichever net was last *exported*; `make net-fetch` replaces
`net.nnue` without touching it, and `make nnue-test` re-exports from the local
checkpoint and replaces it back. The metadata beside a net is not evidence of
what the net is. `make nnue-info` and `NET_SHA256` are.
---


### E19 — Search batch: ProbCut, cut-node retry, ttPv, history split

**Date** 2026-08-29 · **Baseline** `5917ef9` (the tree E17/E18 measured) ·
**Bench** nnue 204156 -> 185533, classical 252945 -> 280881

**No SPRT has been run yet.** Four independent search changes, each written to
be tested on its own; this entry records what the bench and the correctness
probes said, and nothing about Elo. Every number below is a node count.

| # | Change | nnue d7 | nnue d12 | classical d12 |
|---|---|---|---|---|
| — | baseline | 204156 | 3469791 | 5155842 |
| 1 | + ProbCut | 203537 | 3448112 | — |
| 2 | + cut-node retry | 203537 | 3497603 | 4177747 |
| 3 | + ttPv | 206666 | 3212428 | 4409637 |
| 4 | + history split | 185533 | 3144581 | 4199385 |

The classical build's depth-7 count goes the other way, 252945 -> 280881
(+11%), and depth 12 says why that is not the number to read: -18.6% there.
Depth 7 is too shallow for any of these to pay - two of the four cannot fire at
all at that depth.

**1. ProbCut.** Captures only, `depth - 4` behind a quiescence pre-filter, at
non-PV nodes, skipped while verifying a singular move because it ignores
`excluded` and would otherwise prove a fail high with the very move being
excluded.

It needed a guard that the textbook version does not have. `see_ge(pos, m,
probCutBeta - staticEval)` has its threshold go NEGATIVE when the static
evaluation already sits above the raised beta, which admits every capture on
the board including the losing ones, each buying a quiescence search and often
a depth-4 search. Stockfish rarely meets that case because its reverse futility
pruning runs to depth 13 and has already returned; `RFP_DEPTH` here is 7, so
the depths between are exactly where ProbCut spent the most and proved the
least. Measured over depths 10-12 and averaged across margins, the unguarded
version **cost 2.4% of the tree** and the guarded one **saves 2.3%**.

The margin itself could not be chosen by bench. Node counts across 50/70/100/
150/190 came out non-monotonic - 50 saved 9% at depth 12 and cost 6% at depth
10 - which is tree shape, not signal. It ships at 100 with a sweep seat.

**2. Cut-node retry, and the version that had to be thrown away.** Written
first in the form E6's note describes, a bare `depth -= 2` at cut nodes with no
lower-bound table entry. It is wrong, and cheaply provable: on **WAC.001** the
engine stopped finding a mate in two at depth 16 that it finds at depth 10
without it. The reductions compound down a line and nothing re-searches. The
reduction amount does not control it either — 1 also lost the mate, 3 found it
again, which is a coin toss rather than a knob.

Replaced by the verification the same E6 note prescribes: run the reduced
search at the same ply with the same window, keep its answer only if it reaches
beta, and otherwise fall through to the full-depth search immediately below. A
line can now cost time here but cannot be lost here. The mate comes back, and
the classical depth-12 tree is 5.8% smaller than the unverified version's.

Two implementation notes worth keeping. `Stack[ply].cutRetry` is read-and-
cleared exactly like `excludedMove`, because the reduced search re-enters at
the same ply and must not start a retry of its own. And zero is special-cased:
without that test a reduction of zero would not disable the retry but DOUBLE
it, searching the node twice at the same depth.

**3. ttPv.** A byte of what was `TTEntry` padding, so the entry stays 16 bytes
and the generation cycle is untouched. `ttPv = pvNode || (ttHit && is_pv)`,
sticky within a slot, and it generalises the existing `if (pvNode) --r` in the
LMR curve to `if (ttPv) --r` rather than stacking a second rule beside it.

The first version seeded the flag from `pvNode` inside quiescence as well. That
looks equivalent and is not: a PV node hands quiescence a full window, the
window propagates through the whole capture tree below it, and every position
in it gets marked. It cost **18% of the classical bench tree**. Quiescence
preserves the flag and never sets it; marking a position is the main search's
decision.

**4. History split.** `history_malus()` on its own multiplier, so that "this
move caused a cutoff" and "this move was tried and did not" stop being sized by
one number - the first names one move, the second is levelled at up to
sixty-three at once.

This one is honest about being structural. Swept at depths 10-12 the tree is
flat between 5 and 6 (0.7% apart, inside the noise) and degrades sharply above:
7 costs 7%, 8 costs 14% at depth 12. It ships at 6, which is chosen not to move
the tree while the split gets measured. The value is for the tuner to fit, and
`HistMalusMul` now has a seat in `Tunables[]` for the continuation run.

**What was checked besides node counts.** `make perft` exact (14/14, 7/7);
both suites again under `make debug` with assertions live; a depth-16 search
from the start position and from Kiwipete under assertions; `nnue-test` exact
on 10,000 positions; `datagen-test`; `openbench-check`; `format-check`. Across
the 21 in-repo EPD positions at depth 14, baseline and candidate find the same
three mates.

**New sweep seats**: `ProbCutMargin` (30-300), `CutNodeRetryReduction` (0-3),
`HistMalusMul` (1-32). All three default to values chosen to be defensible
rather than fitted, which makes the tuner continuation run more valuable than
it was before this batch.

---

### E19a — the batch measured negative, and what the ablations did and did not say

**Date** 2026-08-29 · All at STC 8+0.08, bounds [0, 5] normalized.

The four-feature batch, against `prev-5917ef9-nnue`:

| | |
|---|---|
| **Result** | **negative**, stopped at LLR -1.19 |
| Elo | **-11.74 ± 11.58** (nElo -18.72 ± 18.45) |
| Record | 353W / 399L / 610D over 1362 games, 48.31% |
| Ptnml | [28, 183, 302, 143, 25], LOS 2.34% |

Then four ablations, each `dev = one feature OFF` against a baseline with all
four ON, so a **negative** number means removing the feature LOSES Elo and the
feature is earning its place. All were stopped early.

| ablation | Elo | 95% interval | games | reads as |
|---|---|---|---|---|
| cut-node retry, first version | **+11.50 ± 16.28** | [-4.8, +27.8] | 816 | costing us |
| cut-node retry, after the re-probe | **+3.64 ± 16.35** | [-12.7, +20.0] | 764 | ~neutral |
| ttPv | -13.09 ± 21.98 | [-35.1, +8.9] | 478 | earning it |
| ProbCut | -4.44 ± 19.31 | [-23.8, +14.9] | 548 | earning it |
| history split | -18.65 ± 22.65 | [-41.3, +4.0] | 522 | earning it |

**Every one of those intervals spans zero, and the set is internally
inconsistent.** The three "earning it" point estimates imply the features are
together worth **+36 Elo**, while the batch that contains them measured
**-11.74** against the build without them - a 48-point contradiction. Both
cannot be true, and the batch has the most games and the tightest interval. The
honest reading is that the ablation point estimates are dominated by noise, and
that selecting a subset on them would ship a configuration nothing tested. E2-E5
reached the same wall for the same reason: resolving a ±5 Elo question needs
several times 2000 games.

**One conclusion does survive**, because three independent lines agree on it:
the cut-node retry does not earn its place. Its two ablations sit at +11.50 and
+3.64 - the only two of the five whose sign says "remove me". It is the only
one of the four that shrinks the tree by nothing measurable (0.25% at depth 12,
against 8.5% for ProbCut, 5.3% for ttPv, 4.3% for the history split). And E6
had already measured its per-move cousin at -16 Elo on this same search.

**So it is removed**, and that is E6's verdict confirmed a second time in a
second form. The re-probe did fix something real - a failed retry was
discarding the move its own search had just found, and the ablation moved from
+11.50 to +3.64 after it - but fixing a feature into neutrality is not a reason
to carry it.

The removal was verified the way E17 verified its parameter rewrite: by node
count. The build with the code deleted benches **3134217** at depth 12,
identical to the ablation binary driven with `CutNodeRetryReduction=0`, so what
ships is exactly the configuration that was measured.

**What remains**: ProbCut, ttPv and the history split, bench nnue 204156 ->
185533 (d7) and 3469791 -> 3134217 (d12), classical 5155842 -> 4199385 (d12).
**None of the three is individually certified** and the batch containing them
has never been measured in this form - the -11.74 above was the build with the
broken retry in it. That single SPRT against `prev-5917ef9-nnue` is the number
that decides whether any of this ships.

**Ablation switches kept**, because the next round of this will want them:
`ProbCutDepth` (5-99; set it above the search depth and ProbCut is off) and
`TtPvReduction` (0-3; zero restores the pre-E19 reduction rule exactly, with
the table flag still written and nothing reading it). `ProbCutDepth` is an
ablation switch and NOT a sweep seat - it is a depth threshold, and the note
beside `ASPIRATION_MIN_DEPTH` says why those measure as noise under SPSA. Pass
it to `make tune ARGS="--exclude ..."`.

---


### E19b — the three that survived, against the build before them

**Date** 2026-08-29 · **Baseline** `prev-5917ef9-nnue` · **Bench** nnue 204156
-> 185533 (d7), 3469791 -> 3134217 (d12)

ProbCut, ttPv and the history split, with the cut-node retry removed. Same
baseline, time control and book as the E19a batch measurement, which is what
makes the two directly comparable.

| | |
|---|---|
| TC / bounds | STC 8+0.08, [0, 5] normalized |
| **Result** | **positive, NOT a verdict** — LLR 0.97 of 2.94 (33%) at 2596 games |
| Elo | **+7.50 ± 8.71** (nElo +11.51 ± 13.37) |
| Record | 758W / 702L / 1136D, 51.08% |
| Ptnml | [51, 307, 539, 337, 64], PairsRatio 1.12 |
| LOS | 95.42% |

**Read the interval, not the point estimate.** [-1.2, +16.2] still contains
zero. LOS 95.42% is suggestive and it is not the 97.5% a two-sided interval
would need, let alone the LLR the test is actually waiting on. Nothing here is
certified yet.

**What it does establish is the removal.** The same three features, measured
the same way with the cut-node retry still in them, scored **-11.74**. Taking
one feature out moved the batch by **+19.2 Elo**, against a first ablation that
had put the retry at +11.50 on its own. Two independent measurements of the
same quantity, agreeing inside their intervals, on the one conclusion this
round supports.

It also retires the reading in E19a that the ablation point estimates implied
+36 Elo for these three. They are worth about +7.5, and the +36 was noise, as
that entry said it probably was.

**Still open**: this needs to run to a verdict, and then LTC confirmation before
anything is claimed. Two of the three defaults - `ProbCutMargin` at 100 and
`HistMalusMul` at 6 - were chosen to be defensible rather than fitted, so a
tuner continuation is the obvious next lever on the same code.

---

### E19c — why E17 could not move seven of its parameters

**Date** 2026-08-29 · **Not a game result.** A defect in `tools/tune.py`, found
by working out what its own schedule does rather than by playing anything.

E17 reported `CapHistDivisor`, `NmpEvalMax` and `HistBonusDepthMax` as
"unchanged" after 2300 iterations. That was read at the time as those values
already being right. It was not.

Travel over a run is about `r_end * c_end * iterations * E[result]`, and
`c_end` defaults to a twentieth of the declared range. Every parameter
therefore gets the same *relative* travel, while every integer UCI option needs
the same *absolute* resolution, which is one. Worked through at E17's settings:

| c_end | example | travel per run | can it change the shipped integer? |
|---|---|---|---|
| 1.0 | `LmpBase`, `NmpBase`, `NmpEvalMax` | **0.46** | no - it needs 0.5 |
| 1.6 | `CapHistDivisor`, `HistMalusMul` | 0.71 | barely |
| 27.5 | `DeltaMargin` | 12.65 | yes (moved +102) |
| 1536 | `LmrHistDivisor` | 706 | yes (moved -364) |

Nine of twenty-five parameters sat below the threshold. The three E17 called
unchanged are three of them. `LmpBase` is in the same group and moved 3 -> 7,
which is the model working rather than against it: travel scales with signal,
and that one was badly enough wrong to clear the bar anyway.

**Two fixes, both in `tools/tune.py`.**

*The step is now sized separately from the perturbation.* `c_end` goes on
setting how far the two test engines differ; a new `step_c = max(c_end, 5.0)`
sets how far the value moves. Travel for the nine goes 0.46 -> 2.30 and **no
parameter's step got smaller** - every wide one is arithmetically identical, so
E17's fit is not disturbed.

*The gradient divides by the perturbation the engines actually saw.* The update
divided by the float `ck`, but what reached the engines went through
`int(round(clamp(...)))` on both sides, and near a bound the clamp can halve the
separation or remove it. Dividing by `ck` there understates the gradient exactly
where it is weakest. It now divides by `(plus - minus) / 2`, and skips a
parameter whose two sides collapsed onto the same integer instead of dividing
into noise.

**And one parameter that was never tunable at all.** `HistBonusDepthMax` is
`min(depth, cap)` over *remaining* depth, so a cap of 20 cannot bind until the
search passes depth 20 - and at STC this engine reaches 12-13. It was inert,
not weakly measurable, and E17's "unchanged" was a zero gradient rather than a
noisy one. Its range now reaches 32 so an **LTC** sweep can move it. At STC it
should be excluded from the fit; widening a range a parameter cannot influence
only buys somewhere to random-walk, which is what the README warned about and
was right to.

A third fix was identified and deliberately not applied: stochastic rounding of
the perturbed values, which would make the expected engine value continuous in
the tuned value rather than a staircase between integer crossings. It changes
every run's trajectory, so it wants to land on its own.

---

### E20 — Uncertainty-scaled pruning margins: the corrhist-magnitude probe

**Date** 2026-08-30 · **Baseline** `stormbreaker-nnue-best-gen-4` ·
**Bench** nnue 226961 -> 238814 (d7), 3286704 -> 3194362 (d12)

Every margin-based prune insures against the eval-vs-search residual with a
globally constant width; the residual is heteroscedastic (measured |corrhist|:
median 0, mean ~6cp over the d12 tree). `unc_scale()` scales the five margins
(RFP, razoring, ProbCut, futility, qsearch delta) by
`min(89 + 2 * |correction|cp, 140)` percent, centred so the node-weighted
average margin is unchanged and the SPRT measures the conditioning alone. See
docs/NNUE.md Task 5b for the idea's full arc; this is its step 1, the probe
that cost no trainer work.

| | |
|---|---|
| TC / bounds | STC 8+0.08, [0, 5] normalized |
| **Result** | **H1 accepted** — LLR 2.97 of 2.94 at 1862 games |
| Elo | **+25.61 ± 9.85** (nElo +41.20 ± 15.78) |
| Record | 584W / 447L / 831D, 53.68% |
| Ptnml | [20, 183, 425, 246, 57], PairsRatio 1.49 |
| LOS | 100.00% |

Both binaries were verified mid-run to embed the same pinned gen-4 net
(`54f9285f0585`), with depth-1 evals identical position-by-position — the
measured difference is the search change alone. The point estimate came down
from +38 at 558 games to +25.6 at the verdict, which is the usual drift and
why only the verdict is quoted.

Two honest caveats. The centering distribution was measured on the *previous*
net (`cacfebf399cb`) — the net was re-pinned to gen-4 mid-development and the
constants were never re-centred, so `UncScaleBase/Slope/Max` go to the sweep
as unfitted seats. And this is STC only; LTC confirmation is pending, like
E19b's.

What this licenses is Task 5b step 2: the trained σ head. Its SPRT must run
against **this** build — the head has to beat the probe it replaces, not the
engine without scaling.

---

### E21 — The trained uncertainty head replaces the corrhist probe

**Date** 2026-08-30 · **Baseline** the E20 build (`stormbreaker-nnue-best-gen-4`,
net `54f9285f0585`) · **Bench** nnue 238814 -> 218976 (d7),
3194362 -> 2989539 (d12)

Dev is one batch with two parts. The net is a retrain of the baseline's own
recipe (gen-3 + gen-4 data, 512x2, 4 epochs) with `--uncertainty`: a second
output head on the shared trunk, trained by L1 against the detached residual
`|search score − value|`, exported behind the header's `reserved[0]` flag as
net `0ba56166ba9c`. The search is unchanged except that `unc_scale()` now
derives its margin factor from the head's predicted error instead of the
corrhist magnitude: `min(72 + sigma/2, 140)` percent, centred like E20's
constants on the measured d12 distribution (sigma median 47cp, node-weighted
mean ~72cp, taken with scaling held neutral so the mapping could not shape the
tree it was measured on).

| | |
|---|---|
| TC / bounds | STC 8+0.08, [0, 5] normalized |
| **Result** | **H1 accepted** — LLR 2.98 at 2516 games |
| Elo | **+21.29 ± 9.22** (nElo +31.44 ± 13.58) |
| Record | 844W / 690L / 982D, 53.06% |
| Ptnml | [58, 246, 526, 340, 88], PairsRatio 1.41 |
| LOS | 100.00% |

With E20, that is two verdicts and ~+47 at STC over the pre-probe best, from
one idea taken in two steps. A sanity run of the same dev against the older
pre-E20 baseline trended consistently (+27.7 ± 18.8 at 528 games) and was
superseded by this test rather than run to a verdict.

**What this does and does not attribute.** The batch measures the retrain and
the signal swap together. The value trunk was retrained with the head's
gradients flowing into it, and two same-recipe training runs differ by
run-to-run variance on their own, so "the head's signal beats corrhist's" is
NOT isolated here — that A/B (same net, same binary, only `unc_scale()`'s
input differing) remains unrun and is a one-line experiment if attribution
ever matters. What IS established: the corrhist-magnitude signal was replaced
by a position-only learned prior and the engine got stronger, against the
prediction (argued from Stockfish's corrplexity being path-dependent, see the
prior-art note under E20) that the value lives in the running measurement. As
far as known, this is the first measured instance of a learned uncertainty
head paying its way in an alpha-beta engine.

**Open debts.** STC only — with E20 that is two entries of LTC confirmation
debt. The five `Unc*` tunables are sweep seats fitted by centring, not by
SPSA. The corrhist and sigma signals are not exclusive, and combining them is
an untried one-SPRT experiment.

---

### E22 — SPSA continuation: 28 parameters re-fitted, the uncertainty pair included

**Date** 2026-08-31 · **Baseline** the same `TUNE_SEARCH` build at its default
options (the E21 build, net `0ba56166ba9c`) · **Bench** nnue 218976 -> 204540
(d7), 2989539 -> 3537141 (d12); classical 300904 -> 275123

A second SPSA pass over `search.c`'s tunables, following E17. Two things had
changed since that fit: E19b's search batch altered what the parameters act on,
and E20/E21 added five `Unc*` seats that had never been swept — E21 listed them
as an open debt in as many words, "sweep seats fitted by centring, not by
SPSA". This run is the first gradient ever taken on them.

Twenty-eight of the thirty seats in `Tunables[]` were fitted jointly at STC
8+0.08. `ProbCutDepth` and `HistBonusDepthMax` were excluded: both are depth
caps that cannot bind at the depths STC reaches, so a sweep there random-walks
them inside a flat region and reports the walk (the reasoning is spelled out
beside `HIST_BONUS_DEPTH_MAX`).

| | STC 8+0.08 |
|---|---|
| Bounds | [0, 5] normalized |
| **Result** | **H1 accepted** — LLR 2.96 at 3466 games |
| Elo | **+14.24 ± 7.10** (nElo +23.24 ± 11.57) |
| Record | 1006W / 864L / 1596D, 52.05% |
| Ptnml | [48, 373, 769, 475, 68], PairsRatio 1.29, DrawRatio 44.37% |
| LOS | 100.00% |

**The test isolates the options and nothing else.** Both sides were the same
`stormbreaker-tune-nnue.exe`, the same net, the same binary on disk; dev was
that binary with 28 `option.*` values on its command line and base was that
binary with none. No build difference, no net difference, no compile flags to
confound it — the only thing that differed between the two players was the 28
integers.

**What moved.** Nineteen of the twenty-eight defaults changed after rounding:

| | from | to | | | from | to |
|---|---|---|---|---|---|---|
| `LMR_CONT_DIVISOR` | 7162 | **6845** | | `PROBCUT_MARGIN` | 103 | 108 |
| `DELTA_MARGIN` | 361 | **374** | | `RAZOR_MARGIN` | 305 | 309 |
| `UNC_SIGMA_SLOPE` | 8 | **12** | | `UNC_SCALE_MAX` | 140 | 143 |
| `LMR_HIST_DIVISOR` | 7622 | 7714 | | `CORR_W_PAWN` | 129 | 132 |
| `SEE_QUIET_MARGIN` | 20 | **16** | | `RFP_MARGIN` | 64 | 67 |
| `SEE_CAPTURE_MARGIN` | 90 | 86 | | `SINGULAR_MARGIN` | 41 | 39 |
| `HIST_MALUS_MUL` | 7 | 9 | | `NMP_EVAL_DIVISOR` | 189 | 187 |
| `NMP_DEPTH_DIVISOR` | 3 | 4 | | `LMR_DIVISOR` | 23 | 22 |
| `NMP_EVAL_MAX` | 2 | 3 | | `FUTILITY_MARGIN` | 57 | 58 |
| `UNC_SCALE_SLOPE` | 2 | 1 | | | | |

Nine rounded back to their starting values and are unchanged in the source:
`UNC_SCALE_BASE`, `UNC_SIGMA_BASE`, `LMR_BASE`, `CAPHIST_DIVISOR`, `NMP_BASE`,
`HIST_BONUS_MUL`, `LMP_BASE`, `TTPV_REDUCTION`, `ASPIRATION_DELTA`. As in E17
**all twenty-eight shipped as a set**, the low-confidence movers included: the
SPRT measured the set as a unit, and adopting a subset would ship a
configuration nothing tested.

**The uncertainty head's slope is the largest coherent mover in the run.**
Drift z — total displacement over the random-walk scale of the same increments,
the same statistic E17 used — over 876 iterations:

| param | z | | param | z |
|---|---|---|---|---|
| `UncSigmaSlope` | **+5.6** | | `HistMalusMul` | +2.7 |
| `SeeQuietMargin` | **−5.1** | | `ProbCutMargin` | +2.6 |
| `UncScaleMax` | **+3.7** | | `SeeCaptureMargin` | −2.1 |
| `DeltaMargin` | **+3.1** | | `NmpDepthDivisor` | +2.1 |

Everything else is under 2. Two of the four strongest are the pair E21 flagged,
and they moved the way E21's own comment predicted they would: that comment
observed the sigma distribution is right-skewed and `UNC_SCALE_MAX` truncates
its tail, so "a sweep that moves the cap moves the average margin with it". The
sweep raised the cap and the slope together — margins scaled harder off
predicted error, with more headroom to scale into. Centring got the mapping to
the right neighbourhood; it did not get the slope right, and it was 50% low.

`SeeQuietMargin` falling to 16 is the one clear tightening in an otherwise
loosening set, and it is the parameter with the second-strongest gradient.

**Shallow and deep trees moved in opposite directions.** d7 nodes fell 7% while
d12 nodes rose 18%. Most margins went up, which prunes less, so the d12
direction is the expected one and the d7 number is the surprise — the LMR
changes dominate where depth is short and the margins dominate where it is not.
Recorded mainly as another instance of the habit from E1's ablations: a
single-depth bench diff would have read this change as "searches less" and been
wrong about the tree that actually plays the games.

**This is an intermediate checkpoint, not a converged run.** The tuner was
stopped at **iteration 876 of 2500** (24,528 games, 6712W/6626L/11190D, seed
652340824, 28 games/iteration, concurrency 14) and the checkpoint was tested as
it stood — the same move E17 made at its iteration 188. The state file is
intact and `python tools/tune.py --resume` continues it.

If it is resumed, **the next checkpoint must be SPRT'd against these new
defaults, not against the pre-E22 ones.** The tuner's own `start` values are
now stale relative to the shipped source, and testing a later checkpoint
against the old baseline would re-measure this gain and count it twice.

**E22a below is that continuation**, run narrow rather than resumed: the drift
table is what it was chosen from.

**Verification.** The 28 values were written into `search.c` programmatically
from the tuner's checkpoint, using the UCI-name-to-C-identifier mapping parsed
out of `search.c`'s own `Tunables[]` table rather than retyped — the E17
procedure. The gate was the node count, checked at two depths: the new default
build benches **204540** at d7 and **3537141** at d12, both identical to the
`TUNE_SEARCH` build driven with the same 28 options by `setoption`. A single
mistyped digit moves the tree and would have shown immediately. `perft`,
`datagen-test` and `openbench-check` pass.

**Caveats.** STC only — the LTC confirmation debt from E20 and E21 now stands
at three entries. The values were fitted against the network and shipped as the
defaults for **both** evaluations, so the classical build's search changed
(bench 300904 -> 275123) at values no game was played on; that is E17's
precedent rather than a new decision, and it matters less while `classical` is
not what plays. +14.24 against E17's +66.09 is what a second pass over
already-fitted parameters should look like — the large errors were taken out
the first time, and what remains here is mostly the five seats that had never
been fitted at all.

---

### E22a — the same run narrowed to the nine seats that still had a gradient

**Date** 2026-09-01 · **Baseline** the same `TUNE_SEARCH` build at its E22
defaults (`0a0e072`, net `0ba56166ba9c`) · **Bench** nnue 204540 -> 203047
(d7), 3537141 -> 2963084 (d12); classical 275123 -> 277139

E22 swept 28 seats and found eight of them moving coherently. Continuing that
run spends 20/28 of the games measuring parameters it had already shown to be
flat, so this is a narrower run instead: the six E22 measured at |z| >= 2, plus
the uncertainty seats that bind under this net. Nine parameters, same TC, same
book, fresh state in `external\tune\spsa-unc.json`.

**Two of the five `Unc*` seats cannot be tuned against this net, and were left
out on that ground.** `unc_scale()` branches on `nnue_has_uncertainty()`: a net
carrying the head returns `UNC_SIGMA_BASE + sigma * UNC_SIGMA_SLOPE / 16`
capped at `UNC_SCALE_MAX` and returns before ever reading `UNC_SCALE_BASE` or
`UNC_SCALE_SLOPE`, which serve the corrhist fallback below it. Net
`0ba56166ba9c` carries the head, so in a tuning game that pair is unreachable
and a sweep of it measures its own random walk. E22 swept them anyway — that is
what its sub-noise z scores for the pair were saying — and shipped
`UNC_SCALE_SLOPE` 2 -> 1 out of a walk rather than a fit. The value only binds
in a classical build or under a headless net, so nothing measured here is
affected, but the constants are centred values with a walk on top and should be
read that way. The cap is shared by both branches and stayed in the sweep.

| | STC 8+0.08 |
|---|---|
| Bounds | [0, 5] normalized |
| **Result** | **H1 accepted** — LLR 2.97 at 3914 games |
| Elo | **+12.88 ± 6.71** (nElo +20.90 ± 10.88) |
| Record | 1123W / 978L / 1813D, 51.85% |
| Ptnml | [58, 428, 855, 543, 73], PairsRatio 1.27, DrawRatio 43.69% |
| LOS | 99.99% |

The isolation is E22's: both sides were `stormbreaker-tune-nnue.exe`, the same
net, the same binary on disk, dev carrying nine `option.*` values on its command
line and base carrying none.

**What moved.** Seven of the nine defaults changed after rounding:

| | from | to | | | from | to |
|---|---|---|---|---|---|---|
| `SEE_QUIET_MARGIN` | 16 | **12** | | `UNC_SIGMA_BASE` | 72 | 73 |
| `DELTA_MARGIN` | 374 | **389** | | `UNC_SCALE_MAX` | 143 | 144 |
| `NMP_DEPTH_DIVISOR` | 4 | 5 | | `PROBCUT_MARGIN` | 108 | 109 |
| `UNC_SIGMA_SLOPE` | 12 | 13 | | | | |

`SEE_CAPTURE_MARGIN` (86) and `HIST_MALUS_MUL` (9) rounded back to where they
started. All nine shipped as a set, for E17's reason: the SPRT measured the set.

**Two parameters carry the run.** Drift z — total displacement over the
random-walk scale of the same increments — over 781 iterations:

| param | z | | param | z |
|---|---|---|---|---|
| `SeeQuietMargin` | **−5.6** | | `NmpDepthDivisor` | +1.2 |
| `DeltaMargin` | **+4.2** | | `ProbCutMargin` | +0.8 |
| `UncSigmaSlope` | +2.0 | | `HistMalusMul` | +0.3 |
| `UncSigmaBase` | +1.4 | | `SeeCaptureMargin` | −0.2 |
| `UncScaleMax` | +1.4 | | | |

**The uncertainty mapping has converged and the SEE/delta pair has not.**
`UncSigmaSlope` entered this run as E22's strongest mover at z +5.6 and leaves
at +2.0; `UncScaleMax` entered at +3.7 and leaves at +1.4. Both moved one unit
and stopped. That is the shape of a parameter that was mis-centred once and has
since been found: E22 took out the 50% error in the slope, and a second pass
over the same seats finds nothing left worth games. `SeeQuietMargin` and
`DeltaMargin` were E22's second and fourth strongest and are this run's first
and second, moving the same direction on both passes — down and up. After two
sweeps they are the only seats in `search.c` still visibly climbing.

**The d12 tree lost 16% of its nodes while d7 lost 0.7%.** Three of the seven
changes loosen — `DELTA_MARGIN` up prunes fewer captures in qsearch,
`NMP_DEPTH_DIVISOR` up shrinks the null-move reduction, `PROBCUT_MARGIN` up
cuts less — and one tightens. The one that tightens wins by a wide margin,
because `SEE_QUIET_MARGIN` gates quiet moves against `-margin * depth * depth`,
a threshold that is shallowest near the leaves and therefore applies to almost
every node in a deep tree. Dropping 16 -> 12 raises that floor everywhere it
bites. E22 recorded the mirror image of this — d7 down 7%, d12 up 18% — and the
lesson is the same one twice: a single-depth bench diff describes a tree that
is not the one playing the games.

**This is again an intermediate checkpoint.** Stopped at **iteration 781 of
1500** (21,868 games, 5972W/5857L/10039D, seed 1866954482, 28 games/iteration,
concurrency 14) and tested as it stood. `python tools/tune.py --resume --state
external/tune/spsa-unc.json` continues it, and as in E22 the next checkpoint
must be SPRT'd against **these** defaults rather than the ones before them.

**Verification.** The nine values were written into `search.c` programmatically
from the tuner's checkpoint, using the UCI-name-to-C-identifier mapping parsed
out of `search.c`'s own `Tunables[]` table — the E17 procedure, for the third
time. The gate was the node count at two depths: the new default build benches
**203047** at d7 and **2963084** at d12, both identical to the `TUNE_SEARCH`
build driven with the same nine options by `setoption`. `perft`,
`datagen-test` and `openbench-check` pass.

**Caveats.** STC only, so the LTC confirmation debt stands at four entries
(E20, E21, E22, E22a). The values were fitted against the network and shipped as
the defaults for both evaluations: the two `UNC_SIGMA_*` seats are compiled out
of a classical build, but `UNC_SCALE_MAX`, `SEE_QUIET_MARGIN`, `DELTA_MARGIN`,
`PROBCUT_MARGIN` and `NMP_DEPTH_DIVISOR` all bind there at values no game was
played on (classical bench 275123 -> 277139). +12.88 after E22's +14.24 and
E17's +66.09 is a third pass behaving like a third pass, and the narrowing is
the point: 3914 games bought most of E22's gain from a third of the seats.

---

### E23 — Syzygy tablebases, and the removal of every adjudication rule

**Date** 2026-09-01 · **Baseline** the E22 build · **Bench** classical 277139
-> **277139**, nnue 203047 -> **203047** (both unchanged, by construction)

Both baselines were re-measured from a clean build of `HEAD` rather than taken
from E22's entry above, which records 275123 and 204540. Those are the numbers
from before `0a0e072` ("Update tunnables"), which moved search parameters again
without restating the bench — worth knowing before comparing anything to E22's
line.

Preparation for gen-5 data, not an Elo patch. Two observations motivated it:
the engine scores KNP-vs-KP as roughly +3 when it is dead drawn, and it draws
far more against 3000-3200 opponents than engines of comparable strength do.

**What changed.**

1. **Syzygy probing in the engine** — Fathom vendored into `src/fathom/`
   (MIT, see CREDITS.md), adapted by `src/syzygy.c`. WDL at interior nodes
   where `halfmoveClock == 0`, DTZ at the root for both the move and the
   score. `SyzygyPath` over UCI, `-syzygy` for datagen; **off by default**.
2. **Both adjudication rules deleted** from `datagen selfplay`. Games end by
   mate, stalemate, the fifty-move rule, repetition or insufficient material.
3. **Ply-capped games are labelled WDL 3 (unknown)**, not draw.
4. **`-maxscore` now defaults to 0**, so conversion positions are kept.

**Why the root probe carries the score, not just the move.** Interior nodes
can only probe at a zero halfmove clock, which is what WDL tables assume. The
children of a five-man root have clock 1 after any piece move, so they are
searched heuristically and the root's score comes back from the evaluation —
the labelling bug would have survived the integration intact. DTZ is
fifty-move-aware at any clock, so one root probe settles both. Verified: the
KNP-vs-KP position now labels 0.

**Why bench is unchanged.** Probing is gated on tablebases being loaded and
`make bench` never loads them, so the benchmark is identical on a machine with
tables and one without. This is invariant 1, not a convenience: a node count
that depended on which files a machine happened to have would make every
cross-machine comparison meaningless.

**Gates.** `bench` byte-identical in both evaluations · `perft` 14/14 ·
`openbench-check` · `format-check` · `datagen-test`, extended with an
assertion that ply-capped games carry WDL 3 · new `make syzygy-test`, which
probes known endgames — the drawn KNP-vs-KP among them — each as given and
mirrored, and fails if a probe silently never fires.

**No SPRT, and none needed for the default build**: with `SyzygyPath` unset
the engine is bit-identical, which bench proves. Shipping tablebases on by
default in match play would be a separate change and would need its own test.

**Caveat.** The draw-rate observation against 3000-3200 engines is *not*
established as a data problem. It could as easily be contempt, rule50 scaling
of the evaluation, or pruning that is too aggressive to find a long grind.
Classifying the drawn games by how they drew — repetition from a better
position, fifty-move, dead-drawn material reached from a win — is the cheap
diagnostic and has not been run. This entry fixes the label truth; whether
that alone moves the draw rate is unmeasured.

---


## Absolute strength

Every Elo figure above is relative to another build in `external\baselines`,
none of which is itself rated. This anchors them: `make gauntlet` plays the
third-party engines `make engines-fetch` puts in `external\engines`, each
carrying a published CCRL rating, and the tables below fit one rating by
inverse-variance weighting over those rungs.

Entries below dated before 2026-08-30 use a second ladder — Stockfish `UCI_Elo`
rungs, run by `tools/rating.py`. Both the tool and the ladder are gone. They
were never the CCRL pool, and a strength-limited Stockfish is not a genuinely
weaker engine, so those numbers do not difference against the CCRL tables. The
engine is now strong enough to play Stockfish unlimited instead — `make gauntlet
ARGS="--field stockfish"` — which is an honest opponent rather than a rung.

**2026-08-30**, after E19b, at STC 8+0.08, on the **network** build, against
the CCRL ladder. 638 games round-robin, ~44 against each rung:

| CCRL rung | W-L-D | score | implied rating |
|---|---|---|---|
| halogen-8.1, 3008 | 22-12-9 | 61.6% | 3090 ± 107 |
| berserk-4.1.0, 3133 | 19-13-12 | 56.8% | 3181 ± 104 |
| weiss-1.4, 3256 | 7-29-7 | 24.4% | 3060 ± 121 |
| clover-3.0, 3340 | 6-33-5 | 19.3% | 3092 ± 130 |
| ethereal-12.75, 3426 | 0-40-4 | 4.5% | 2897 ± 246 |

**Combined: 3100 ± 55**, on the CCRL Blitz scale.

**What this measurement is for, and it is not "a second opinion on 3096".**
The `UCI_Elo` ladder's largest caveat is that a strength-limited Stockfish is
not a weaker engine - it plays near-full-strength moves with occasional
deliberate errors, and that error distribution is not the one a genuinely
weaker opponent produces. This ladder has no such problem: all five are real
engines at full strength, so the objection does not apply at all. That is the
reason to run it, not the agreement with 3096.

**2026-08-30**, after E21, at STC 8+0.08, 16MB hash, on the shipping build
(net `0ba56166ba9c`), against the same CCRL ladder. Round-robin, 1000 games
per engine, 200 per pairing:

| CCRL rung | W-L-D | score | implied rating |
|---|---|---|---|
| halogen-8.1, 3008 | 172-9-19 | 90.8% | 3406 |
| berserk-4.1.0, 3133 | 146-24-30 | 80.5% | 3379 |
| weiss-1.4, 3256 | 111-46-43 | 66.2% | 3373 |
| clover-3.0, 3340 | 111-43-46 | 67.0% | 3463 |
| ethereal-12.75, 3426 | 74-68-58 | 51.5% | 3436 |

**Combined: ~3410 ± 40 (statistical)** on the CCRL Blitz scale; the rung
offsets agree within ±25 of their mean and the internal spread matches the
CCRL spread to within ~10%, so the event is internally coherent. The single
most defensible datapoint is the direct match with the strongest anchor:
even with Ethereal 12.75.

**Two reasons this is claimed as ~3300, not 3410.** First, the delta from the
entry above - 3100 ± 55 to ~3410 on the same ladder in the same conditions -
exceeds what the measured relative gains in between (E20 +25.6, E21 +21.3)
can explain. The gen-4 net, the timeman fixes and the tunable retune shipped
between the two events without a gauntlet of their own, so either those were
worth ~+230 together, or one of the two events mismeasures; nothing currently
distinguishes the cases. Second, both this event and every SPRT feeding it
are STC-only, and ratings are time-control specific - the CCRL anchors were
earned at 2'+1", not 8+0.08, against 2020-era time management. The claim is
**~3300 blitz, provisional**, until a 40+0.4 gauntlet with CCRL-comparable
hash confirms it.

**Do not read the agreement with the 2026-08-28 figure as a validation.** Two
things differ at once - the pool (CCRL's, not Stockfish's self-calibration)
and the time control (STC here, LTC there). Landing 4 Elo apart across both is
a coincidence of the size the confidence intervals permit, not a replication.
There is no same-build STC number on the `UCI_Elo` ladder to difference
against, which is what a real cross-check would need.

**Read the bracket, not the sweep.** berserk (56.8%) and weiss (24.4%) straddle
the 50% crossing; they imply 3181 and 3060 and disagree by 121 Elo, which their
intervals permit (z = 0.76). The ethereal rung is a 4.5% near-sweep, and its
±246 correctly says it locates nothing - it is in the table because omitting a
rung after seeing its result is how a ladder gets talked into the answer you
wanted, not because it carries weight.

**One pattern worth watching, not yet a finding.** The two handcrafted-eval
opponents imply lower ratings for us (weiss 3060, ethereal 2897) than the three
network ones (halogen 3090, berserk 3181, clover 3092). If that is real rather
than 44-game noise, the likeliest cause is time-control scaling: CCRL Blitz is
2+1 and this ran at 8+0.08, roughly 15x faster, and a cheap evaluation converts
that deficit into depth better than a network does. It would mean the CCRL
numbers slightly understate the handcrafted rungs *at this TC*. Re-running at
LTC would separate the two explanations; until then it is an anomaly on the
record, not a correction to apply.

The interval is statistical only, and understates the real uncertainty for the
usual reason: a rating list is a pool, and ours is not CCRL's. Quote this as
"roughly this class at blitz", never as a rating the engine holds.

**2026-08-28**, after E17, at LTC 40+0.4, on the **network** build:

| SF rung | W-L-D | score | implied rating |
|---|---|---|---|
| 3000 | 114-64-66 | 60.25% | 3072 ± 45 |
| 3190 | 76-139-85 | 39.50% | 3116 ± 40 |

**Combined: 3096 ± 30.** Interpolating the 50% crossing between the two rungs
independently gives SF-3094, which is the same answer by a different route.

This is a better-anchored measurement than the 2026-08-19 one below, and the
reason is the bracket: both rungs sit near even (60.25% and 39.50%) with the
crossing between them, so neither leans on extrapolation. The older table's
3000 rung scored 28.5%, far enough from even that its implied rating was mostly
inference. The two rungs here disagree by 44 Elo (z = 1.43, not significant),
and in the direction the compression artefact predicts - the higher rung
implies the higher rating, exactly as it did in 2026-08-19's 2791/2790/2840/2915
progression. Read that as the lower rung being the safer of the two.

The **classical** build at the same time control, for comparison: 38-66-32
against SF-3000, 39.71%, implying **2927 ± 60** on one rung. The implied gap to
the network is +145, but that is two ladder runs differenced rather than a
head-to-head, and E11 measured the same gap at +238 at STC before correction
history and the search tuning both landed. Correction history was worth more to
`eval.c` (+25.8) than to the net (+17.3), and E17 transferred to the classical
build despite being fitted against the network, so a narrowing gap is the
expected direction. A head-to-head SPRT is the way to actually measure it.

**What moved since the 2026-08-19 table**: the network (E11, E12), correction
history (E14) and the search parameter fit (E17). The time control differs too,
so the two blocks are not directly subtractable.

---

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

### E24 — An engine-specific Syzygy prober replaces Fathom

**Date** 2026-09-02 · **Baseline** the E23 build · **Bench** classical 277139
-> **277139**, nnue 203047 -> **203047** (unchanged; probing is off unless
`SyzygyPath` is set, and bench never sets it)

E23 vendored Fathom to read the tablebases. This replaces it with
[`src/syzygy.c`](../src/syzygy.c), written against this engine rather than
against a general-purpose board, and deletes the vendored copy. CREDITS.md
records that the result is *derived* from Fathom and de Man's reference and
retains the MIT notice - the constant index tables and the shape of the
decompressor are the file format and could not be written differently.

**What it drops.** Of Fathom's 3,783 lines: all of `tbchess.c` (1,050 - its own
board, move generator and attack tables, all of which this engine already has),
depth-to-mate support (~350 lines, for `.rtbm` files no distribution ships),
the `TbRootMoves` helper API, and the `EncInfo` gaps those left behind.

**Verification is the point of this entry.** A tablebase prober that is wrong
does not crash: it returns a plausible number for a plausible position and
every low-piece label the generator writes is quietly poisoned. So Fathom was
kept in the tree as an oracle and the two were compared position by position.

The oracle was kept in the tree while the new prober was written, and the two
were linked side by side by a temporary `tools/tbdiff.c`. Material coverage is
exhaustive **by construction** - the 286 configurations up to five men are
enumerated, and only the placement within a configuration is sampled, because a
prober that is wrong for one endgame is wrong for a whole table and uniform
position sampling would find a small table only in proportion to its size.

| | |
|---|---|
| positions compared | **4,111,419** (and 411,106 at a second seed) |
| WDL probes agreeing | 4,111,419 / 4,111,419 |
| DTZ root probes agreeing | 4,087,888, compared as **integers**, not as win/draw/loss |
| chosen moves replayed and re-checked | 408,768 |
| positions carrying an en passant square | 7,891 |
| disagreements | **0** |

**The harness was itself tested.** Agreement is worthless if the test cannot
fail, so faults were injected and confirmed caught: treating a cursed win as a
win (a 0.08% effect) produced 50 mismatches across 6 configurations, and the
inverted root filter described below produced 5,948 across 218.

**Four real bugs, and what found each.** Worth recording because they map onto
what each check is actually for.

1. *Table names were built in ascending piece order* (`KPNvK`, `KPvKQ`), and the
   format names the strongest man first and one side first (`KNPvK`, `KQvKP`).
   Every pawnful table silently failed to load. Found by the differential run;
   fixed by generating both side orderings and letting the filesystem decide
   which exists, rather than deriving a naming rule with exceptions in it.
2. *The block index offset was read as signed.* It is unsigned in the format;
   read as signed it goes negative, the block walk runs off the front of the
   size table, and `--block` underflows a `uint32` into a four-billion-entry
   read. Found as an access violation at a larger sample - the only bug here
   that announced itself.
3. *The root move chooser's draw branch had an inverted filter*, rejecting our
   own winning moves and never rejecting a losing one, so it degenerated to
   "play the first legal move" - which in a drawn position can lose outright,
   and `search.c` cuts the root list down to whatever it picks. **The
   differential test could not see this**: it compared value and distance, both
   of which were perfect. Found by a code review, and the harness then grew a
   check that plays the chosen move and asks the oracle what the child is
   worth.
4. *`TB_MAX_PIECE_TABLES`/`TB_MAX_PAWN_TABLES` were the six-man counts* while
   `TB_PIECES` was 7, so a complete six-man set would fill both arrays exactly
   and every seven-man table would be dropped by a silent `return`. Invisible
   to a five-man test by construction. Found by the same review; the caps are
   now the seven-man counts and the guard says so out loud.

**Speed: the hypothesis did not survive contact.** The rewrite was motivated
partly by expecting it to be faster - no marshalling into a second board
representation, and capture resolution on our own generator and make/unmake.
Measured over 205,576 positions:

| | before | after `GEN_CAPTURES` |
|---|---|---|
| WDL probes | 0.74x Fathom | **0.86-0.96x** |
| DTZ root probes | 1.03x | **1.12x** |

The WDL figure varies ~10% run to run, so read it as *parity, possibly slightly
behind*. Narrowing capture resolution from `GEN_ALL` to `GEN_CAPTURES` was worth
a real step; a second pass adding a bare-king fast path measured neutral and was
**not kept**. The honest summary is that this is a wash on speed, and its value
is the dependency removed and the ~2,400 lines not carried.

**Gates.** `bench` classical 277139 and nnue 203047, both unchanged · `perft`
14/14 · `openbench-check` · `format-check` · `datagen-test` · `syzygy-test`,
which now runs the sixteen-endgame suite **and** the manifest.

**What survives.** `tests/syzygy/probe.manifest` - 8 KB, one checksum per
material configuration, sealed from Fathom before it was deleted. `make
syzygy-test` re-derives all 286 with this prober and names the endgame that
differs. Verified to fail: corrupting one line reports
`FAIL KQvKR expected 0000000000000000, got 390339f8b1d64cf6`.

**Caveat.** Everything above is five-man. The prober claims seven and the
enumeration reaches it, but no six- or seven-man table has ever been probed by
it - the review's finding 4 is exactly the kind of bug that lives there. Fetch a
six-man set and re-seal before trusting one.

---

### E25 — Chess960

**Date** 2026-09-02 · **Baseline** the E24 build · **Bench** classical 277139
-> **277139** (unchanged, and that is the claim: for a standard position the
derived geometry resolves to the squares the constant table held, so this is
not a behavioural change to standard chess at all)

The move encoding was already king-captures-own-rook. What was missing was the
rules: `board_set_fen` rejected Shredder-FEN outright, and `movegen.c` held a
`CastlingSpec` table of fixed squares. Both are replaced by geometry derived
per position — rook origin, the squares that must be empty, the squares the
king occupies or crosses — so there is one code path for both variants and no
runtime variant switch below the FEN parser.

**Why the bench number is the headline.** Standard chess is the thing that must
not move. A castling rewrite that changed even one node count would mean the
derived geometry disagreed with the table it replaced somewhere, and every Elo
measurement taken since E24 would be against a different engine. It was checked
by building HEAD and the patch side by side rather than by argument.

**The three bugs Chess960 has that standard chess cannot.** Each is a real
rule, not an edge case, and each has a line in `tests/perft/chess960.epd`:

1. *The rook screens its own king.* King c1, its rook b1, an enemy queen a1.
   Castling long does not move the king at all and drops it into check, because
   the rook that was blocking the queen has just left. The king's path scans as
   **safe** while the rook is still standing on the line. `movegen_is_legal`
   therefore also rejects a castle whose rook is pinned. Standard chess never
   trips this: a rook on a1 or h1 has no square behind it to be pinned from.
2. *Either piece may not move, and they may swap.* King g1 with rook h1 leaves
   the king where it is; king e1 with rook f1 leaves the rook where it is; king
   f1 with rook g1 exchanges the two. So both pieces are lifted before either
   is put down, and each piece's own square is exempt from the path-is-clear
   test — otherwise a castle is generated as blocked by itself.
3. *`KQkq` cannot describe every position.* With two rooks on one side of the
   king, "the outermost rook" does not say which may castle. Shredder spelling
   (the rook's file) is parsed on input and emitted for Chess960 positions.

**Verification, against an independent engine.** Same method as E24 and for the
same reason: there are no published perft numbers for Chess960 beyond the
standard array, so the alternative to an oracle is numbers recalled at a
keyboard, which is how a suite ends up certifying the bug it was written
beside. Stockfish 18 was the oracle, compared **divide by divide** — a mismatch
names the move, not just the total.

| | |
|---|---|
| random-walk positions compared, divide by divide | **5,550** |
| all 960 start positions, depth 4 | 181,106,056 nodes |
| all 960 start positions, depth 5 | 4,614,154,886 nodes |
| hand-built castling edge cases | 22 positions, 8,543,750 nodes |
| self-test walk over all 960 arrays | 23,027 positions |
| disagreements | **0** |

**The cross-check that costs nothing.** SP 518 is the standard array. It is
sealed in `chess960-startpos.epd` spelled `HAha` and appears in `standard.epd`
spelled `KQkq`; both give D4 197281 and D5 4865609. The two suites reach one
board down completely different parser paths, so agreeing there is worth more
than either count alone.

**One real bug, found by the test that exists for it.** `move_to_str` read a
file-static `OptChess960` rather than the position it was spelling. The
self-test's notation check caught it immediately at
`1b1r1k1r/pnpppppb/1pqn3p/2N5/3P4/5PPP/PPP1PK2/QB1NR1BR b h - 2 8`, where a
black king on f8 can both castle short and step to g8 — and both spelled
`f8g8`. Every perft count stays correct through that bug; a GUI just plays the
wrong move. The fix passes `pos->chess960` as a parameter, so the spelling
cannot drift from the board.

**Also fixed by the same reasoning.** A FEN that only Chess960 can describe now
latches the notation on by itself, so a GUI that sends such a position without
setting `UCI_Chess960` is not handed ambiguous moves.

**Gates.** `bench` classical 277139, unchanged · `perft` now 4 suites, 1,003
positions · `perft-all` at full depth, 4.8 billion nodes · `chess960-test` ·
`openbench-check` · `format-check` · a debug build (assertions on:
`board_is_consistent`, incremental key vs recomputed, evasions vs `GEN_ALL`)
over all 960 arrays at depth 4 · **80 complete Chess960 games** under
fast-chess, which validates every move against its own board: 64 castles, no
illegal moves, no protocol failures.

**What survives.** `tools/chess960diff.py`, which sealed the counts and can
re-earn them (`make chess960-campaign`), and `src/test/chess960test.c` for the four
things a node count is blind to — the SP numbering (perft cannot know the
position it was given was the one asked for), FEN round-trips, notation
ambiguity, and do/undo, which castling can break in exactly compensating ways.

**Caveat, and it is not small.** This is the RULES only. Nothing in the
evaluation knows about Chess960: the king-safety and pawn-shelter terms were
fitted on standard games, and the NNUE king buckets were trained on positions
where the king starts on e1. Both read the king's actual file rather than
assuming e1, so they are not *wrong*, but they are untuned for the other 959
arrays and no Chess960 SPRT has been run. The engine plays Chess960 legally and
at unmeasured strength.

---

### E26 — gen-004 is ten copies of itself, and the gen-5 datagen hardening

**Not an SPRT.** This is a measurement of the datasets on disk, made while
preparing the gen-5 generation run, plus the changes it motivated. It is here
because it changes how an existing result should be read.

**The measurement.** Hash each record's first 25 bytes — occupied, the piece
nibbles, and the side-to-move/en-passant byte, which is the whole position and
nothing else — and count distinct values over the entire file:

| dataset | records | distinct positions | mean copies |
|---|---|---|---|
| `gen-003`, labelled human corpus | 178,808,618 | 156,127,545 | 1.15 |
| `gen-004`, self-play | 122,722,327 | **11,743,846** | **10.45** |

`gen-004`'s redundancy is uniform across every piece count — 10.2× at 25-32
pieces just as at 2-6 — which rules out the comfortable explanation. Endgame
convergence would concentrate in the low buckets; a flat 10× across the opening
means the *games* repeat.

**The cause, and it is already fixed.** The manifests name their derived
worker seeds. `gen-004_sp_b00_00` records 1000 and `b00_01` records
11400714819323199485, which is exactly `1000 + 0x9E3779B97F4A7C15` — the
pre-fix `worker_rng`, `seed + index * GOLDEN`. `rng_next` advances splitmix64's
state by precisely that constant, so worker *k* started *k* draws into worker
0's stream and replayed its neighbours' games. Measured directly: two shards
from one batch of that run share **99.3%** of their positions, two shards from
different batches share 0.5%, and 8 workers of the current build share 0.09%.

`worker_rng` was fixed before this measurement, and the comment above it in
`tools/datagen.c` describes this exact failure. What it did not have was
anything that would *notice* — dedup is per shard, each worker writes its own
file, and no stage held the whole dataset in one stream.

**What it means for E18.** E18 recorded the `gen-003` re-labelling as
"marginal, not a step change" and concluded that position *coverage*, not label
quality, was the binding constraint. That conclusion survives, but the gen-004
half of the evidence was taken on an effective 11.7M positions against 24,576
feature rows. Any comparison involving `gen-004` is a comparison at a tenth of
its nominal size, and the shards should be re-shuffled through the dedup below
before being mixed into a gen-5 set — otherwise their positions arrive carrying
ten times the weight of a gen-5 one.

**What changed, all of it gated by `make datagen-test`.**

- **`datagen shuffle` deduplicates**, by default, reporting the count. It is
  the only stage that can see across shards, since the workers are separate
  processes on purpose. `-nodedup` restores a pure permutation.
- **A game is a function of `-seed` and its global ordinal.** Games are dealt
  round-robin by ordinal and seeded from it, so `-threads` no longer changes
  the dataset a seed produces — asserted by comparing a 1-worker run against a
  4-worker one, record for record.
- **`selfplay -resume`**, which follows from the above: a checkpoint every 30
  seconds at a game boundary, and a rerun rejoins there. Verified by killing a
  30-game run at 50 seconds and resuming: the shard and its policy sidecar came
  back **byte-identical** to the uninterrupted one.
- **Game progress per record**, in the format's four reserved bits: how far the
  position was from the end of its game, in eight-ply buckets, 0 for "not
  recorded". It cannot be backfilled, and it is what `--lambda-progress` reads.
- **`-maxplies` is validated** against `MAX_GAME_PLY - MAX_PLY`. A game and the
  deepest search inside it share one history array, and the cap used to be an
  unchecked `atoi`.
- **`datagen stats` reports `decisive scores`** — the share of labels that are
  a mate or a tablebase result rather than an evaluation. On a tablebase run
  with games played out it measures 4.6%, where the old `-maxscore 2000` used
  to drop them entirely.

**And on the trainer side**, the dials that let a gen-5 label be used for what
it is worth: a per-record lambda keyed on game progress, phase and source
tag, a score clip, and per-source loss weights. Every one defaults to no
effect, so this changes no existing run. See [NNUE.md](NNUE.md), Task 2.

**Gates.** `make datagen-test` (round-trip, label reproducibility, book start,
opening parity, ply-capped results, progress pairing, seed/thread invariance,
shuffle dedup with the sidecar in step) · `make trainer-test`, 100 passed ·
`datagen verify` on a pre-change `gen-004` slice, 100,000 records, 0 failures,
which is what makes the format change backwards compatible. No `src/` file was
touched, so bench and perft are unaffected by construction.
