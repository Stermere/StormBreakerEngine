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
--workers 6`, on `gen-012.cnn` (8.14 GB, 254M records). Steady-state
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
carrying a published CCRL rating, and `make ratings` reads the PGN back and
fits one rating for every seat against all of them at once.

Entries dated 2026-09-04 or later come from that joint Bradley-Terry fit, in
which the rungs' games against EACH OTHER also constrain the answer, so a rung
that underperforms shows up as its own residual instead of quietly moving the
mean. Earlier entries averaged the per-rung implied ratings by inverse variance
and have no residual column; the two agree to within a few Elo when the rungs
do, and diverge exactly when it matters that they did not.

Entries below dated before 2026-08-30 use a second ladder — Stockfish `UCI_Elo`
rungs, run by `tools/rating.py`. Both the tool and the ladder are gone. They
were never the CCRL pool, and a strength-limited Stockfish is not a genuinely
weaker engine, so those numbers do not difference against the CCRL tables. The
engine is now strong enough to play Stockfish unlimited instead — `make gauntlet
ARGS="--field stockfish"` — which is an honest opponent rather than a rung.

**2026-09-20 — the ladder was replaced below 3426.** Not a measurement: a
change to what the measurements are made against. The field of 2026-09-04 ran
from 3008, and the gauntlet of this date (150 games per pairing, 1,050 for the
engine, LTC 40+0.4) scored 96.3%, 86.7%, 85.0% and 78.0% against its four
lowest rungs. Two things follow from that. A 96.3% score pins the rung's
implied rating to ± 53 and costs exactly the machine time a 40% score would;
and the four weak rungs implied 3537 ± 16 for the engine against the three
strong ones' 3474 ± 13 — 63 ± 21 Elo apart, three sigma, with nothing to
arbitrate it, because a rung being swept is not measuring the engine so much as
recording that it is stronger.

So halogen-8.1 (3008), berserk-4.1.0 (3133), weiss-1.4 (3256) and clover-3.0
(3340) left `CCRL_LADDER`, and **berserk-8.5 (3575 ± 12)**, **viridithas-12.0.0
(3600 ± 13)**, **clover-6.0 (3628 ± 10)** and **seer-2.8.0 (3640 ± 7)** took
their seats. ethereal-12.75, carp-3.0.1 and koivisto-8.0 stay. Still seven
rungs and seven authors, still single-file binaries with the net embedded, now
spanning 3426-3640 — every seat within about 170 Elo of the engine, six of the
seven above it.

One check the change makes cheap: re-running `make ratings` on that same PGN
now fits it on the three surviving anchors alone, since the other four are no
longer rated in `CCRL_LADDER`, and returns **3481 ± 18** where the seven-anchor
fit returned 3500 ± 17 — the strong band's answer, reproduced from games
already on disk, and within a few Elo of the 3474 that
[ROAD_TO_3600.md](ROAD_TO_3600.md) sizes its plan against.

**What it costs.** Four of the seven seats changed, so gauntlet tables from
before this date and after it do not difference: the fit is relative to the
field, and the field moved. Any conversion factor between self-play SPRT Elo
and gauntlet Elo has to be re-established here rather than carried across, and
the first run on this ladder is a new baseline rather than a continuation of
the 3500 ± 17 one.

**2026-09-04**, at STC 8+0.08, 16MB hash, on the network build, against the
five-rung CCRL ladder. Round-robin, 1217 games, ~81 per pairing. Fitted with
`make ratings`, which reads the PGN back and puts every seat on one scale:

| CCRL rung | W-L-D | score | this rung alone | fitted estimate |
|---|---|---|---|---|
| halogen-8.1, 3008 | 74-3-5 | 93.3% | 3465 ± 126 | 3008 (resid -0) |
| berserk-4.1.0, 3133 | 63-6-13 | 84.8% | 3431 ± 77 | 3122 (resid -11) |
| weiss-1.4, 3256 | 57-12-13 | 77.4% | 3470 ± 71 | 3262 (resid +6) |
| clover-3.0, 3340 | 51-10-21 | 75.0% | 3531 ± 65 | 3311 (resid -29) |
| ethereal-12.75, 3426 | 34-23-25 | 56.7% | 3473 ± 52 | 3458 (resid +32) |

**engine: 3483 ± 33** (95%) on the CCRL Blitz scale, residual rms 20 Elo over
the five anchors, anchor slope 0.94. The five rungs' own games against each
other are in the fit, which is why the rungs get an estimate of their own and a
residual: this field ranks them in CCRL's order and spans 94% of CCRL's spread,
so it is internally coherent, and the +32 on ethereal is the only rung more
than one CCRL error bar out.

**The number is an extrapolation, and that is why the ladder grew.** 3483 sits
57 Elo above the top anchor. Every rung below it is a near-sweep - four of the
five are 75% or more, and a 93.3% score locates a rating to ± 126 - so the fit
is pinned from below and open above, exactly the direction it is least
constrained in. Adding rungs the engine does NOT sweep is the fix, so
`CCRL_LADDER` gained **carp-3.0.1 (3528 ± 9)** and **koivisto-8.0 (3593 ± 10)**,
the two best-measured CCRL engines in that band (4855 and 2789 games there).
Both bracket 3483 from above. Nothing above claims a rating on the strength of
this event; re-run the gauntlet on the seven-rung field before quoting one.

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

### E26 — gen-5 datagen hardening

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

**Yield, measured while sizing the run.** `-nodes 10000 -threads 14`, ply-20
CCRL book, `-opening 2-3`, 3-4-5 tablebases, games played out: **92 records per
game** over 700 games, 4.36M records per hour, so 100M records is about a day
from 1.09M games. The shuffle dropped 0.24% of that run as duplicates, against
90.43% of `gen-004`.

**What the second search per position buys, measured.** Every game-line
position is searched twice — once by the game, with a warm table, and once from
a cleared engine to produce the label — and that is half the cost of a run.
`selfplay -warmdiff` reports the difference. Over 33,282 positions, 280 games,
`-nodes 10000`:

| | |
|---|---|
| cold − warm, mean | **+0.59 cp** |
| cold − warm, mean absolute / RMS | 74.3 / 272.7 cp (median ~25) |
| depth reached, warm / cold | 16.05 / **14.84** |
| the two disagree about who stands better | 2.08% |
| the quiet filter would decide differently on the warm best move | 4.86% |

So the cold re-search was **not correcting a bias** — there isn't one at 0.6 cp
over 33k positions — and it was **1.2 plies shallower**, i.e. the worse of the
two estimates. **It was removed**, along with the tree sampler, and the label is
now the score the game's own search returned.

The gain was **5.2×**, not the 2× the search count suggests, and the extra
factor is worth naming: `label_position()` called `search_clear()` per
candidate, which memsets the whole 64 MB transposition table — roughly 10 GB of
memset per game per worker. Dropping the re-search also let the quiet filter
move *before* the record is built, so the ~40% of positions it discards cost
nothing at all now; the game's best move decides that filter the same way the
label search's did on 95.1% of positions, from 1.2 plies deeper.

| `-nodes 10000 -threads 14`, book, tablebases | before | after |
|---|---|---|
| records per game | 92.2 | 92.0 |
| records / hour | 838k | **4.36M** |
| records / day | ~20M | **~105M** |

**What replaced the gate.** `verify -relabel` re-searches a stored position and
requires the same score; that only means anything for a shard whose scores were
a search from a cleared engine. It now reads the manifest's new `labels` field
and refuses a game-labelled shard by name, rather than reporting a wall of
mismatches — a gate that always fails is worse than no gate. What checks a
self-play shard instead is **regeneration**: a game is a function of `-seed`
and its global ordinal, so the same command must produce the same bytes.
`make datagen-test` asserts that, and separately keeps `-relabel` alive on a
three-line `label` shard where it still means something.

**Tree sampling went with it**, including the `-DDATAGEN` node visitor in
`negamax` — so `datagen` now compiles the same sources the engine does. Bench
is unchanged at 203047 nodes, as it must be: the hook was never in an engine
build. The `tree` source tag stays in the record format, because removing a tag
value would renumber the others.

**Gates.** `make datagen-test` (round-trip, label reproducibility, book start,
opening parity, ply-capped results, progress pairing, seed/thread invariance,
shuffle dedup with the sidecar in step) · `make trainer-test`, 100 passed ·
`datagen verify` on a pre-change `gen-004` slice, 100,000 records, 0 failures,
which is what makes the format change backwards compatible. No `src/` file was
touched, so bench and perft are unaffected by construction.

---

### E27 - three candidates against the gen-5 net, all rejected

**Date** 2026-09-05 · **Baseline** `606d1d4`, net `34aaa009f3db` · **Recorded
after the fact**, from the run PGNs in `external/games` and the verdicts of
whoever ran them. None of the three reached a commit, so this entry exists to
stop them being proposed again - a rejected candidate that leaves no record is
one the roadmap will re-derive in a month.

**A caveat this entry cannot remove.** These runs were not written up as they
happened, and a fastchess PGN records the seat names (`dev`, `sb-base`,
`tuned`, `default`) rather than what was in them. The verdicts below are the
runner's; the game counts and Elo are recomputed from the PGNs; the pairing of
one to the other is certain only for the first, where the binaries are still on
disk under their own names. Read the numbers as sound and the attributions as
reported.

#### 1. Sigma-scaled LMR - rejected

Reductions conditioned on the uncertainty head the way the pruning margins
already are, via two new seats `LmrSigmaLo` / `LmrSigmaHi`. The source was
reverted; the build that carried it survives as `sb-lmrsigma.exe`, and
`sb-tune.exe` still advertises both seats, which is what makes this attribution
certain.

| vs `sb-base`, STC 8+0.08 | |
|---|---|
| Games | 256 (230 + 26) |
| W/L/D | 51 / 73 / 132 |
| **Elo** | **-29.9 ± 29.6** |

Stopped early and correctly: the point estimate is a full standard error below
zero and the interval does not reach the [0, 5] bound the test was looking for.

**Why it is worth having tried.** The margins are a claim about how much the
static evaluation can be wrong by, and sigma answers exactly that. A reduction
is a different claim - how much of the tree below a move can be skipped - and
the uncertainty of the evaluation AT this node says little about it. The
negative result is consistent with that reading rather than with a bug.

#### 2. Re-centring `unc_scale()` on the gen-5 net - rejected

Roadmap item 3 and [NNUE.md](NNUE.md) 5c: `UncSigmaBase` 73 -> 57,
`UncSigmaSlope` 13 -> 12, the pair that reproduces on net `34aaa009f3db` the
distribution E22a's fit saw on `0ba56166ba9c`. One `TUNE_SEARCH` build, both
seats, no rebuild either side.

| same binary, options only, STC 8+0.08 | |
|---|---|
| Games | 656 |
| W/L/D (tuned) | 179 / 193 / 284 |
| **Elo** | **-7.4 ± 20.0** |

**This is the result that matters most, because 5c predicted it would pass.**
The measurement behind it stands - the margins really do run ~13% wide on this
net and a third of nodes really are pinned at the cap - but re-centring the
distribution did not convert into Elo. The reading that survives is that
matching the *fitted* distribution was the wrong target: it treats the mapping
as correct-but-displaced, when 5c's own error table says the SHAPE is wrong
over half the tree. Re-centring a curve of the wrong shape moves it from one
kind of wrong to another.

That is what motivated E28's split rather than another pass at the constants.

#### 3. A wider net (h1024) on gen-005 - rejected

The gen-5 corpus retrained at `--hidden 1024` against the shipped 512, same
data, same schedule, same lambda. The checkpoint is still on disk as `external\nets\net.pt`
(val 0.006623, tag `epoch4-h1024`), and `net.json` currently describes IT
rather than the shipped net - the same trap E18 records, sprung again.

| vs the shipped h512, STC 8+0.08 | |
|---|---|
| Games | 1986 |
| **Elo** | **-7.0 ± 11.3** |

Doubling the accumulator cost roughly its own nps and returned nothing
measurable. **Coverage, not capacity, is still the binding constraint** - which
is what E18 concluded from the other direction and what item 1 of the roadmap
already says. A wider net is not the way to spend the next generation; more
varied data is.

---

### E29 - Time management: the sudden-death horizon was a decay rate

**Date** 2026-09-06 · **Commit** `7b3d7c0` · **Baseline** `sb_old.exe`, which is
that same build with `src/timeman.c` reverted to `4e1ee5b`. Both binaries report
`StormBreaker 0.4.0` and both bench 220800, so `timeman.c` is the only variable -
including the `UNC_SCALE_SLOPE` 1 -> 2 reset that rode in on the same commit,
which was present identically on both sides.

E28 is deliberately skipped: E27 already refers forward to it for the
uncertainty-head split.

**The defect.** `timeman_init()` divided the clock by 20 whenever `movestogo`
was absent. That is not an estimate of how long the game runs. A twentieth of
what is *left* is spent every move, so the divisor is the rate the clock decays
at, and the right rate depends entirely on whether an increment refills it. The
file's own comment shows the design was reasoned only about the increment case -
"being paid `inc` back is a contraction, so the clock converges". With `inc = 0`
that contraction does not exist and the clock decays geometrically to nothing;
with one it converges on `moves / 4` times the increment, which at 8+0.08 is
400ms.

**The change.** One line - reuse `MOVESTOGO_CAP` (50) as the horizon when
`movestogo` says nothing, instead of 20.

**Allocation, read out of `timeman_init()` directly** rather than inferred from
games, so these are exact:

| 8+0.08 | old opt / clock | new opt / clock |
|---|---|---|
| move 1 | 381 / 8000 | 177 / 8000 |
| move 20 | 196 / 2619 | 169 / 5235 |
| move 40 | 88 / 712 | **116** / **3099** |
| move 60 | 61 / 346 | **81** / **2087** |
| move 80 | 54 / 338 | **66** / **1681** |

The point that makes this a fix rather than a preference: it spends *more* per
move from about move 25 onward **and** holds six times the reserve. Only the
first twenty moves pay for it, which is where `PhasePercent` already says the
least is at stake. At 60+0 a played-out 50-move game ended with 164ms on the old
clock against 13.6s on the new.

| vs `sb_old`, STC 8+0.08, bounds [0, 5], UHO_Lichess_4852_v1 | |
|---|---|
| Games | 318 |
| W/L/D | 218 / 10 / 90 |
| Ptnml(0-2) | [0, 1, 14, 79, 65] |
| **Elo** | **+271.84 ± 31.03** (nElo +491.80 ± 38.19) |
| LLR | 2.59 (87.8%) of (-2.94, 2.94) |

Recorded at the 318-game snapshot, where the LLR had not yet crossed 2.94. LOS
was 100% and the lower bound sat 240 Elo above the H1 threshold, so the verdict
was not in question, but the entry records what was actually observed.

**Why the number is this large, and what it is not.** E23 removed every
adjudication rule, so games run to natural termination - median 68 moves across
the 281 games in the run PGN, 83% of them past move 50. Overlaid on the table
above, the baseline was spending at or below its own 80ms increment for roughly
the last two thirds of every game. All 281 games terminated `normal`; there
were **zero time forfeits**, so this is play quality under starvation rather
than flagging, and the score recomputed independently from the PGN (82.97%)
matches what fastchess reported.

That also bounds the claim. +272 Elo is measured against an opponent with
exactly the weakness the patch removes, under conditions that maximise it - an
unbalanced book and no adjudication, so every game plays deep into the starved
regime. It does **not** predict +272 against the field. Earlier entries are not
invalidated, because both sides of those tests carried the same allocator and
the handicap was symmetric, but the engine's *absolute* strength has been
understated wherever it was measured against outside opposition, E11's +238
included.

**Owed: confirmation at LTC.** Not yet run. The decay is scale-invariant, so
40+0.4 should show the same shape rather than washing out - at move 60 the old
allocator sits at 325ms with 1.5s left - but `docs/TESTING.md` wants a patch
this size confirmed at long time control before the Elo claim is leaned on, and
STC exercises the deep-endgame regime least.

**Two follow-ups, each its own test.** 50 was reasoned, not fitted, and given
the size of this effect there may be more in 55-60. And `MAX_NOMINAL_MULT` and
the `PhasePercent` curve were both chosen against a divisor of 20; they now sit
on a different base and are plausible SPSA targets.

---

### E30: Surprise-weighted quiet history

**Date** 2026-09-07 · **Baseline** `pre-surprise-512`, commit `3449337` ·
**Net** 512-wide `34aaa009f3db` on both sides · **Status** stopped inconclusive;
implementation, options and dedicated tests subsequently removed.

Scaled winning quiet moves' butterfly/continuation rewards by
`1 + 0.5 * clamp((beta - staticEval) / (sigma + 32), -1, 1)`; penalties unchanged.
Bench d7/d12: **220800/3081372 -> 235643/3124197**.

| STC 8+0.08, 1 thread, 16 MB, UHO, normalized bounds [0,5] | Result |
|---|---|
| User-reported snapshot | 1238 games: 310W / 315L / 613D |
| Elo / nElo | **-1.40 +/- 11.94** / -2.28 +/- 19.35 |
| LLR / Ptnml | **-0.24** / [20, 159, 265, 156, 19] |

Neither boundary was reached; no demonstrated gain, not an H0 acceptance.
The stopped PGN has 1257 completed games (316W/319L/622D), all normal; the
snapshot's LLR/error bar must not be attached to that later count.

The d12 diagnostic reduced rewards 3.2 times as often as it raised them,
leaving 92.9% of ordinary positive credit with unchanged penalties. Cutoff rank
improved, but reductions increased; different trees do not prove causation.
Likely design weaknesses: position difficulty is not move difficulty, and
RFP/null-move pruning select which cutoffs ever reach learning. Any rescue-only
contextual reward or depth-aware penalty follow-up must be a separate test.

### E31: Pawn-structure move history

**Date** 2026-09-07 · **Baseline/net** same as E30 · **Status** retained by
user decision after an unfinished, mildly positive SPRT; no H1 verdict.
E30 was disabled in the tested binary and is now removed from source.

Adds a per-thread `int16[512][16][64]` table keyed by parent pawn hash, colored
piece and destination: **1 MiB/thread**, cleared with all other history.
Existing gravity rewards/maluses train quiet moves; context contributes to
ordering and `(butterfly + pawnScore) / LmrHistDivisor`. `PawnHistWeight` is
128/128 by default, range 0..256; zero disables reads/learning, not allocation.
No new evaluation, network, pruning-margin or time-management change.

| STC 8+0.08, 1 thread, 16 MB, UHO_Lichess_4852_v1.epd, normalized bounds [0,5] | Result |
|---|---|
| Games / W-L-D | **2320** / 626-583-1111 |
| Elo / nElo | **+6.44 +/- 8.46** / +10.77 +/- 14.14 |
| Score / LOS | 1181.5 points (50.93%) / 93.23% |
| LLR / boundaries | **0.79** / [-2.94, 2.94] |
| Ptnml | [30, 270, 521, 305, 34] |

Neither SPRT boundary was reached and the Elo interval includes zero. This
snapshot supersedes the early approximately +30 Elo estimate at 500 games;
retaining the feature is a development decision, not a demonstrated gain.

Bench d7/d12: **220800/3081372 -> 204300/3200251**. Before cleanup, repeated
GCC/Clang, native/scalar and debug searches agreed; weight zero reproduced
every baseline iteration and PV. Perft/Chess960, SMP reset, UCI, history and
10000-position NNUE equivalence checks passed. Post-cleanup executable
regression checks remain pending.

The d12 context opposed global history on 459525 lookups. Median time over
eight alternating runs/seat was **1570 -> 1706ms** (+8.7%), throughput -4.4%:
an overhead warning, not strength evidence. LTC confirmation remains pending.

### E32: Rescue-only pawn-history credit

**Status** retained with E33 at weights **25/25** by user decision; combined
SPRT inconclusive (below). Unlike E30, successes never lose ordinary credit and
only the winning quiet move's **pawn** entry gets extra credit.
Butterfly/continuation rewards and all maluses are untouched.

Extra credit is `bonus * weight/100 * clamp((beta - eval)/(error + floor), 0, 1)`,
truncated in int64. `PawnRescueWeight` defaults to **25** (range 0..100; zero
disables it), `PawnRescueFloor` **32cp** (range 1..256). Reuses the existing
uncapped head output; no credit in check, excluded searches, without a head,
or for decisive eval/window/result scores. Raw-error versus corrected-eval
calibration remains an approximation. A survivor needs uniform-credit and
constant-denominator controls before attributing a gain to the head.

### E33: Requested-depth pawn-history penalties

**Status** retained with E32 at weights **25/25**; no H1 verdict. Record a quiet
move only after its search completes. A reduced-only failure gets evidence depth
`clamp(childDepth - reduction + 1, 0, parentDepth)`; any normal-depth retry
restores the ordinary penalty. This is requested depth, not guaranteed work:
TT hits and internal pruning can still return early.

Blend the ordinary and evidence-depth maluses only in pawn history, using
`ordinary - (ordinary - shallow) * weight/100`. `PawnEvidenceWeight` defaults
to **25** (range 0..100; zero disables it). Non-reduced and excluded-search
attempts keep their ordinary penalties. Pruned/uncompleted moves teach nothing;
global/capture histories are unchanged. No new persistent or shared state.

**Combined E32 + E33 result:** user reports the individual trials looked even;
no separate figures supplied. The retained combination is
`PawnRescueWeight=25,PawnEvidenceWeight=25` with `PawnRescueFloor=32` unchanged,
against the same build's then-default **0/0** control.

| STC 8+0.08, 1 thread, 16 MB, UHO_Lichess_4852_v1.epd, normalized bounds [0,5] | Tuned vs default |
|---|---|
| Games / W-L-D | **4080** / 1107-1034-1939 |
| Elo / nElo | **+6.22 +/- 6.27** / +10.57 +/- 10.66 |
| Score / LOS | 2076.5 points (50.89%) / 97.40% |
| LLR / boundaries | **1.36** / [-2.94, 2.94] |
| Ptnml | [42, 484, 933, 521, 60] |

Neither boundary was reached; the Elo interval narrowly includes zero. Retained
as a mildly positive development choice, not a demonstrated gain or evidence
that either adjustment helps independently. Total-gain and LTC confirmation
remain pending; this estimate must not be added to E31's as a measured total.

Regression checks should verify both weights at zero reproduce E31's d7/d12
**204300/3200251** and per-iteration scores/PVs. Run history, perft/Chess960,
SMP and debug gates with each option and both enabled, including classical/headless
compatibility. At `PawnHistWeight=0` neither experiment should have any effect.

Future ablations must explicitly set both weights: new builds default to
25/25, so a default seat is no longer the original 0/0 control.

---

### E34: Factorized NNUE training recipe with a low-LR finish

**Date** 2026-09-08 · **Baseline** `stormbreaker-base.exe`, embedded net
`34aaa009f3db` · **Dev** `stormbreaker.exe`, embedded net `6e5d89a32b73` ·
**Status** **H1 accepted at STC**; the new net is pinned in the Makefile.
LTC confirmation and individual training-change ablations remain pending.

The gen-5 corpus retrained at the same **512x2 hidden width, 8 output buckets,
HalfKA-32sq SCReLU, with uncertainty head**. Training adds a shared 768-row
piece-square factor to the king-specific transformer. Export folds their sum
before quantization, so the engine receives the same feature count, architecture,
and **25,199,776-byte** network format with no additional inference work.

This is a **combined recipe result, not +44 Elo attributed to factorization
alone**. Relative to the documented original gen-5 recipe, it also reduces the
uncertainty-loss coefficient from 0.05 to 0.01, changes the progress lambda
adjustment from -0.4 to -0.2, removes the -0.2 piece-count adjustment, and adds
one low-learning-rate pass. Filtering and metric/provenance fixes were included
in the trainer; this run did not select or reweight sources. No new positions
were generated for the run.

#### Training and export identity

The saved run manifest, rather than a remembered command, records:

| Setting | Value |
|---|---|
| Training / validation records | **548,696,001** gen-005 self-play / **251,882** held-out |
| Initialization / seed | From scratch / 0 |
| Batch size / workers | 16,384 / 4 |
| Main training | 4 full passes, AdamW, LR 0.0005, per-epoch gamma 0.8, weight decay 0 |
| Finishing stage | 1 additional full pass at **0.00001**, retaining Adam moments |
| Feature factorization | Enabled; training-only shared piece-square embeddings |
| Uncertainty coefficient | **0.01** |
| Lambda | Base **0.95**, progress **-0.2**, piece-count **0**; mean applied **0.86685** |
| Sigmoid K / score clip | 400 / 2000 cp |
| Exported checkpoint | Epoch **5**, stage **finish**; QA=255, QB=64, scale=400 |
| Training code / environment | `e9b6c47` with local changes; PyTorch `2.13.0+cu126`, CUDA |

Run ID: `dbe42f3af76d4b3ba383225bda1c5af9`.

- Checkpoint SHA-256: `1946e344c13a0bbe7bc25dcd4dadaa541602d6946170a49bea5f7b4e0035ca06`.
- Network SHA-256: `6e5d89a32b736d74e76d679899aa312031c4ce65650d1e9fc04b012f0f3be7ca`.
- Header tag: `e5-h512-1946e344c13a`.
- Checkpoint: `C:/Users/colli/Desktop/Small_programing_stuff/ChessEngine/external/nets/net-fact.pt`.
- Run manifest: `C:/Users/colli/Desktop/Small_programing_stuff/ChessEngine/external/nets/net-fact-run.json`.
- Export manifest: `C:/Users/colli/Desktop/Small_programing_stuff/ChessEngine/external/nets/net.json`.

The checkpoint and deployed network hashes were checked against the export
manifest, and both match. Querying the two executables confirmed the embedded
net identities above: **this test is against `stormbreaker-base`, not the
automatically selected `pre-surprise-512` from the earlier mis-invoked command**.

#### Match result

The user's fastchess acceptance snapshot:

| STC 8+0.08, 1 thread, 16 MB, UHO_Lichess_4852_v1.epd, normalized bounds [0,5] | Dev vs baseline |
|---|---|
| **Result** | **H1 accepted**, LLR **2.95** across upper boundary **2.94** |
| Games / W-L-D | **1,142** / **408-265-469** |
| **Elo / nElo** | **+43.74 +/- 13.35** / **+66.75 +/- 20.15** |
| Score / LOS | **642.5 points (56.26%)** / **100.00%** |
| Ptnml | **[12, 107, 224, 182, 46]** |
| DrawRatio / PairsRatio | 39.23% / 1.92 |
| WL/DD ratio | 1.49 |

Saved PGN:
`C:/Users/colli/Desktop/Small_programing_stuff/ChessEngine/external/games/20260908-174042-STC.pgn`.
Its final contents contain **1,144 games: 409W/266L/469D**, two more than the
acceptance snapshot. Keep the reported LLR and Elo/error bars attached to
**1,142**, not that later count. The PGN contains **three time forfeits** and
1,141 normal terminations; do not describe it as a forfeit-free run. Earlier
same-seat PGNs are separate runs and are not pooled into this result.

#### Validation and attribution limits

The actual exported net passed **10,000 positions with exact C/quantized-Python
agreement** when this entry was recorded. The export reports float-to-integer
drift of **8.81 cp mean absolute / 52.65 cp maximum** on its vector set; this
is a diagnostic, not an additional Elo measurement.

Held-out value MSE went from **0.00309570** at epoch 4 to **0.00303827** after
the finishing pass; fixed score MSE went from **0.00262030** to **0.00256248**.
Those comparisons use this run's unchanged target policy. They support testing
the finish, but do not establish its independent strength gain. Old gen-5
validation losses used a different target policy and must not be read as a
like-for-like improvement in value prediction.

The saved pre-finish checkpoint permits an epoch-4 versus epoch-5 match. A
matched-budget, same-recipe run without factorization is needed to isolate the
shared factor; uncertainty and lambda changes likewise need their own controls.
What this test establishes is the **new combined net's STC gain against the
named baseline**, not a per-feature attribution or an LTC result.

### E35: Lazy SMP — one throughput win and four measured refusals

**Date** 2026-09-12 · **Baseline** `abd646d` · **Machine** Ryzen 7 5800X,
8 physical cores / 16 logical, Windows 11 · **Status** the binding change is
in; everything else here is a negative result kept so it is not tried again.

Bench node count is **5,016,992 before and after**, so nothing below is a
behavioural change. Two instruments were used, and the difference between them
is most of what this entry is about:

- **nps at fixed movetime** — throughput alone, spread about ±3% per round.
- **time-to-depth at fixed depth** — the metric that tracks strength, and at
  eight threads it has a per-round spread of **±40%**, because whether thread 0
  finishes an iteration quickly depends on whether some helper happened to seed
  the entry it needed. Nothing under ~15% is measurable with it at all.

Every comparison below is interleaved A/B/A/B, because the machine's clocks
drift enough over a few minutes to manufacture a result on their own.

#### Where the parallel search actually stands

Time-to-depth at depth 17 over twelve middlegames, each thread count pinned to
one logical processor per physical core so the numbers describe the pool rather
than the scheduler's mood:

| threads | median TTD | speedup | nodes | nps |
|---|---|---|---|---|
| 1 | 7.020s | 1.00x | 15.3M | 2.17M |
| 2 | 5.540s | 1.27x | 19.6M | 3.51M |
| 4 | 4.092s | 1.72x | 29.3M | 7.20M |
| 8 | 3.756s | 1.87x | 39.5M | 10.95M |

The speedup factors as **throughput scaling divided by node overhead**: 5.04x
of nps against 2.59x more nodes for the same depth. Both halves are worth
attacking, but only the first can be measured without games.

#### What went in: a physical core per thread

Windows will seat two search threads on the two halves of one physical core
while another core is idle. It costs about a tenth of the search - the pair
share an L1 and the vector units the network runs on, so the second thread is
not getting a core, it is getting half of one.

`thread_bind()` previously did nothing on a single-group machine, on the
reasoning that the default affinity already covers it and a mask could only
take choices away from the scheduler. That reasoning is measurably wrong. The
same binary, unpinned against pinned one-per-core, interleaved:

| | min | median | max |
|---|---|---|---|
| free | 13,382,702 | 14,393,042 | 14,948,297 |
| pinned | 14,766,898 | **15,660,151** | 16,069,514 |

**+8.8% median, and pinned won all ten rounds.**

Three things had to be got right before that became shippable.

**`SetThreadIdealProcessorEx` is not enough.** It is accepted, returns success,
and moves the number by **+0.1%** - the scheduler takes the hint and places the
thread wherever it likes. A real affinity mask is what moves the threads apart.
The hint is still issued, because it names which half of the core to prefer and
the mask does not.

**The mask is the whole physical core, not one logical processor.** Binding to
a single processor measured **+5.6%** at eight threads; binding to both halves
of the core measured **+6.8% to +9.2%** across three runs. Naming one processor
buys nothing - no two threads are given the same core until every core has one -
and it stops the thread stepping aside from a processor busy with interrupts.

**It is gated on `poolSize >= CoreCount`, and that gate is not a detail.** Below
the core count, binding is actively harmful: a pool of four on eight cores
measured **-6.2%**, because confining four threads to the first four cores gives
up the idle cores' thermal headroom. A pool of one measured **-5.1%** bound, and
a pool of one is what bench, datagen and a single SPRT game all run on. The gate
also removes the reason not to do this by default: at or above the core count no
core is left idle, and `index % CoreCount` spreads every instance evenly, so two
engines running at once oversubscribe the way the scheduler would have anyway
rather than piling onto processor 0.

Final, unpinned, eight threads, complete change set: **+6.8% median nps, ten
rounds out of ten.** At sixteen threads it is neutral (-1.0% median, inside the
spread). One thread is untouched - the code does not run.

Time-to-depth over sixteen rounds of the same change set came out at **-3.1%
median** - the right direction, and below what that instrument resolves. Do not
quote it as the result; the throughput number is the measured one.

#### Also in: the generation was a data race, and helper entries were born stale

`tt_new_search()` ran inside thread 0's iterations, and every thread in the pool
is woken at the same moment - so the bump happened while the helpers were
already storing. Their entries got stamped with the *previous* generation, which
makes them look one search old immediately and puts the helpers' contribution -
the entire point of Lazy SMP - first in line for replacement. It now runs in
`search_setup()`, before anything is released.

`Generation` itself was a plain `uint8_t` read by every thread on every probe and
every store while another thread wrote it. It is now `_Atomic` and read relaxed,
which on x86 is the same instruction: a thread that reads the old value stamps
one entry one generation early, and that is a heuristic allowed to be
approximate. The replacement scan now reads it once instead of once per entry.

#### Refused: guarding the generation refresh in `tt_probe`

`tt_probe` writes `genBound` on every hit. Instrumented over a search, the
fraction of those writes that **change the byte** is **0.0%** - a hit on an entry
from an earlier search is rare, and once the first hit refreshes one, every later
hit finds the generation already current. 6.85M pointless stores to shared lines
per eight-thread search.

Guarding them measured **-0.28% at one thread** and could not be resolved at
eight. The reason is that the probe and the store land on the **same cluster at
the same node**: `tt_store` dirties that line on the way out regardless, so
skipping the refresh saves no cache transaction at all. The guarded field writes
in `tt_store` are worse - they only help when the value block is skipped, and
cost a compare every time it is not. Neither is in the tree.

#### Refused: removing the depth-skipping schedule

The skip tables are Stockfish's from the years its search used them, and modern
Stockfish has none - so they are an obvious thing to delete. Deleting them costs
**31% more nodes for the same depth** (54.7M against 41.9M at depth 17, eight
threads), with time-to-depth unchanged because nps rises to match. They are
earning their place; leave them.

#### Refused: letting helpers pass the depth limit

With `go depth N` the limit binds every thread, and because the helpers skip
depths they reach it **first** - they stop searching and park while thread 0 is
still grinding through the deepest and most expensive iteration. Stockfish
applies its limit to the main thread only, for exactly this reason.

Making the limit thread 0's alone gave the helpers **54% more nodes** (62.9M
against 40.9M), which confirms they really were idling about a third of the
search. Time-to-depth did not move: **+1.9% median over 14 rounds.** Helper work
beyond thread 0's current depth is worthless - it fills the table with entries
for depths thread 0 has not reached and evicts the ones it needs. It also lets a
helper's depth-19 result be played in answer to `go depth 17`. Reverted.

Nothing here affects a timed search, where the limit is `MAX_PLY` and no helper
ever runs out.

#### Refused: independent processes as a scaling ceiling

Eight single-threaded engines, one per core, were run to establish what the
machine can give before the pool's own costs. They reached 14.7M nps aggregate
on bench against 2.44M for one - but the construction is invalid and the number
must not be quoted as a ceiling. Eight processes carry **eight private 25 MB
copies of the network** against a 32 MB L3, where the pool shares one. On the
middlegame set the pool beat them outright, which is the tell.

#### Still open, and each needs games rather than a stopwatch

- **Node overhead is 2.59x at eight threads.** This is where the remaining
  speedup is, and no throughput measurement can see it.
- `best_thread()` picks by depth then score; Stockfish votes across the pool.
- `TTEntry` spends **5 of its 16 bytes on padding**. Packing to 10 would fit six
  entries in a cache line instead of four - half again as much table for the same
  memory, which matters most under SMP, where the table is under the most
  pressure. Changes replacement, so it is an SPRT.
- `search_clear()` memsets every thread's 8 MB of history from the calling
  thread, which on a NUMA machine puts all of it on one node. Not measurable
  here - this box has one.

### E36: Full main-search staged generation — provisional keep, STC stopped undecided

**Date** 2026-09-12 · **Baseline** `defc301`, preserved as
`stormbreaker-staged-base.exe` · **Net** `f2886d3e2c71` · **Status** correctness
and local throughput checked; **provisional keep after encouraging STC evidence,
stopped for machine-time budget before an SPRT verdict. LTC pending.**

Unlike E15's TT-only attempt, the picker defers quiet generation/scoring past
good tacticals, both killers and the counter-move. All promotions belong to the
main tactical batch without changing quiescence's generator. Evasions, qsearch,
ProbCut, pruning and reduction policies remain unchanged. Invocation-local
buffers survive singular verification at the same ply; canonical encoding
validation replaces the old TT move's implicit generator-membership gate.

Bench depth 13: **5,016,992 original / 5,589,919 staged**. Deferred history reads
and stage-local ties are behavioural. An eager-GENERATION control keeping the
same scoring times produces **5,589,919 exactly**, isolating generation timing
from tree changes. Four alternating native-GCC runs gave median NPS of about
2.488M original, 2.529M control and 2.565M staged (~3.1% and ~1.4% gains).
Total time to depth was LONGER than original, since the staged tree grew.
These short local timings are not evidence of strength or a guaranteed speedup.

The depth-12 profile reached quiet generation at 346,009 of 742,570 non-check
main-picker nodes, avoiding that work at 53.4%. Qsearch and ProbCut generation
are counted separately. Production-picker set tests, exhaustive encoding checks,
picker-driven perft, standard/Chess960 perft, history, SMP, NNUE reference
verification, OpenBench compliance and datagen tests passed. Linux sanitizer
execution is left to CI; Windows debug assertions passed locally.

#### Owner-run STC result

The owner ran the match manually and elected to stop for lack of further machine
time. The reported snapshot was:

| Metric | Reported result |
|---|---|
| Match | dev vs stormbreaker-staged-base |
| Time control / normalized bounds | 8+0.08 / [0, 5] |
| Panel progress | 6,280 games |
| W / L / D | 1,676 / 1,573 / 3,011 |
| Score | 50.82% |
| Elo | **+5.72 ± 4.96** |
| Normalized Elo | +9.91 ± 8.61 |
| LOS | 98.8% |
| LLR / stopping boundaries | **+1.920** / −2.944, +2.944 |
| Pentanomial | [56, 751, 1429, 822, 72] |
| Elapsed | 5:42:22 |
| Decision | **Provisional keep; SPRT inconclusive** |

Source: the owner's pasted live panel, not an independently reanalysed PGN.
Its W/L/D totals sum to 6,260 games and its pentanomial counts to 3,130 pairs
(also 6,260 games), 20 fewer than the panel's progress count. Both are recorded
as supplied; no final PGN/log path was supplied to reconcile the discrepancy.

The evidence favours an improvement and gives practical reassurance against a
regression, enough for the owner's provisional keep. It is **not a passed SPRT
or a formal non-inferiority result**: the LLR did not reach +2.944. The reported
Elo interval and LOS are interim estimates, not substitutes for the sequential
stopping boundary or a proven +6 Elo gain. Stopping for budget leaves the test
undecided. LTC confirmation remains pending; no further matches were launched
by the coding agent.

See [STAGED_MOVEGEN.md](STAGED_MOVEGEN.md) for exact scope, raw timing values,
baseline identity, test/control builds and explicit **manual STC/LTC commands**.
The coding agent checked only their `--dry-run` forms; the STC above was run by
the owner.

### E37: The feature transformer as a sparse matrix product

**Date** 2026-09-13 · **Not an Elo test.** No engine change, no SPRT, and the
net file format is untouched. The network this produces is the same network;
what moved is how long it takes to fit one, so the measurement is positions/s
and the gate is that the loss curve is unchanged.

Reference configuration throughout: `--hidden 512 --output-buckets 8
--uncertainty --feature-factorization --batch-size 16384 --workers 4` on
`gen-005.cnn` (17.6 GB, 549M records), RTX 3070. Rates are the steady-state
window between batch 200 and batch 1000, so worker spawn and CUDA init are
excluded.

| | before | after |
|---|---|---|
| **steady state** | 447k pos/s | **1,387k pos/s (3.10x)** |
| GPU step, same batch resident | 34.6 ms | **11.6 ms** |
| loader alone | 1.59M pos/s | **2.81M pos/s** |
| `unpack()`, one core | 26.3 ms/batch | **19.2 ms/batch** |

**What changed.** Four things, in descending order of what they were worth:

  * the accumulator is `S @ W` against a CSR matrix of ones rather than an
    `nn.EmbeddingBag`, with the backward run off an explicitly built `S^T`.
    EmbeddingBag's dense backward sorts the index array and runs a segment
    reduction over a gradient the full width of the table, every step; two
    cuSPARSE SpMM calls do the same arithmetic for a third of the time.
  * the padding is gone from both sides of the bus. A padded (B, 32) index
    matrix is half padding at the piece counts real data has - gen-005
    averages 15.4 - and every pad slot was memset in a worker, pushed over
    PCIe and then multiplied by. `unpack_sparse` emits one int16 index per
    piece, so a batch is 1.05 MB where it was 4.19 MB and the feature
    transformer does half the work.
  * the perspective swap moved from the accumulators to the indices. Choosing
    between two (B, 32) index matrices in the loader is the same answer as
    blending two (B, 512) float matrices on the GPU, for a sixty-fourth of the
    traffic.
  * factorisation folds into the feature table once per step instead of
    running a second embedding over every piece. It cost 34% of a step; it
    now costs 16%.

Two supporting changes mattered more than their size suggests. Seven implicit
host synchronisations per step - `int(tensor)`, `repeat_interleave` without
`output_size`, `bincount`, and `new_tensor` on a Python int in the metrics -
each drained the queue and left the GPU idle while Python caught up; removing
them took the step from 14.8 ms to 14.2 ms and turned a CPU-bound loop back
into a GPU-bound one. Computing both output heads in one matmul rather than
two took it from 14.5 ms to 12.1 ms.

**The gate.** 300 batches from the same seed, `--hidden 256`, sparse path
against the EmbeddingBag one (driven by a local patch forcing `sparse=False` on
the loader; there is no flag for it, see below):

| batch | sparse | EmbeddingBag |
|---|---|---|
| 50 | 0.383060 | 0.383060 |
| 150 | 0.301605 | 0.301606 |
| 300 | 0.245324 | 0.245322 |
| epoch train value | 0.019960 | 0.019960 |
| epoch val value | 0.006875 | 0.006877 |

`trainer/tests/test_sparse.py` is the standing version of that check: outputs
and gradients, across seven architectures, on real records, to 1e-5 relative
against a measured worst case near 4e-7.

**On determinism.** cuSPARSE reduces in whatever order its work partitioning
lands on, so two runs from one seed are no longer bit-identical. This was
briefly exposed as a `--deterministic-ft` flag and then removed: the
justification did not survive contact. The disagreement is ~1e-6 relative,
which is fp32 summation order; training diverges further than that from its own
chaos inside one epoch; nothing reads a checkpoint bit-wise; and nets are
judged by SPRT, whose error bars are in Elo. The padded presentation remains
the loader's default, so `make_loader(..., sparse=False)` still reaches the
EmbeddingBag path for a differential check - which is also how the test suite
keeps that path alive.

**Other configurations.** The win is largest where the old FT dominated least
in absolute terms; a wider net moves less because the elementwise chain scales
with it and the fixed per-step costs do not:

| | before | after |
|---|---|---|
| hidden 512, 8 buckets, uncertainty + factor | 447k | **1,387k (3.10x)** |
| hidden 1024, 4 buckets | 422k | **919k (2.18x)** |

**Where the remaining time goes**, per 16,384-position step at the reference
configuration. This is the reason the number is 3.1x and not higher:

| | ms |
|---|---|
| feature transformer, forward and backward (cuSPARSE) | 1.4 |
| the activation chain and its backward | 2.3 |
| weight clipping | 1.3 |
| fused AdamW over 13M parameters | 0.9 |
| per-epoch metrics | 0.7 |
| building the batch's sparsity pattern | 0.6 |
| everything else, mostly unfused elementwise kernels | 4.4 |
| **total** | **11.6** |

The loader runs concurrently at 5.8 ms and is no longer the constraint. What
is left is not one hot spot but roughly fifteen separate kernels, each
streaming a 33-67 MB tensor in and back out, which eager PyTorch will not
merge: the step moves about 4 GB through a card that does ~450 GB/s. Closing
that needs a fusing compiler - `torch.compile` needs Triton, which is not
installed and is not a dependency this repository has agreed to take. On the
arithmetic above, fusion plus CUDA graphs would be worth something like
another 1.3x at this batch size, and a larger batch would be worth more again
by amortising the optimiser and the clip - but a larger batch is a different
training recipe, not a faster implementation of this one.

---

### E38: A correctness sweep, and datagen's missing accumulator stack

**Date** 2026-09-16 · **Not an Elo test.** No search, evaluation or time-allocation
rule changed, and bench is **214915** at depth 7 on both sides of every change
below. What moved is datagen throughput, and a set of defects that were costing
nothing in Elo and something in every other currency.

#### datagen ran the network with the accumulator stack turned off

`Threads[0]->es` was assigned in exactly one place, `thread_entry()`. Datagen
calls neither `search_start()` nor `search_set_threads()`, so `pool_start()`
never ran, `thread_entry()` never ran, and the block reached `thread_search()`
with `es == NULL` for the life of the process. `eval_evaluate()` then took the
from-scratch path at every node: 32 feature rows per perspective instead of a
two-row delta. Instrumented, on the shipped path:

    [instr] run_sync es=0000000000000000 started=0 count=1

`thread_search()` now claims the calling thread's state when the block has none,
which also covers `search_start()`'s inline fallback - the path taken when no
pooled thread could be created, which had the same hole.

Interleaved, 8 games x 5000 nodes, one worker, one seed, `-march=native`:

| | labels/s |
|---|---|
| before | 201, 202, 202 |
| after | 366, 369, 363 |

**1.81x**, and the shards are byte-identical (`42d2a434bc7acd218884901c38c167ba`
on both sides), so this is a speedup and not a relabelling. Isolated on bench the
same way - `td->es` forced to NULL against the ordinary build - it is 0.80M nps
against 1.39M at an identical 214915 nodes.

**The effect is AVX2-specific.** At `-march=x86-64-v2` the same pair measures 66
against 66 labels/s: with the vector paths compiled out, the layer stack dominates
and the accumulation disappears into it. `make datagen` inherits the default
`-march=native`, so the shipped configuration is the one that was losing the 1.81x.
`gen-005` was generated this way.

#### Eight `go` spellings produced a search with no deadline

`search_limits_clear()` zeroes the struct, so an absent `wtime` and a `wtime 0`
were the same value, and `timeman_init()` read zero as "no clock was given" -
the sentinel that means bench and `go depth`. A flagged clock therefore bought an
unbounded search, recoverable only by `stop`. Measured with an 8-second cutoff:

    go wtime 0 btime 0 / +winc +binc      go movetime 0 / movetime -1
    go btime 30000 binc 100               go depth 0 / go nodes 0
    go wtime -50 btime 30000

`SearchLimits` now records that a clock field ARRIVED separately from what it
said; a clock at or below zero falls through the existing one-millisecond floors
and answers at once, and `depth` and `nodes` are floored at one so a limit that
cannot be met is not read as no limit at all. All eight now return a legal move
in under 10ms; `movetime 300`, `depth 6`, `nodes 100000` and `wtime 3000` are
unchanged to the millisecond.

#### The rest

* **`bench` never gave the hash back.** It pinned 16 MB for the run - correctly,
  invariant 1 - and left it there, while restoring the thread count it pinned
  beside it. A session that benched and then played did so on a table nobody
  asked for and nothing reports: `hashfull` on one fixed search went 6 permille
  to 116 with `Hash 512` still set. Now 2.
* **`nnue verify` dereferenced a NULL accumulator** when the stack could not be
  allocated - the one site in nnue.c that did not handle what the rest of the
  file documents as legal.
* **`setoption` on a tunable did not end the search first.** It is the only
  option path that skipped `end_search_for_option()`, and it rewrites
  `Reductions[][]`, which every thread indexes per node. TUNE_SEARCH builds only.
* **`quietPawnMaluses[]` is filled unconditionally.** Under TUNE_SEARCH a
  `setoption` could move `PawnEvidenceWeight` off zero between the move loop and
  `update_stats()`, which then read an entry never written. Identical machine
  code in a normal build, where both weights are non-zero enum constants.
* **The main thread's stack was 2 MB against the pool's 8.** `-fstack-usage` puts
  `negamax` at 5,040 bytes and `qsearch` at 4,336, and singular verification puts
  two `negamax` frames on one ply, so a line to MAX_PLY wants two to five
  megabytes - not the "half a megabyte" the comment claimed. Both consumers of
  the main thread's stack run the whole search on it. `-Wl,--stack,8388608` now
  matches the two.
* **A transposition cluster could straddle two cache lines.** `calloc` aligns to
  16 bytes, not 64, so "one cluster, one cache miss" was an aspiration. The block
  is over-allocated by a line and the table slid forward, the same way
  `nnue_load_file()` places the net; calloc is kept so the pages still arrive
  zeroed from the OS rather than being written at `setoption` time.
* **`spin_value` echoed `strtoll`'s saturated value**, so an over-large Hash was
  reported as 9223372036854775807 - a number nobody sent. And a known option sent
  without a value was reported as unknown.

**Gates:** `perft` standard and Chess960 exact, `chess960`/`smp`/`movepick`/
`history` selftests, `nnue verify` 10000 positions exact against the pinned net,
`datagen-test`, `openbench-check`, `format-check`. Bench 214915 throughout.

---

### E39: The pawn-history stack is removed

**Date** 2026-09-17 · **Baseline** `stormbreaker-pawnhist` (a `TUNE_SEARCH=on` build
of E38's tree, driving BOTH seats) · **Net** `f2886d3e2c71` · **Status** **H1
accepted; E31, E32 and E33 are deleted from source.**

E31, E32 and E33 all shipped on SPRTs that never reached a boundary - `+6.44 ±
8.46` at LLR 0.79, then `+6.22 ± 6.27` at LLR 1.36 - and E31 measured a
throughput cost beside them. Three features retained by judgement, on one table,
none of them individually certified. This is the test that settles the set.

#### The test

`dev` is the side with the feature OFF, so the sign reads directly. One binary
drives both seats, which makes the option the only difference between them, and
`PawnHistWeight=0` disables reads and learning for all three at once - E32 and
E33 are both gated on it.

Bounds are **[-5, 0]** rather than the usual [0, 5], because "can this be
deleted" is a NON-INFERIORITY question. The standard bounds would have asked
whether removing it is worth +5 Elo, which nothing claimed, and could not have
separated neutral from harmful.

| STC 8+0.08, 1 thread, 16 MB, UHO_Lichess_4852_v1.epd, normalized bounds [-5, 0] | dev = stack OFF |
|---|---|
| Games / W-L-D | **13,830** / 3578-3517-6735 |
| Elo / nElo | **+1.53 ± 3.35** / +2.65 ± 5.79 |
| Score / LOS | 50.22% / 81.5% |
| LLR / boundaries | **+2.950** / [-2.944, +2.944] |
| Ptnml | [148, 1665, 3259, 1664, 179] |
| Elapsed | 7:13:02 |

**H1 accepted at the upper boundary.** Removing the whole stack is not a loss.

**Read that precisely.** It does NOT say the removal gains 1.53 Elo - the
interval spans zero and LOS is 81.5%, well short of anything. What the boundary
establishes, at alpha = beta = 0.05, is that the alternative - that removing it
costs five normalised Elo or more - is rejected. The feature is not paying for
itself, which is a different and much weaker claim than it being harmful, and it
is the only claim needed to justify deleting it.

#### What it was costing

| | with | without |
|---|---|---|
| per-thread state | 9 MB | **8 MB** |
| time to bench depth 12 | 1778-1795 ms | **1623-1652 ms** |
| bench nodes d7 / d12 | 214915 / 4369806 | 242977 / 4060026 |

The ~8.7% is E31's own figure, reproduced: that entry recorded 1570 -> 1706 ms
and called it "an overhead warning, not strength evidence". It was the warning.
The bench tree is also smaller without it, which is suggestive and is not
evidence - a node count is not a strength measurement, which is what the 13,830
games above are for.

#### What was removed

`src/history.h` and `src/test/historytest.{c,h}` in full; the `PawnHistory`
table from `SearchThread`; `PawnHistWeight`, `PawnRescueWeight`,
`PawnRescueFloor` and `PawnEvidenceWeight` and their sweep seats; the ordering
and LMR reads; the rescue credit and the evidence-depth malus; the `history
selftest` command and `make history-test`. The `errorEstimate` plumbing through
`unc_scale()`/`unc_start()`/`unc_get()` went with it - the rescue credit was its
only consumer - as did `fullDepthSearch`, which only the evidence depth read.

**Correction history is untouched.** `pawnCorrHist` is keyed on the pawn
structure too and the names are one word apart, but it is E14, it is worth
+25.8 Elo, and it answers a different question: how far the static evaluation
and the search have been running apart, not how a quiet move has been ordering.
Nothing in this entry's diff mentions `corr`.

**The removal is verified by node count**, the way E19a verified the cut-node
retry's. The build with the code deleted benches **242977** at depth 7 and
**4060026** at depth 12 - identical to the ablation binary driven with
`PawnHistWeight=0`, which is the configuration the SPRT above actually played.
What ships is exactly what was measured, and not a neighbouring configuration
that happens to compile.

**Gates:** `perft` standard and Chess960 exact, `chess960`/`smp`/`movepick`
selftests, `nnue verify` 10000 positions exact, `openbench-check`,
`format-check`, and clean builds at `TUNE_SEARCH=on` and `EVAL=classical`.

**Still open.** LTC confirmation was not run. The three were never separated
from each other, and now cannot be - if any one of them was carrying real value
it went out with the other two. That is the cost of having shipped them as a
batch; E19b made the same point about the reverse case.

---

### E40: Making the layer stack cheap - exact speedups, then a pairwise L1

**Date** 2026-09-21 · **Nets** `68628c124d51` (gen-6-3, `512x2 -> 16 -> 32 -> 8 +unc`,
SCReLU) and `48428bbbd57e` (gen-6-pw, the same recipe with `--pairwise`) · **Bench** 218889 and
7135323 at depth 13 on gen-6-3, unchanged by every exact change here · **Status** the
exact work is a pure speedup (**+17.7% nps** on gen-6-3). The pairwise net changes the
evaluation and has **no SPRT yet**.

The target was a stacked evaluation no more than 1.5x the cost of the flat one:
- The SCReLU stack reached **2.00x** through exact changes, and no exact change took
  it further.
- The pairwise architecture reaches **1.43x**, with L1 run dense. Its bench signature
  is 291947 nodes; `stormbreaker-pw.exe` is built with it embedded, for the SPRT.

#### How it was measured: replaying a real search

`bench` nps cannot compare two nets, because each net searches its own tree. A fixed
perft walk can compare them, but it evaluates every node, and a search does not.

A throwaway harness therefore records every call `bench 13` makes into the
evaluation: pushes, pops, null pushes, evaluations, uncertainty requests and clears,
with the position key at each evaluation. It then replays that exact sequence against
each net and subtracts the same walk with no evaluation. Both nets pay for identical
work in the engine's real mix: 6.94M pushes, 4.68M evaluations, 3.70M uncertainty
requests, 988K of them at nodes whose value came from the transposition table. The
replay reproduces the recording's keys and values with 0 mismatches. The nets are
interleaved per round, taking the minimum of 3-5 rounds each; run-to-run noise is
about 3%.

| inference over one `bench 13` | accumulators | trunk and heads | total | vs flat |
|---|---|---|---|---|
| flat gen-5 `f2886d3e2c71` | 569 ms | 312 ms | 881 ms | 1.00x |
| gen-6-3 at HEAD | 569 ms | 1945 ms | 2514 ms | **2.85x** |
| gen-6-3, this entry | 583 ms | 1199 ms | 1782 ms | **2.00x** |
| gen-6-pw, epoch 3, this entry | 605 ms | 715 ms | 1320 ms | **1.43x** |
| **gen-6-pw, final** | 595 ms | 708 ms | 1304 ms | **1.43x** |

The final row and a gen-6-3 control were measured in the same session, five rounds
each, at 1.426x and 2.153x; gen-6-3 read 2.00x in an earlier session, which is the
size of the between-session drift. The accumulators are the same work for every net,
about 575-600 ms. Everything a
stacked net costs beyond a flat one is in the second column.

#### Exact changes: gen-6-3 from 2.85x to 2.00x

All of these are in `src/nnue.c`, verified by `nnue verify` at 10000/10000 on the AVX2
and scalar builds, and by unchanged node counts.

1. **Sparse L1.** 46% of the SCReLU activation's pairs are nonzero (27% of its
   units), so L1 walks only those pairs:
   - The activation lists them as it goes (`cmpgt` then `movemask`, a 2 KB lookup
     table, `popcount`), with no branch per pair.
   - L1's weights are re-laid out pair-major at load, `[bucket][pair][unit][2]`, so
     each pair's weights for all 16 units are one cache line.
   - Requantisation and packing happen in registers, which retires the `hadd`
     reductions and the per-unit scalar requantise.
2. **SCReLU as one `mulhi_epu16`.** `(x << k1) * (x << k2) >> 16` with
   `k1 + k2 = 16 - log2(qa)` is `x*x >> log2(qa)` exactly, in one multiply rather
   than two multiplies and three shifts.
3. **A constant stride for 16-unit L1.** The per-pair address becomes a shift rather
   than an `imul`.
4. **The stack's outputs are recorded.**
   - The first head asked for computes both, and stores them in a record per
     accumulator level and in a 2^18-entry direct-mapped table per thread (4 MB).
     Both are keyed on the full 64-bit key.
   - 27% of the trunks `bench 13` built repeated a position already evaluated in the
     same search, mostly nodes whose value came from the transposition table and
     whose uncertainty did not. 24% of builds now hit the table.
   - A generation counter makes the per-search clear O(1).
5. **`eval_state_push()` prefetches the line** the evaluation will probe. Without it
   the 2^18 table measured worse than a 2^16 one, because its extra hits cost more in
   misses than they saved. With it, the 2^18 table is better.

The debug build asserts every recorded value and uncertainty against a from-scratch
computation, and ran `bench 10` clean.

`bench 13` on gen-6-3, 8 interleaved rounds each, 7135323 nodes every run:

| | max nps | median nps |
|---|---|---|
| HEAD | 1,597,341 | 1,591,108 |
| **this entry** | **1,882,670** | **1,872,788** |

That is **+17.9% / +17.7%**.

#### Why SCReLU stops at 2.00x

The first version of this entry blamed the multiply port. That was wrong. Measured on
this machine, `vpmaddwd` issues **two per cycle**, `vpmulhuw` one, and `vpermd` one
every 1.35 cycles. What bounds the SCReLU trunk is two things:

- **Instructions and loads per pair.** The hot sparse loop runs at about 2.3 cycles a
  pair against a floor of 1.
- **Its weights arriving from L2.** Timed with its inputs hot, a trunk costs 199 ns.
  Across 1000 varied positions it costs 258 ns, with 16 KB of unrelated traffic
  between calls 293 ns, and with 32 KB 348 ns.

A bucket's L1 is 32 KB, the size of the whole L1 data cache, and it cannot stay
resident beside the accumulators.

#### The pairwise architecture: gen-6-pw

No exact change moves SCReLU past 2x, so the remaining lever was the architecture.
Pairwise multiplication is NNUE.md's first listed follow-up, and in this format it is
a new activation tag, `2`:
- Each perspective's accumulator is split in half, and the clamped halves are
  multiplied, `(x * y) >> log2(qa)`, into the same [0, qa] range SCReLU lands in.
- L1 therefore reads 512 inputs rather than 1024, and a bucket's L1 is 16 KB rather
  than 32.

| file | change |
|---|---|
| `trainer/nnue/model.py`, `train.py` | `--pairwise`: L1's input width, `activate()` per perspective, the checkpoint round-trip |
| `tools/export_net.py` | the integer activation in `stack_trunk()`, L1's width, the header tag |
| `src/nnue.c`, `src/nnue.h` | validation (stacked only, hidden a multiple of 32), payload size, `hot->inputs`, and the activation taking two input pointers - the same one twice for SCReLU, which compiles to exactly the old code |
| `trainer/tests/test_stack.py` | five new tests, among them float-vs-quantised tracking for pairwise |

**A pairwise net runs L1 dense.** A product of two clamped units is nonzero far more
often than one squared unit. Pair density was 71% at epoch 1, 63% at epoch 3 and 61%
on the final net, and at that density skipping zeros costs more than
multiplying them. The activation writes no list, and L1 is a dense pass over the same
pair-major layout:
- On the epoch-1 net, dense measured 1.553/1.554 against 1.68-1.76 sparse.
- On the epoch-3 net, 1.496/1.509 against 1.58-1.62.
- On the final net, 1.437/1.465 against 1.53-1.60.

**The dense loop is bound by loads, not multiplies.** Each pair needs two 32-byte
weight loads, and broadcasting its inputs from memory made a third. So four pairs'
inputs arrive in one 16-byte load copied to both lanes, and `vpshufd` splits out each
pair. That took the layer from 90 to 83 ns hot and the replay from 1.46-1.48 to
**1.42-1.43**. The four products are summed as a tree before they meet the
accumulator.

Gates:
- `nnue verify` is exact on pairwise nets on the AVX2 and scalar builds.
- `make trainer-test` gives 188 passed.
- `make smp-test` gives 11/0.
- The debug build's pairwise bench is assert-clean.

gen-6-3's bench node count is unchanged through all of it.

The training run uses gen-6-3's exact recipe with `--pairwise`: gen-006, 24 epochs of
500M plus one finishing epoch, lr 5e-4 with gamma 0.87, lambda 0.95.

| val loss | gen-6-3 (SCReLU) | gen-6-pw |
|---|---|---|
| epoch 1 | 0.003647 | 0.003745 |
| epoch 3 | 0.003501 | 0.003527 |
| final | 0.003299 | 0.003320 |

The final net quantises with 3.22 cp mean drift against the float model (gen-6-3:
3.38 on the same positions). Its uncertainty head predicts a mean error of 78.7 cp
(median 67), against gen-6-3's 77.4 (median 62), so `unc_scale()` is centred close
enough that the SPRT does not wait on `make unc-probe`. Its sanity table matches
gen-6-3's line for line in shape.

**Lower loss is not Elo.** Whether gen-6-pw plays as well as gen-6-3 is an SPRT's
question, and it decides whether the speed is worth having. The search's margins were
SPSA-fitted against gen-5, as they were for gen-6-3.

#### Measured, and not kept

| change | result |
|---|---|
| compacting the pairs' values and offsets in the activation, so sparse L1 skips a load and shift per pair | L1 30 ns faster hot, activation 22 ns slower (a permute and a wider store); **+270 ms** on the replay |
| prefetching the first half's weight lines while the second half activates | **+100-200 ms** |
| L2 specialised to a constant 16 -> 32 shape, always inlined | 3-7 ns slower per trunk |
| L2 on the pairwise L1's load-saving kernel | 1.472 vs 1.425 on pairwise, twice |
| four accumulators in the dense loop | GCC re-associates them back into one chain, as its asm shows; replaced by the tree |
| moving the trunk's 12 KB scratch off the stack (it costs a `___chkstk_ms` per call) | neutral |
| permuting FT units so zeros share pairs | ceiling 43.5% nonzero pairs vs 46.4%, about 7 ns; not built |
| updating L1 incrementally from the parent's sums | impossible: FT rows are 1.6% zeros, so a quiet move changes every pair |
| int8 L1 weights (export at shift 6, clamp at 127; sign-extended on load), sparse and dense | about 1%, within noise, for 10.2 cp of quantisation drift against 3.4; removed |

---

### E41: The pairwise net measured - gen-6-pw adopted, gen-6-pw-big rejected

**Date** 2026-09-22 · **Nets** `48428bbbd57e` (gen-6-pw, `512x2 -> 16 -> 32 -> 8
+unc`, pairwise, bench 291947) and `e6bfc4517795` (gen-6-pw-big, the same recipe at
`--hidden 1024`, bench 234418) · **Baseline** `stormbreaker-base.exe`, gen-5
`f2886d3e2c71`, bench 242977 · **Status** gen-6-pw **passed STC and LTC**;
gen-6-pw-big is not adopted.

E40 built the pairwise architecture, measured it at 1.43x a flat evaluation, and left
its strength as "an SPRT's question". This is that SPRT. It also re-asks E27's
question - whether a wider net pays - now that the L1 stack rather than the
accumulator is where the capacity lives.

#### The runs

Every dev binary was built from `eafa8c3` and differs from the others **only** in
`-DNNUE_EVALFILE`. Conditions throughout: 1 thread, 16 MB hash,
`UHO_Lichess_4852_v1.epd`, concurrency 14, `model=normalized`, alpha = beta = 0.05.
**Zero time losses and no engine anomalies in any of the five runs**, 11,646 games.

| # | dev | vs | TC, bounds | games | Elo | nElo | LLR | verdict |
|---|---|---|---|---|---|---|---|---|
| 1 | gen-6-pw | base | STC [0, 5] | 1128 | **+43.66 ± 12.90** | +69.40 ± 20.28 | 2.96 | **H1 accepted** |
| 2 | gen-6-pw-big | base | STC [0, 5] | 1078 | **+45.38 ± 13.23** | +71.98 ± 20.74 | 2.96 | **H1 accepted** |
| 3 | gen-5 net (control) | base | STC [0, 5] | 5000 | **+1.25 ± 5.09** | +2.37 ± 9.63 | -0.03 | truncated, see below |
| 4 | gen-6-pw-big | gen-6-pw | STC [0, 5] | 3224 | **-3.23 ± 7.21** | -5.38 ± 11.99 | -1.05 | stopped by hand |
| 5 | gen-6-pw | base | **LTC [0.5, 4.5]** | 1216 | **+43.08 ± 10.53** | +80.70 ± 19.53 | 2.94 | **H1 accepted** |

| # | W-L-D | Ptnml(0-2) | draw % | pairs | wall | PGN |
|---|---|---|---|---|---|---|
| 1 | 382-241-505 (56.25%) | [14, 88, 243, 181, 38] | 43.09 | 2.15 | 35m42 | `20260922-093224-STC.pgn` |
| 2 | 370-230-478 (56.49%) | [10, 93, 220, 179, 37] | 40.82 | 2.10 | 34m59 | `20260922-100808-STC.pgn` |
| 3 | 1294-1276-2430 (50.18%) | [43, 522, 1346, 552, 37] | 53.84 | 1.04 | 2h37m | `20260922-104307-STC.pgn` |
| 4 | 866-896-1462 (49.53%) | [53, 393, 743, 377, 46] | 46.09 | 0.95 | 1h44m | `20260922-132040-STC.pgn` |
| 5 | 401-251-564 (56.17%) | [3, 93, 276, 223, 13] | 45.39 | 2.46 | 3h20m | `20260922-150520-LTC.pgn` |

Logs and the run manifest: `external/games/sprt-20260922-gen6/`. The manifest is not
decoration - `sprt.py` names every dev seat `dev`, so the PGN alone cannot say which
net played it. That is the attribution E27 had to caveat after the fact, and
`queue.json` records dev, net hash, bench, bounds, verdict and PGN per run.

#### The control is the row the other four depend on

Run 3 is the same binary build as runs 1 and 2 carrying **base's own net**, played
against base. It reproduces base's node counts exactly - 242977 at depth 7, 5819071 at
depth 13 - so it makes identical search decisions and the only thing it can measure is
the speed E40's `nnue.c` work bought or cost on a flat net. It measured **+1.25 ±
5.09** over 5000 games.

That is what licenses reading runs 1 and 2 as the nets. Without it, +43 Elo against a
baseline built before E40 is a claim about a net *and* 824 changed lines of inference
code at once, which is two changes in one test.

Its 2500-pair cap was declared before it started. An SPRT on [0, 5] whose true value
is 0 expects roughly 25,000 games before a boundary, and the control's deliverable is
the interval, not a verdict - so it was truncated on purpose and reports
"inconclusive", which is the correct output of a truncated SPRT and not a failure.

#### gen-6-pw: the two time controls agree

**STC +43.66 ± 12.90, LTC +43.08 ± 10.53.** A five-fold increase in time control left
the point estimate within 0.6 Elo and the intervals almost coincident. Gains usually
shrink at LTC as the baseline's extra depth finds what the better evaluation was
seeing earlier; this one did not, which is the strongest evidence in the entry that
the net is better rather than better-suited to shallow search. LOS 100% in both.

#### gen-6-pw-big: wider, slower, no better

Exported and gated the same way: `24576 -> 1024x2 -> 16 -> 32 -> 8`, pairwise,
halfka-32sq, +uncertainty; `nnue verify` **10000/10000 exact**; 3.46 cp mean
quantisation drift against the float model (gen-6-pw: 3.22).

Three readings, none of them favourable:

1. **Against base it is indistinguishable from gen-6-pw** - +45.38 ± 13.23 against
   +43.66 ± 12.90. A 1.7 Elo lead with intervals four times that wide is not a lead.
2. **Head to head it is behind**, -3.23 ± 7.21 over 3224 games.
3. **It is markedly slower.** Its accumulator is doubled; `bench` reads 1.15M nps
   against gen-6-pw's 1.54M at depth 7. Read that as indicative only - the two nets
   search different trees, which is exactly why E40 built the replay harness - but the
   direction is not in doubt, and the head-to-head charged it for that cost at equal
   time.

This is E27's answer again through a different architecture: **coverage, not capacity,
is the binding constraint**. E27's h1024 on the gen-5 corpus measured -7.0 ± 11.3, and
moving the extra width behind a pairwise L1 did not change the verdict.

**Two honest limits on that conclusion.** Run 4 was stopped by hand at 3224 games to
free the machine for the LTC, so it is a standing and not a verdict; `queue.json`
records it as killed, and `sprt.py`'s "inconclusive" line for it is an artifact of the
kill. And gen-6-pw-big's uncertainty head predicts a mean error of **90.1 cp** (median
71) against gen-6-pw's 78.7 (67) and gen-6-3's 77.4 (62), while `UncSigma*` has never
been re-centred for a gen-6 net - so it played with pruning margins running wider than
the constants assume. That confound cannot be excluded from its result. It was not
treated as a blocker because E27's re-centring attempt on gen-5 measured -7.4 ± 20.0,
i.e. re-centring did not convert into Elo there either; clearing gen-6-pw-big properly
would need the re-centring first and then run 4 to a verdict.

---

### E42: One night's search batch - node-share time management and four search changes, +29 at STC

**Date** 2026-09-26 · **Net** `48428bbbd57e` (gen-6-pw) · **Baseline** HEAD `7bd499e`, bench
250106 (d7) / 6694801 (d13) · **Final bench** 289292 / 6514462 · **Status** time management
**passed STC on its own** (+11.32 ± 6.02); the combined build **passed STC** against HEAD
(+32.16 ± 10.63, H1) and measured **+28.90 ± 6.98 over 3000 fixed-length STC games**. The
four search changes are attributed only by VSTC screens - see "What this does not show".

The brief was a fixed budget - one night of machine time - for at least 30 Elo, proven at
STC. That shaped the method more than anything else here, and the method is the thing to
read before the numbers.

#### Method

- **Time management was confident and tree-neutral**, so it went straight to an STC SPRT.
- **Every search change was screened at VSTC** (2+0.02) against HEAD, one change per
  test, before any STC time was spent on it. The first two screens used [0, 5]; the rest
  [0, 8] with a 3000-game cap, because at ~22 minutes a screen the queue would otherwise
  not have fitted the night. A screen *selects*; it does not certify.
- **The survivors were combined into one build and that build was SPRT'd at STC against
  HEAD.** That test is the proof. The screens are what chose its contents.
- **Then a fixed-length match of the same pairing**, because an SPRT that stops on H1
  early overestimates: run 8 stopped at 1376 games on +32.16, and the unbiased 3000-game
  run put the same build at +28.90. `gauntlet.py` with a single opponent plays exactly the
  SPRT's conditions with no stopping rule.
- Every candidate was a script of asserted text replacements against HEAD, so each could
  be built alone for its screen and the survivors stacked onto one tree. The scripted
  stack and the tree that ships bench identically (289292 / 6514462).

Conditions throughout: 1 thread, 16 MB, `UHO_Lichess_4852_v1.epd`, concurrency 14,
`model=normalized`, alpha = beta = 0.05. Logs and the run manifest are in
`external/games/sprt-20260926-night/`; `queue.json` names the binary, bench and PGN of
every run, since `sprt.py` calls every dev seat `dev`. Its ids differ from the numbering
here, which drops the two held screens.

#### The runs

| # | change | vs | TC, bounds | games | Elo | LLR | verdict |
|---|---|---|---|---|---|---|---|
| 1 | node-share time management | HEAD | STC [0, 5] | 4082 | **+11.32 ± 6.02** | 2.96 | **H1** |
| 2 | double extensions | HEAD | VSTC [0, 5] | 2370 | **+14.23 ± 8.69** | 2.96 | **H1** |
| 3 | threat-indexed main history | HEAD | VSTC [0, 8] | 3000 | +6.83 ± 7.86 | 1.36 | capped |
| 4 | table score as the pruning eval | HEAD | VSTC [0, 8] | 824 | **-33.41 ± 15.43** | -2.97 | **H0** |
| 5 | quiet checks exempt from LMP and futility | HEAD | VSTC [0, 8] | 3000 | +8.11 ± 8.22 | 1.64 | capped |
| 6 | cutoff-count LMR | HEAD | VSTC [0, 8] | 3000 | +9.04 ± 7.87 | 2.04 | capped |
| 7 | fail-low prior-move credit | HEAD | VSTC [0, 8] | 3000 | -2.43 ± 7.70 | -1.58 | capped, dropped |
| 8 | **1 + 2 + 3 + 5 + 6** | HEAD | **STC [0, 5]** | 1376 | **+32.16 ± 10.63** | 2.95 | **H1** |
| 9 | small batch (five, below) | HEAD | VSTC [0, 8] | 2718 | +13.04 ± 8.29 | 2.98 | H1 |
| 10 | **8, fixed length** | HEAD | STC, none | 3000 | **+28.90 ± 6.98** | - | - |
| 11 | 8 + 9, fixed length | HEAD | STC, none | 3000 | +32.17 ± 7.06 | - | - |
| 12 | 9 on top of 8 | 8 | STC [0, 5] | running | - | - | see below |

| # | W-L-D | Ptnml(0-2) | PGN |
|---|---|---|---|
| 1 | 1100-967-2015 | [29, 454, 960, 551, 47] | `20260926-005557-STC.pgn` |
| 2 | 704-607-1059 | [34, 258, 521, 321, 51] | `20260926-030701-VSTC.pgn` |
| 3 | 848-789-1363 | [46, 362, 649, 373, 70] | `20260926-032506-VSTC.pgn` |
| 4 | 197-276-351 | [30, 115, 184, 70, 13] | `20260926-034755-VSTC.pgn` |
| 5 | 870-800-1330 | [62, 355, 615, 387, 81] | `20260926-035406-VSTC.pgn` |
| 6 | 867-789-1344 | [58, 328, 653, 400, 61] | `20260926-041651-VSTC.pgn` |
| 7 | 846-867-1287 | [67, 331, 712, 336, 54] | `20260926-043933-VSTC.pgn` |
| 8 | 434-307-635 | [8, 128, 304, 225, 23] | `20260926-050219-STC.pgn` |
| 9 | 760-658-1300 | [43, 303, 589, 357, 67] | `20260926-054554-VSTC.pgn` |
| 10 | 873-624-1503 | [21, 255, 723, 456, 45] | `20260926-060701-gauntlet.pgn` |
| 11 | 918-641-1441 | [19, 260, 692, 483, 46] | `20260926-074201-gauntlet.pgn` |
| 12 | - | - | `20260926-091612-STC.pgn` |

Two screens were written and never run, held so the proof fitted the night: score-trend
time management (a falling score lengthens the soft target, 80-140%) and one extra ply of
LMR at cut nodes. Both are in the pending list below.

#### What shipped, and what each change is

**1. Node-share time management** (`timeman.c`, `search_root()`). `search_root()` credits
every node it spends to the root move it was spent under, cumulatively over the search and
keyed by from/to so the count survives the root list being re-sorted. At each iteration's
stop check the soft target is scaled by `(1400 - share) * 135 / 1000` percent, the share
being the best move's thousandths of the nodes: 67% when it took 90% of the tree, 135% when
it took 40%. **BASE is 1400 rather than the textbook 1500 on a measurement**: over 60 book
positions at 6+0.08 the share at the stopping check averaged 670, and 1500 would have
lengthened the average think by 11% - a second change riding on the first. Bench identical
at d7 and d13; bench has no clock, and the scale lives behind `timeman_has_clock()`.

**2. Double extensions.** A singular move whose alternatives all fail more than
`DextMargin` (20) below the singular window extends two plies instead of one, off the PV,
at most `DextMax` (6) times per line - `SearchStack.doubleExtensions`, inherited on entry
so null-move and ProbCut children read their own line's count. Both constants have sweep
seats. **It fails the WAC.001 probe** (no mate by depth 16, where HEAD finds it at 13) and
roughly doubles nodes to a fixed depth on two of four probe positions; it passed its screen
at +14 anyway. The probe was measuring tree shape, not strength - see change 5 for why
HEAD's own depth 13 on that position was never a good yardstick.

**3. Threat-indexed main history.** `history[side][from attacked][to attacked][from][to]`,
the attacks of the side not to move computed once per node before the move loop and kept
in `SearchStack.threats`. Every reader and writer goes through `main_hist()`, so the index
cannot be computed two ways. The table grows 16 KB to 64 KB per thread.

**5. Quiet checks exempt from LMP and futility.** Both prunes run before the move is made,
so a quiet check was pruned like any quiet move. **HEAD first finds WAC.001's mate in two
at depth 13** - the mating move is a quiet check at a futile node. With the exemption it is
found at depth 3. `gives_check()` decides before the move: pawn and knight direct checks,
then one bishop and one rook lookup from the king with the moved piece placed on its
destination, which covers direct slider checks and discovered ones together. The cost is
+24% nodes at d13. Exempting from futility alone found nothing by depth 14; a SEE-safe
filter on the LMP exemption changed d7 by 35 nodes and was not pursued.

**6. Cutoff-count LMR.** `SearchStack.cutoffCnt` counts beta cutoffs at a ply since the
grandparent was entered (Stockfish's scheme); a move's reduction grows by one when its
siblings' children have cut more than three times. `stack` is `MAX_PLY + 2` so the
grandchild clear at the deepest ply stays in bounds.

Gates on the combined build: bench deterministic, `smp selftest` 11/0, `movepick selftest`
14/0, and an assertion build of every candidate stacked ran `bench 10` clean. Movegen is
untouched, so perft is not in play.

#### Rejected

**The table score as the pruning eval, -33.41 ± 15.43.** Stockfish lets RFP, razoring and
null move read `ttValue` where its bound points past the static evaluation. Here it lost
heavily and quickly. The likeliest reading: every margin those three use was SPSA-fitted,
under `unc_scale()`, against the corrected *static* evaluation, and a stored score -
often a depth-0 quiescence bound - is a different distribution. Not worth re-trying without
refitting the margins against it.

**Fail-low prior-move credit, -2.43 ± 7.70** (ROAD_TO_3600 change 4). Written first as an
unconditional credit, which grew bench 16% at both depths; then weighted as Stockfish does
- by depth, node type and how late the move sat in its list - which is what was screened.
Neutral to negative. The parent already credits the same move whenever it cuts, which is
most of the time.

#### The small batch - screened, not shipped

Five small refinements, each expected to be worth a few Elo and so each unmeasurable alone
in the time available, were screened as one: RFP and quiescence stand-pat returning
`(value + beta) / 2`; history credit for a quiet table move on a table cutoff; each
aspiration fail high re-searching one ply shallower (Stockfish's `failedHighCnt`); and the
search's evaluation damped by `(200 - halfmoveClock) / 200` in `corrected_eval()` - there,
not in the evaluation, so datagen labels, the tuner and `nnue-test` are untouched.

It passed its VSTC screen at +13.04 ± 8.29, but at STC the evidence is thin: runs 10 and 11
differ by +3.3 ± ~10, and run 12 is its own SPRT. That SPRT was still running when this entry was written; its `queue.json` record is the verdict, and it belongs here when it lands. It is **not in the tree**;
`external/patches/e42-small-batch.patch` applies cleanly on top of it, and the build it
makes benches 275098 / 6234144.

#### Time losses, and a clock that runs too low

STC forfeits: run 1 dev 1, HEAD 0; run 8 dev 4, HEAD 1; run 10 none; run 11 dev 4, HEAD 0.
Nine to one is not chance. The VSTC screens were even at dev 15, HEAD 10.

Every forfeiting move overran its own hard ceiling: rebuilding the clocks from the PGNs'
move times, the flagging side held 0.1-0.3 s and took 0.18-0.35 s, where `maximum` at that
clock is under half of it. That is a process that did not answer in time, not an
allocation. But the reason a hiccup is fatal is the finding worth keeping: **at 8+0.08 both
engines' clocks run far lower than the time manager intends.** Over the 3000 games of run
10, HEAD fell below 0.2 s in 371 of them, 12%. The dev builds do so somewhat more often
(456 in the same run), which is where the nine-to-one comes from.

That is a pre-existing property of `timeman.c`, not of this batch. The soft target is only
checked between iterations, an iteration started just under it can run on to `maximum`,
and `maximum` is half of whatever is left - so a run of unsettled moves halves the clock
each time. The remedy is its own test (a floor on the bank, or a mid-iteration check of
the soft target), and forfeits count against whoever flags, so every dev result in this
entry is biased down by them, not up.

#### What this does not show

- **No change except time management holds its own STC SPRT.** Double extensions passed a
  VSTC screen; threat history, quiet checks and cutoff-count ran to the screen's cap at
  LOS of roughly 95-99% each. The proof is runs 8 and 10, the sum. Ablating each out of the
  final build at STC is the pending work that attributes it.
- **The parts do not add up, and should not be expected to.** Their estimates total about
  +49; the build measures +29. Changes that each reshape the same tree overlap, and VSTC and
  STC Elo are not one scale.
- **STC only.** LTC confirmation of the whole batch is owed, as it is for E20-E22a and E29.
- **The new constants are chosen, not fitted.** `DextMargin`, `DextMax`,
  `NODE_SHARE_BASE`/`SCALE` and `CUTOFF_CNT_THRESHOLD` are defensible defaults. The first
  two have sweep seats; the time-management pair are `timeman.c` constants.

#### Pending

| change | why |
|---|---|
| the time manager's clock floor (above) | forfeits, and shallow endgames |
| ablate each of the four search changes out of the final build, STC | attribution |
| LTC confirmation of the batch | STC-only |
| the small batch, run 12 | not yet decided at STC |
| score-trend time management | held for time |
| cut-node LMR +1 (E6 measured +2 at about -16) | held for time |
| SPSA seats: `DextMargin`, `DextMax` | defaults are chosen |

---

### E43: Reverse futility on its own uncertainty curve

**Date** 2026-09-27 · **Net** `48428bbbd57e` (gen-6-pw) · **Baseline** HEAD `874d4b2`, bench
289292 (d7) / 6514462 (d13) · **Dev bench** 279977 / 6886996 · **Status** stopped by hand at
3194 games, **-0.33 ± 7.03**, inconclusive; code removed.

NNUE.md 5c's `probe err` found the shared mapping the wrong *shape* on gen-5, and the
per-site `UNC_W_*` weights that followed can only rescale a shape. This tested the shape
itself, on reverse futility alone: its own curve `max(floor, min(σ * slope / 16, cap))`,
with razoring, futility, ProbCut and delta left on the shared mapping.

#### The measurement it came from

`probe err` at d12 on the shipped net, scaling neutral, 700,916 paired nodes. The 1%
downside error each band showed, in percent of the whole tree's 1% margin (382 cp), against
the shared mapping `min(99 + σ * 9 / 16, 145)`:

| σ band | nodes | needs | shared mapping |
|---|---|---|---|
| 0-63 | 55% | 9-34 | 99-129 |
| 64-127 | 30% | 57-58 | 142-145 |
| 128-191 | 10% | 109 | 145 |
| 192+ | 5% | 231-444 | 145 |

`probe unc` on the same tree: the mapping holds every node between 115 and 145, with 34% at
the cap. The 99/9 constants are the gen-6-pw `unc-probe -ref` re-centring, not an SPSA fit.

#### The curve, and how it was centred

Shape from the table: a floor near 30, about 0.75 per centipawn of σ above σ ~40, a plateau
near 330. One scale factor was then solved so that the node-weighted mean over the nodes
where RFP actually consults the head equals what the shipped mapping gave those same nodes:
129, over 2,182,422 nodes at bench d13. The nodes were recorded by a throwaway build whose
bench reproduced the baseline's, so the population is the baseline's own tree. Centring on
100 instead would have added a 23% global tightening to the shape change, which is two
changes. The result was `max(61, min(σ * 24 / 16, 667))`: tighter than shipped on 74% of
those nodes, wider on 26%, and 39% at the floor.

Because the head is a whole inference, the node cache was changed to hold σ rather than a
scale. That refactor alone reproduced both bench counts and the neutral probe exactly
before the curve went in. Gates: perft (all four suites), smp-test 11/0, openbench-check,
movepick-test 14/0, chess960-test, and the debug bench and d16 searches assertion-clean.
The classical bench was unchanged at 274441, since a net without the head keeps the shared
mapping.

#### The run

| STC 8+0.08, 1 thread, 16 MB, UHO, normalized [0, 5] | |
|---|---|
| Games | 3194, stopped by hand at 1h40m |
| **Elo / nElo** | **-0.33 ± 7.03** / -0.56 ± 12.07 |
| LLR / LOS | -0.40 / 46.4% |
| Ptnml | [40, 385, 740, 390, 36], draw 46.5%, pairs 1.00 |
| PGN | `20260927-125032-STC.pgn` |

The panel's W/L/D (794-797-1591) sums to 3182 against its own count of 3194. The PGN holds
3194 games, dev 798-800-1596, all normal terminations and **no time losses**. The Elo and
LLR above are the panel's.

It was stopped rather than run to a boundary because the question was whether this is a
30-50 Elo idea. The interval's upper end, +6.7, already answers that, and a [0, 5] test
whose true value is near zero needs on the order of 25,000 games to decide (E41). This is
not an H0 acceptance.

#### What it says

**The 1% quantile is not the margin the search wants.** The curve fired - bench moved -3.2%
at d7 and +5.7% at d13 - and tighter margins on the confident three quarters and wider ones
on the uncertain quarter came out even. Two readings, not separated here: the quantile was
always a proxy for the optimal margin (NNUE.md 5c says so), and null move and futility
catch much of what RFP stops pruning at the uncertain nodes. This is E27's re-centring
lesson from the other side: the probe describes the error, but it does not choose the
margin.

**It closes the search side of the head for large gains.** Margin scaling banked about +47
(E20, E21). Since then: re-centring -7.4, sigma-scaled LMR -29.9, sigma-weighted history
-1.4, and this, about 0. An SPSA over the three curve seats might find a few Elo and was not
judged worth the machine time. What would reopen it is a shape fitted by games rather than
by the probe.

---

### E44: Perturbations into uncertainty, and a gen-6 val set

**Date** 2026-09-27 to 09-29 · **Net** `48428bbbd57e` (gen-6-pw) · **Engine** `884e652`,
search identical to `874d4b2` · **Status** built and gated; no net trained on it yet, so
**no Elo claim**. The design and every table are in [NNUE.md](NNUE.md), "Directing a
generation at the net's uncertainty"; this entry records what was measured and what it
licenses.

E43 closed the search side of the uncertainty head for large gains. This spends it where
E18, E27 and E41 say the constraint is - coverage - by deciding where a generation's
perturbations send its games.

#### A gen-6 validation set

`external/data/val-006.cnn`: **1,969,285 records** from 20,000 games at gen-6's exact
settings (no book, `-opening 8-21`, `-dfrc 10`, `-dfrcopening 2-5`, `-openingscore 300`,
3-4-5 tables, 10,000 nodes, 64 MB), seed 900000001, built from a clean worktree so every
manifest reads `884e652`. Shuffled with cross-worker dedup (23,227 dropped); `verify` passes.
It replaces a gen-5 set labelled by a weaker engine, which every gen-6 run so far has been
validated against. Unperturbed on purpose: it is the distribution the engine plays.

#### Does the head rank the error that the loss sees?

Scored on 983,040 `val-006` records with the gen-6-pw checkpoint: the signal (sigma weighted
by `4p(1-p)` at the label) has Spearman **0.572** with the real win-probability error, against
0.252 for `4p(1-p)` alone. Its top decile carries **2.17x** the mean error and its bottom
0.07x. Raw sigma is flat past its fifth decile, because decided positions have large
centipawn errors and almost no win-probability error. Among balanced positions raw sigma
still sorts the error at 0.494.

#### `-randompick uncertain`: what a perturbation plays

`-random` fires as before; this changes the move. The four quiet, non-checking alternatives
whose resulting positions have the highest signal - above the search's own move's - go to a
`searchmoves` search at the game's node budget, and its choice is played if it concedes at
most `-randommargin`; otherwise the search's move stands. Per perturbation played, both modes
report the cost by the next search and the signal of the position it landed in. gen-6's
settings, 1,600 games a row on one seed:

| | played per game | cost each | lands at signal | run signal |
|---|---|---|---|---|
| none | 0 | - | - | 37.6 |
| uniform, `-random 32` | 1.44 | 500cp | 34.1 | 33.1 |
| uncertain, `-random 8`, 100cp | 1.04 | 57cp | 51.7 | 38.7 |
| uncertain, `-random 8`, 200cp | 1.95 | 107cp | 52.5 | 37.1 |

A uniform move lands somewhere more certain than the plain game line and shortens games (87
quiet positions against 100); an uncertain pick lands near the line's 75th percentile at a
tenth of the cost, and `-random 8` at 100cp still writes **97.1 records per game**. `uniform`
is the default and is byte-identical to HEAD's `-random` on three command lines (plain,
`-random 4`, and with DFRC and tablebases), policy sidecars included.

#### Built and removed on the way

**The same pick as a `-steer` flag,** judged on the mean signal of every position written -
+1.6% at 2.4 moves a game - and removed. That was the wrong comparison: a perturbation
replaces a random move, not nothing, so the baseline is the uniform row, and the gain is in
where the game goes and how it resolves from there.

**`-focus`, a filter writing only positions above a signal threshold.** A step at 40 kept
46.8% of positions with x1.40 the real error per record (a proportional ramp plateaued at
x1.35) and cut decisive scores from 7.5% to 2.0%. Removed by choice, not on a measurement:
every position it dropped had already cost its search, and the extension is to be trained on
all of them.

#### What this does not show

That a net trained on this data is stronger. That is the gen-6 extension's SPRT: the same
recipe on gen-006 plus an extension generated with uncertain perturbations, against
gen-6-pw, both validated on val-006. gen-006 had no perturbations at all, so that SPRT prices
"perturbed games" and "perturbed toward uncertainty" together; separating them would need a
uniform-perturbation extension of the same size.

---

### E45: The two heads as a probability - z-scored children, a WDL model, and what reached STC

**Date** 2026-09-29 to 10-01, two nights · **Net** `48428bbbd57e` (gen-6-pw) · **Baseline**
`stormbreaker-wdl-base.exe` = HEAD `1ef61c3`, bench 289292 (d7) / 6514462 (d13) · **Status**
**shipped: z-scored reductions, the promising side only, +8.30 +/- 4.28 over 8000 STC games**,
of which a fresh 5000-game confirmation alone measured +7.02 +/- 5.41 (LOS 99.4%). Bench
333802 (d7) / 7285139 (d13). Every other candidate is in `external/patches/e45-wdl-research.patch`
(night 1) and `e45-wdl-research-night2.patch`; the shipped change alone is `e45-zlessdeep.patch`.

The brief was a night and a day of machine time for 30-60 Elo at STC, from the uncertainty head
used as a win/draw/loss signal rather than only as a margin scale. Since E21 the head has done
one job - widening and tightening five pruning margins at the NODE, about +47 at STC over E20
and E21 - and every attempt to use the node's sigma for anything else measured neutral or
negative: re-centring and sigma-LMR (E27), RFP's own curve (E43), sigma-weighted history (E30).
This entry asked what the value and uncertainty heads say TOGETHER, and where that is
information the search does not already have.

**The short answer.** They say a great deal - the tables below are the cleanest signal this
search has been offered - and the first night's form of it did not survive the trip from VSTC
to STC: z-scored late-move reductions measured +11.35 +/- 7.85 at VSTC and +3.47 +/- 7.07 at
STC. The second night ablated it at STC and found why: z's value at STC is entirely on the side
of searching PROMISING moves deeper, and cutting hopeless ones harder costs a little. Built in
that direction it is a confirmed +8 at STC - short of the 30-60 the brief asked for.

#### Method

Measure first, build second. A stats build (`-DWDL_STATS`, in the research patch) instruments
the d13 bench tree; its node count reproduces the baseline's to the node, because it only READS
evaluations nnue.c has already cached. A candidate was built only where a table said the signal
separated something the search decides. Each candidate was an `#ifdef` in `src/search.c`, built
with `CFLAGS=-D...` in the ENVIRONMENT - given on the command line it replaces the Makefile's
own flags and drops `-Isrc`. Screens at VSTC 2+0.02, [0, 5], 3000-game cap; then STC 8+0.08.
1 thread, 16 MB, UHO_Lichess_4852_v1, concurrency 14 throughout. Queue, logs, binaries and a
run-by-run log: `external/games/sprt-20260929-wdl/` (`NOTES.md`).

#### The number: z

For a move m at a node with bound alpha, after making it:

    z = (-v_child - alpha) / max(sigma_child, 16)

v_child the child's corrected static evaluation, sigma_child the head's predicted error AT THE
CHILD: how many of its own error scales the move sits below the bound. P(m beats alpha) is
monotone in it. It costs nothing - the child evaluates itself on entry, and nnue.c's recorded
stack outputs hand the parent the same (value, sigma) pair.

#### What the tables said (d13 bench, baseline tree unless noted)

**Reduced quiets** (depth >= 3, move > 2), how often the move's search ended above alpha:

| z | share of reduced moves | beat alpha |
|---|---|---|
| <= -4 | ~29% | 0.00% |
| -4 .. -2 | ~24% | 0.02 - 0.10% |
| -2 .. 0 | ~30% | 0.2 - 2% |
| > 0 | ~7% | 4.5 - 14% |

Both heads are needed. At the same gap of -128 .. -64cp below alpha the rate runs 0.02%
(sigma < 32), 0.18%, 0.37%, 1.38%, 1.90%, 3.01% (sigma 512+) - a hundredfold spread the value
head cannot see. The ratio of child to parent sigma alone was weak and non-monotone; the child's
evaluation alone was strong; z was the cleanest of the three.

**Where z is not the signal:**

| population | z <= -3 beats alpha | reading |
|---|---|---|
| late quiets, non-PV, depth 1-10 | < 0.1% at every depth | the clean case |
| captures | 0 - 0.3%, small samples | same shape - but see run 8 |
| checking moves | 1 - 7% at depth 1-3 | an in-check child's evaluation is not trusted |
| evasions, move > 2 | ~0% | the child is quiet again |
| second moves (killers) | 0.3 - 1.5% | ordering evidence outranks the static verdict |

z orders nothing. At quiet cutoffs with k failed quiets before the cutter, ordering by z would
have left 0.506 of them ahead at k = 1 (chance: 0.5) and 0.88 at k = 2 (chance: 1.0). It says
which moves cannot matter, not which one is best.

**Node sigma is not the move's sigma.** Moves LMP would have pruned, searched anyway: 0.41% beat
alpha overall, and 0.3 - 0.9% at depth 1 in EVERY node-sigma band. Nothing to scale.

**Null move.** z of the null child against beta: below -1 the null search succeeded under 2% at
depth 3-4 and under 5% at 5-7, on 45% of the attempts there; break-even near -2 at depth 8-11.

**Aspiration.** Root score swing between iterations, by root sigma: median 4cp (p90 16) under
32, median 48cp (p90 514) over 256.

**Inside the z-LMR tree**: z <= -4 now reduced 3.9 plies on average and still 0.00% beating
alpha; z >= +0.25 beating it 3-10% at r ~ 1.9 - which is what the outer tiers of run 11 were.

**The child's table entry** adds little: where it already bounds the move under alpha at the
reduced depth the child cuts on it anyway, and the case it would add - an entry saying the move
beats alpha - is 4k of 1.3M reduced moves.

#### A WDL model from both heads

1.0M val-006 records scored by the gen-6-pw checkpoint, against the game's result, favoured side
W/D/L in percent:

| abs(v) | sigma < 32 | 32-64 | 64-128 | 128-256 | 256+ |
|---|---|---|---|---|---|
| 0-25 | 14/75/12 | 23/58/19 | 23/55/21 | 25/50/25 | - |
| 100-200 | 39/53/8 | 40/51/8 | 39/51/10 | 37/50/13 | 37/51/12 |
| 300-450 | 51/48/1 | 68/31/1 | 68/29/3 | 61/34/6 | 51/43/6 |
| 450-600 | - | 81/19/0 | 82/17/1 | 75/22/3 | 65/30/6 |
| 600-800 | - | 90/10/0 | 92/7/0 | 87/12/1 | 72/24/3 |

p = sigmoid(v/400) cannot tell a dead draw (0cp, quiet sigma, 75% drawn) from an open fight (0cp,
large sigma, 50%); the pair can. The evaluation implied by the actual expected score is ~1.8x
|v| for sigma 40-96 in every band from 100 to 1200cp - monotone compression, invisible to minimax
- and falls with sigma to ~1.0x past 256, which is not.

Time use at STC (E43's 3194-game PGN): 0-50cp took 14.6% of the thinking and was 95% drawn;
>= 600cp took 17% of it and the side ahead never lost.

#### The runs

| # | change | vs | TC | games | W-L-D | Elo | verdict |
|---|---|---|---|---|---|---|---|
| 1 | **z-LMR**: r+1 at z < -2, r+2 at z < -4, r-1 at z >= 0 | base | VSTC | 3000 | 859-761-1380 | **+11.35 +/- 7.85** | capped, LLR 1.92 |
| 2 | z-prune: skip non-PV quiets at depth <= 6, z < -3 | base | VSTC | 3000 | 827-827-1346 | 0.00 +/- 7.98 | capped |
| 8 | captures reduced at z < -3 / -6 | 1 | VSTC | 3000 | 798-803-1399 | -0.58 +/- 7.80 | capped |
| 14 | WDL-eval: static eval x clamp(1 - (sigma-64)/512, 0.5, 1) | base | VSTC | 2406 | 552-669-1185 | **-16.91 +/- 8.56** | **H0** |
| 11 | z-LMR outer tiers: r+3 at z < -6, r-2 at z >= +1 | 1 | VSTC | 3000 | 810-776-1414 | +3.94 +/- 7.70 | capped |
| 12 | evasions reduced at z < -3 / -6 | 1 | VSTC | 3000 | 833-816-1351 | +1.97 +/- 7.70 | capped |
| - | SPSA, 13 seats, 700 x 28 games | - | VSTC | 19600 | - | - | see below |
| 18 | 1 + 11 + 12 + the SPSA output | base | STC | 17716 | 4293-4336-9087 | -0.84 +/- 2.85 | **H0** (uncapped, 9h18m) |
| 19 | precision-weighted corrhist (weight x 64/sigma) | 18 | VSTC | 3000 | 806-809-1385 | -0.35 +/- 7.87 | capped |
| 23 | **z-LMR**, run 1's build | base | STC | 3000 | 777-747-1476 | **+3.47 +/- 7.07** | capped, LLR 0.45 |
| 15 | TM: soft target x outcome variance from the table above, at the root | base | STC | 1600 | 390-388-822 | +0.43 +/- 9.61 | stopped flat |
| 17 | TM: easy move - every alternative's child > 2.5 sigma under the best | base | STC | 3000 | 736-770-1494 | -3.94 +/- 6.66 | capped |

Ptnml: 1 [49, 330, 668, 380, 73]; 18 [153, 2176, 4224, 2171, 134]; 23 [27, 358, 704, 380, 31];
17 [28, 346, 775, 334, 17]. No time losses in run 18, and the two engines' think time by
|score| band matched to the millisecond, so it measured the search and nothing else.

The SPSA left every z seat within 2 of where the tables put it (ZlmrMore 32 -> 31, ZlmrVdeep
96 -> 98, the capture/evasion pair 48/96 -> 49/97; the rest unmoved) and moved only the old LMR
constants: LmrBase 13 -> 14, LmpBase 10 -> 12, LmrHistDivisor 8493 -> 8050, LmrContDivisor
7334 -> 7213.

Built and never played: z-null-move (skip below z -1 / -2, and the reduction re-based on the
null child), root z-LMR, sigma-sized aspiration windows, a widened z-prune, signed leaf-sigma
terms. All are in the research patch.

#### Night 2: ablating z-LMR at STC, and building it the way STC wants

Every run capped (STC 3000 or 5000, VSTC kill-screens 2000), against `stormbreaker-wdl-base.exe`
unless noted; the user's NNUE-data aggregation shared the machine from 22:10, and the only time
losses all night were one per side in one VSTC screen. Logs: `external/games/sprt-20260930-zlmr/`.

| # | change | vs | TC | games | W-L-D | Elo |
|---|---|---|---|---|---|---|
| 1 | **less arm only**: r-1 at z >= 0 | base | STC | 3000 | 751-714-1535 | **+4.29 +/- 7.06** |
| 2 | more arm only: r+1 at z < -2, r+2 at z < -4 | base | STC | 3000 | 732-757-1511 | -2.90 +/- 6.82 |
| 3 | earlier hopeless quiets count half in LMR's move index | z-LMR | VSTC | 2000 | 515-534-951 | -3.30 +/- 9.67 |
| 4 | history weighted by z (residual learning) | z-LMR | VSTC | 2000 | 545-551-904 | -1.04 +/- 9.50 |
| 5 | less arm + r-2 at z >= +1 | base | STC | 3000 | 722-724-1554 | -0.23 +/- 6.72 |
| 6 | less arm from z >= -0.5, r-2 at z >= +1 | base | STC | 3000 | 736-715-1549 | +2.43 +/- 6.84 |
| 7 | **less arm + deeper re-search**: a reduced fail-high at z >= +1 re-searched a ply PAST normal | base | STC | 3000 | 757-667-1576 | **+10.43 +/- 6.99** |
| 8 | run 1 again | base | STC | 5000 | 1242-1180-2578 | +4.31 +/- 5.35 |
| 9 | **run 7 again** | base | STC | 5000 | 1257-1156-2587 | **+7.02 +/- 5.41** |

Pooled pentanomial: runs 1 + 8, [57, 952, 1910, 997, 84] -> **+4.30 +/- 4.27** over 8000, LOS
97.6%; runs 7 + 9, [54, 921, 1894, 1042, 89] -> **+8.30 +/- 4.28** over 8000. Run 7 was the best
of five variants, so its +10.43 is selection-biased; run 9 is the clean measurement, and it alone
clears zero at LOS 99.4%. Nightly ptnml: 7 [15, 345, 716, 383, 41], 9 [39, 576, 1178, 659, 48].

**What shipped** is runs 7/9, 38 lines in `negamax()`: for a late quiet LMR candidate, z from the
child's corrected evaluation and sigma (one cached lookup), one ply less reduction at z >= 0
(`ZlmrLess` 0), and if its reduced search then fails high with z >= +1 (`ZdeeperZ` 16), the
re-search runs one ply past `childDepth`, bounded by the check extension's `ply < 2 * rootDepth`.
Three sweep seats. Gates: bench deterministic, perft (4 suites) 0 failures, `smp-test` 11/0,
`movepick-test` 14/0, `chess960 selftest`, `openbench-check` PASS, a debug build's bench 11
assertion-clean, `make format-check` clean.

#### What it says

**Sigma is a property of the child, not the node.** Every table that conditioned on the node's
sigma came back flat; every table conditioned on the child's z separated its population by
orders of magnitude. If the head is used again in the search, read it where a move LANDS.

**At STC, z pays only on the promising side.** The night-2 ablation split z-LMR's +3.5 into
+4.3 for searching promising moves (z >= 0) deeper and -2.9 for cutting hopeless ones harder.
At STC depths the base engine already disposes of hopeless late moves cheaply - LMR reduces them
hard and the child's own reverse futility cuts them on entry - so extra cutting buys nothing and
occasionally misses; night 1's z-prune at exactly 0.00 is the same lesson in its purest form.
What the search does NOT already know is which of its heavily reduced late moves deserve depth,
and z names them. Spending that depth twice - less reduction up front, and a ply past normal on
the re-search once the reduced search agrees - is the shipped +8. VSTC, being node-starved,
rewarded the cutting arm instead, which is why night 1's VSTC screen overstated the idea.

**Constants fitted at VSTC took back what was left.** Run 18 is runs 1, 11 and 12 plus the SPSA
drift toward more aggressive LMR and LMP, and it was -0.84 +/- 2.85 over 17,716 STC games.
Whatever z-LMR keeps at STC, 2+0.02 constants removed. SPSA at VSTC is not evidence for STC.

**The WDL table is real and belongs at the root - where it did not pay either.** Applied to
every static evaluation (run 14) it lost 17 Elo: in the tree, high sigma mostly marks
mid-exchange debris - the hanging piece, the capture not yet answered - where the value's
material count is right and sigma is high only because no such position was trained on.
Shrinking those toward zero makes the blunder that created them look cheaper. The relationship
was measured on PLAYED positions, and the only played position a search has is its root. There,
as time management, it moved thinking between positions without moving the result (15, 17).

**Process.** Run 18 ran uncapped: a [0, 5] SPRT on a patch worth zero took 9h18m to reject, the
best hours of the budget. Every STC run after it was capped at 3000 or 5000 games. Cap them from
the start, and confirm the best of several variants on fresh games before believing it.

#### Pending

| item | why |
|---|---|
| LTC confirmation of the shipped change | STC-only, like E20-E22a, E29 and E42 |
| SPSA of `ZlmrLess`, `ZdeeperZ`, `ZlmrSigmaFloor` **at STC** | chosen, not fitted; night 1 showed a VSTC fit does not carry to STC |
| a deeper re-search for z >= +2 (two plies), or for checks the reduced search confirms | the deeper side is where z paid; only one ply at one threshold was tried |
| WDL-eval restricted to quiet leaves | the in-tree failure mode is specific - mid-exchange sigma - and untested |
| datagen labels | the shipped change alters search trees, so `search_run_sync()` labels change with it; the next generation should be produced by the engine it trains for |

---

### E46: One night, twenty-five candidates - two correction keys, capture reductions, and no check extension

**Date** 2026-10-02/03 · **Net** `48428bbbd57e` (gen-6-pw) · **Baseline** HEAD `519a152`, bench
333802 (d7) / 7285139 (d13) · **Final bench** 237294 / 5314637 · **Status** **shipped: the final build
measured +39.55 +/- 7.11 over 3000 fixed-length STC games against HEAD.** It was built in two STC
steps: five changes at +11.35 +/- 7.08 over 3000 games against HEAD, then no check extension on
top of them, H1 at +23.42 +/- 8.66 over 1768 games. Quiet checks into LMR, tested on top of that, measured +5.21 +/- 6.56 over 3000
games and is not shipped.

The brief was one night of machine time to find ~50 Elo anywhere in the engine. Of twenty-five
candidates, five survived their screens and one more came out of a direction the screens kept
pointing at - the engine spends too much of its tree on checks.

#### Method

Each candidate was an `#ifdef CAND_*` block in `src/search.c` (`tt.c`, `timeman.c` and `board.c` for
three of them), built alone with `CFLAGS` in the environment as E45 did, and benched at d7 and d13 to
prove it fired. The flag-free build was re-benched after every batch: it reproduced HEAD every time.
Screens ran at VSTC 2+0.02 against HEAD. The first ran at 2000 games and the rest were capped at 1200
(`--rounds 600`): a 2000-game VSTC screen resolves only about +/-10 Elo, and these candidates were
+2-5 by prior, so a screen could only catch clear losers and was not worth paying more for. The
survivors were stacked and the stack proved at STC 8+0.08 against HEAD, capped at 3000 games. Later
survivors were tested ON TOP of the stack, never against HEAD alone. 1 thread, 16 MB,
`UHO_Lichess_4852_v1`, concurrency 14 throughout. The machine was otherwise idle all night.
Queue, logs, binaries and run notes are in `external/games/sprt-20261002-night/` (`NOTES.md`).
Every candidate, rejected ones included, is in `external/patches/e46-night-candidates.patch`.

Time management was never screened at VSTC. At 2+0.02 the 520 ms overhead reserve (10 ms x 52
moves) and the 300 ms bank are a large slice of a 2 s clock, so a VSTC result describes a different
allocator.

#### The screens (VSTC, against HEAD)

| # | candidate | games | Elo |
|---|---|---|---|
| 23 | **no check extension** (singular extensions alone decide what is forced) | 1200 | **+25.52 +/- 11.54** |
| 24 | **quiet checks enter LMR at r-1** (check extension kept) | 1200 | **+20.87 +/- 12.99** |
| 14 | **continuation correction history** (last two moves, weight 128/128) | 1200 | **+14.19 +/- 11.67** |
| 26 | **z-deeper re-search two plies past normal at z >= 2 x `ZdeeperZ`** | 1200 | **+10.72 +/- 11.93** |
| 13 | **non-pawn correction history**, per colour, weight 64/128 each | 1200 | **+8.40 +/- 12.26** |
| 2 | **captures reduced**: half the quiet curve, less capture history / 8192 | 1200 | **+7.24 +/- 12.55** |
| 6 | **qsearch stand-pat raised or lowered by the table bound** | 1200 | **+6.37 +/- 12.76** |
| 11 | hindsight depth (Stockfish's priorReduction +1 / -1) | 1200 | +0.87 +/- 12.14 |
| 7 | check extension only for SEE >= 0 checks | 1200 | +0.29 +/- 12.48 |
| 1 | quiet LMR +1 at cut nodes (E6 measured +2 at about -16) | 2000 | +0.17 +/- 9.67 |
| 30 | `LMR_BASE` 13 -> 12 | 1200 | -1.74 +/- 12.44 |
| 25 | aspiration half-width sized by root sigma (E45's ZASP) | 1200 | -2.90 +/- 11.85 |
| 29 | quiet LMR -1 for a move out of attack onto a safe square | 1200 | -2.90 +/- 12.62 |
| 3 | Stockfish's doDeeper / doShallower, beside z-deeper | 1200 | -4.34 +/- 12.50 |
| 10 | 10-byte TT entries, 1.5x the entries per MB | 1200 | -4.63 +/- 11.43 |
| 21 | `SINGULAR_DEPTH` 7 -> 6 | 1200 | -6.95 +/- 12.17 |
| 22 | qsearch: captures past the second skipped unless they recapture, promote or check | 1200 | -7.82 +/- 11.64 |
| 18 | capture futility at depth <= 6 | 1200 | -8.69 +/- 12.24 |
| 5 | quiet LMR +1 when the table move is a capture | 1200 | -10.14 +/- 11.93 |
| 4 | post-LMR continuation-history update | 1200 | -11.88 +/- 12.46 |
| 28 | static evaluation damped by the fifty-move counter (E42's `fifty`, alone) | 1200 | -16.81 +/- 12.39 |

The 10-byte TT's screen could not judge it. At 2+0.02 a 16 MB table is barely filled, so capacity
cannot pay there; at STC-like budgets it was 70% full within ten moves of a game.

Written and never played, held for time: IIR only at PV and cut nodes; no null move under a table
upper bound below beta; `RFP_DEPTH` 9; a quiet-check ordering bonus; Stockfish's multi-cut on the
verification value; a -1 singular extension at cut nodes; and two time-management variants
(`MOVESTOGO_CAP` 40, and the whole increment in the nominal allocation).

#### STC

| # | change | vs | games | W-L-D | Elo | LLR |
|---|---|---|---|---|---|---|
| 12 | time: soft target x clamp(100 + 2(prev - v) + (v[d-4] - v), 70, 150)%, where prev is our previous move's root score | HEAD | 3000 | 724-763-1513 | -4.52 +/- 6.84 | -1.33 |
| 31 | **stack1**: screens 2 + 6 + 13 + 14 + 26 | HEAD | 3000 | 788-690-1522 | **+11.35 +/- 7.08** | 2.16 |
| 34 | **stack2**: stack1 + no check extension | stack1 | 1768 | 481-362-925 | **+23.42 +/- 8.66** | 2.96, **H1** |
| 35 | stack3: stack2 + quiet checks into LMR at r-1 | stack2 | 3000 | 742-697-1561 | +5.21 +/- 6.56 | 0.91 |
| 37 | **stack2, fixed length (the final build)** | HEAD | 3000 | 895-555-1550 | **+39.55 +/- 7.11** | - |

Ptnml: 31 [27, 326, 702, 412, 33]; 34 [10, 151, 451, 254, 18]; 35 [16, 332, 759, 377, 16];
37 [14, 245, 688, 493, 60] (PGN `20261003-081955-gauntlet.pgn`). No time losses in any STC run. Runs 31 and 35
went to their caps and run 37 had no stopping rule, so those estimates carry no early-stopping bias.
Run 34 stopped on H1 and does carry one. The fixed-length total is the number to quote: +39.55
against the two steps' +11.35 + 23.42 = +34.8, a difference well inside either interval.

Run 12 changed only how the clock is shared out. Mean think time over its 3000 games was 155.2 ms
for dev against 155.0 ms for HEAD, so the -4.5 is the redistribution itself and not a different
amount of time.

#### What shipped

**No check extension.** Every check used to get a ply, bounded only by `ply < 2 * rootDepth`. Checks
were also exempt from LMR, so a check cost a full extra ply wherever it appeared. That bought a
great deal of tree for lines that are seldom forced: the d13 bench falls 23%. Singular extensions
still find the checks that are forced, and quiet checks keep their E42 exemption from LMP and
futility. WAC.001's mate is still found at depth 3. Gating the extension on SEE >= 0 measured flat
(+0.29); only removing it paid.

**Continuation correction history.** `contCorrHist[piece][to][piece][to]`, keyed on the move two
plies up and the move that led here, 2 MB a thread. It has the same update and the same clamp as the
pawn table, and full weight (`CORR_W_CONT` 128).

**Non-pawn correction history.** Each colour's non-pawn pieces, king included, now have a Zobrist key.
The three board mutators maintain both keys branch-free, as they do the pawn key, and
`board_is_consistent()` recomputes both from scratch. The table is
`[side to move][colour][that colour's key]`, at half weight per colour (`CORR_W_NONPAWN` 64), so the
pair together believe what the pawn table does. E16 tried this key together with minor-piece and
one-move keys on a much weaker engine and measured -6.5 +/- 9.9. Alone, on this engine, it screened
+8, and the continuation key screened +14. Both new tables live in `SearchThread`, and
`search_clear()` resets both.

**Captures reduced.** Once a node is three plies deep, every capture after the first (after the
second at a PV node) is reduced, unless it promotes, checks or the node is in check. The reduction is
half the quiet table's, less capture history over `LMR_CAPHIST_DIVISOR` (8192, two plies either way
at full scale). Captures were never reduced before. The good ones are tried first, so a capture far
down the list is a losing one or one of many.

**The table bound as a better stand-pat.** In quiescence, a table value whose bound points past the
corrected static evaluation replaces it as the stand-pat. Proven scores are excluded, and the delta
prunes still reason from the static evaluation.

**z-deeper by two.** E45's re-search one ply past normal now goes two plies past normal where
z >= 2 x `ZdeeperZ`. That item was on E45's pending list.

The three new weights, `CorrWNonPawn`, `CorrWCont` and `LmrCapHistDivisor`, have sweep seats. Gates
on the final tree: bench deterministic; perft (standard, tricky, Chess960) 0 failures; the four perft
suites at depth 4 run under an assertion build, with both non-pawn keys checked at every move,
0 failures; debug bench 10 clean; `movepick selftest` 14/0; `chess960 selftest`; `smp selftest` 11/0;
`openbench-check` PASS; `make classical` (bench 233632), datagen and tuner all build. The final
tree was written from scratch on HEAD in a separate worktree rather than stripped out of the
candidate tree, and it reproduces the tested binary's bench to the node.

#### What it says

**The largest gain was a deletion.** This engine spent more of its tree on checks than any modern
search does. It extended every one and reduced none. The screens kept pointing the same way:
removing the extension gave +25, letting checks into LMR gave +21, and the SEE-gated half-measure
gave nothing. At STC the removal held at +23 on top of everything else.

**Stockfish's pruning and reduction rules transplant badly here; its correction keys transplant
well.** Of the Stockfish-derived candidates that change WHICH NODES get searched - cut-node and
table-capture LMR, doDeeper/doShallower, the post-LMR history update, qsearch move-count pruning,
capture futility, hindsight depth - none was positive, and three were -8 or worse, at a time
control that normally flatters pruning. This search's margins and reductions were SPSA-fitted in its
own shape, and they already absorb what those rules add. The two correction keys are evidence about
the EVALUATION rather than about the tree, and they were the transplants that worked.

**Reducing more keeps losing; searching promising things deeper keeps winning.** Every candidate that
reduced quiet moves harder was flat or negative. The z-deeper extension to two plies was positive
even at VSTC, which is biased against extending. This repeats E45's lesson: at STC the value lies in
which moves deserve depth, not in cutting harder.

**Time management: the clock is shared out about right.** Lengthening the soft target after a
falling score lost 4.5 at STC. Today's 8,397-game PGN (HEAD's allocator on both sides) shows where
the clock goes: about 260 ms a move just past the book, 130 ms by move 45, then the increment alone
from move ~60, with the clock parked at ~1.5 s and drifting up in long games. Median game length is
151 plies past the book exit, and 8 games in 8,397 were lost on time. The parked reserve is
520 ms + 50 x inc/4. Spending it is the untested `TM_INC_QUARTERS` 4 and `MOVESTOGO_CAP` 40.

#### Pending

| item | why |
|---|---|
| LTC confirmation | STC-only, like E20-E22a, E29, E42 and E45 |
| SPSA of `CorrWNonPawn`, `CorrWCont`, `LmrCapHistDivisor` at STC | chosen, not fitted |
| quiet checks into LMR on top of the final build | +20.87 at VSTC, independent of the extension; +5.21 +/- 6.56 over 3000 STC games on top of the final build (run 35, LOS 94%); another 3000-5000 fresh games settles it (`stack3.exe` vs `stack2.exe`) |
| 10-byte TT at STC | its VSTC screen could not see capacity (`stack1tt10` is built) |
| spending the parked clock: `TM_INC_QUARTERS` 4, `MOVESTOGO_CAP` 40 | built, never played (`stack1inc4`, `tmh40`) |
| datagen labels | search trees changed again, so `search_run_sync()` labels change with them |

---

### E47: A move prior beside the value net - built, gated offline, and its first use

**Date** 2026-10-03 · **Net** `48428bbbd57e` (gen-6-pw) plus a 448-row move prior, exported as
`8002f68c2a92` (`net-gen-6-pw-pol.nnue`: the same value bytes, the prior appended) · **Baseline** HEAD
`fadb7a7`, bench 237294 (default) / 5314637 (d13) · **Status** Phase 1 gate passed, Phase 2 built and
exact, the first search use **lost -14.05 +/- 7.80** at STC. Continued in E48; **the prior was removed
in E49**, so the code this entry describes is no longer in the tree.

Every use of the uncertainty head so far has asked one question per POSITION - how wrong is this
evaluation likely to be - and its in-tree uses that CUT have faded with time control (E45: z-prune
0.00, the hopeless-side arm -2.9 at STC) while the side that spends depth on promising moves held at
+8. E45 also found that sigma is a property of the child and that z "orders nothing". Which late
move deserves depth is a question about each MOVE, and a number per position cannot answer it. So
this asks a second, purpose-built predictor - a move prior - and gates it offline before building
anything that plays.

A separate network was ruled out on cost: evaluation and accumulator pushes are ~30% of cycles
(NNUE.md, "Where the time goes now"), so a second feature transformer would cost ~20-25% nps before
doing anything. The prior is a head on the accumulator the value net already keeps.

#### What was built

- **`datagen legal`** writes `<shard>.mv`: every record's legal moves as the prior's row indices, and
  where `.pol best` sits among them. The row normalisation - rank-flip for black, file-mirror with the
  king kingside, castling as the king's real step, promotions by the promoted piece - is
  `nnue_policy_index()` in `src/nnue.h`, the ONE copy datagen, the stats dump and the engine share.
  `datagen verify` regenerates every list byte for byte; `make datagen-test` checks that and that a
  corrupted `.mv` is refused. `trainer/tests/test_policy.py` checks on datagen's real output that a
  position and its colour-flipped and file-mirrored twins list identical rows.
- **`python -m nnue.policy train`**: `logit(m) = w_from[from]·a + w_to[piece, to]·a`, 64 + 384 rows
  over the activated accumulator `a` (the 512 numbers L1 reads), trained by cross-entropy over the
  legal list against `.pol best`, on a FROZEN gen-6-pw trunk - so the value weights stay bit-identical
  and the prior is the only thing being measured.
- **`make policy-stats`**: a d13 bench whose late quiets (the LMR branch) and quiet cutoffs are
  dumped, with each node's FEN and legal rows. Read-only: the target fails unless its node count
  equals the playing build's (5314637 both). **`python -m nnue.policy tables`** scores the dump.

#### Training (20M gen-005 records, val-006, 8 epochs, ~20 s each on a 3070)

| | CE | top-1 | top-3 |
|---|---|---|---|
| uniform over the legal list | 3.170 | 5.5% | - |
| prior, val-006 | **2.717** | **20.0%** | **41.9%** |

Flat from epoch 4: a linear read of value features is what limits it, not data. val-006 is
quiet-filtered (its `best` is never a capture), which is the population the prior serves.

#### The gate (d13 bench tree, 1,256,601 late quiets, 1.38% beat alpha)

Beat-alpha rate by the prior's rank among the node's legal moves:

| rank | share | beat alpha |
|---|---|---|
| 1 | 4.7% | 5.72% |
| 2-3 | 9.4% | 3.51% |
| 4-6 | 13.6% | 2.05% |
| 7-10 | 15.8% | 1.29% |
| 11+ | 56.5% | 0.52% |

Inside each z band, the prior's top three against its rank 7+: **4.0x** (z <= -4), **4.0x** (-4..-2),
**3.1x** (-2..0), **3.0x** (z > 0, where rank 1 beats alpha 17.3% and rank 11+ 4.2%).

| AUC for "beat alpha" (0.5 is nothing) | |
|---|---|
| history (main + continuation) alone | 0.786 |
| z alone | 0.897 |
| prior alone | 0.752 |
| z inside history quintiles - what z adds to history | 0.871 |
| prior inside history quintiles | 0.678 |
| **prior inside history x z cells - the gate** | **0.593** |
| the same at node depth 3-4 / 5-7 / 8+ | 0.591 / 0.598 / 0.599 |

**Ordering, which z cannot do.** At 93,051 quiet cutoffs that came after at least one failed quiet,
the prior would keep only **0.37** of the failed quiets above the cutter (the search's own order 1.00
by construction, chance 0.50; E45 measured z at 0.506 for k = 1). With one failed quiet it ranks
the cutter first 62.8% of the time against 50% by chance, with two 47.5% against 33.3%.

**Cost.** Negamax nodes at depth >= 4 / 5 / 6 / 8: 16.8% / 9.9% / 6.0% / 2.1%.

Read: the prior knows something history and z together do not, by a moderate margin that does
not fade with depth - unlike sigma's cutting uses, which faded with time control. It is much weaker
than z as a beat-alpha signal and much stronger as an ordering one. The caveat is selection: the
cutoff table samples only the cases where history's order was wrong, and whether reordering costs
the cases it got right is a question only a game can answer.

#### Phase 2: export and inference

`tools/export_net.py --policy <head.pt>` appends int16 rows at scale 2^10 (`reserved[2]`) and int32
biases, flagged in `reserved[1]`; it refuses a head trained on a different trunk and a flat net, and
bounds every row to half of int32 since a move sums two. `src/nnue.c` reads it, rejects unknown flag
values by name, and computes `nnue_policy_scores()` as one activation plus two `nnue_dot()` rows per
move, origin rows shared. **`nnue verify`: 10,000 positions and 4,480,000 prior rows exact, on the
AVX2 and the scalar build.** Quantised against float, rows drift 0.0095 logits on average, 0.22 at
worst. Re-exporting gen-6-pw without a prior reproduces `48428bbbd57e` byte for byte, and the
prior's net benches 237294 - the shipped tree - until the search reads it. UCI `PolicyHead`
(default true) runs a net as if it had no prior: an A/B on one file.

#### Phase 3.1: the promising side of LMR

At depth >= `PolMinDepth` (4), the first late quiet at a node generates the legal quiets and scores
them - at the PARENT, before the move is made, because the prior reads the accumulator of the side
choosing. A late quiet among the prior's top `PolLmrRank` (2) is reduced a ply less. Bench 231634
(default) / 6373741 (d13: the tree is 20% larger, which is what reducing less buys). Gates:
`PolicyHead=false` and the shipped net both bench 237294; `smp selftest` 11/0, `movepick selftest`
14/0, `chess960 selftest`, `openbench-check` PASS, trainer suite 198 passed.

| # | change | vs | TC | games | W-L-D | Elo | verdict |
|---|---|---|---|---|---|---|---|
| 1 | prior top-2 late quiets r-1, depth >= 4 | shipped net | STC | 2424 | 535-633-1256 | **-14.05 +/- 7.80** | stopped by hand at LLR -2.73 |

Binaries, notes and log: `external/games/sprt-20261003-prior/`. Ptnml [24, 340, 583, 240, 25], no time losses.

**Rejected.** Reducing the frozen prior's favourite late quiets less made the d13 tree 20% bigger,
and the prior's picks were not good enough to pay for it. That is consistent with the gate: inside
history x z cells the prior's AUC was 0.593, against z's 0.871 over history - a modest signal
spent on the expensive side of LMR. `PolLmrRank` now defaults to 0, so a net that carries a prior
does not switch the rejected use on by itself, and the prior is not computed unless something
reads it. The next step is not a different use of this head but a better head: a prior trained
jointly with the value net, so the feature transformer learns move choice too (`--move-prior`).

#### Joint training: what the pilot said

The frozen head plateaued at 20% top-1 by epoch 4: a linear read of features learned for
EVALUATION, which more data cannot fix. Trained jointly (`--move-prior`), the prior's gradient
reaches the feature transformer, and then the whole of gen-6 is worth reading. The loader makes
each batch's legal lists through `make policy-lib` - `tools/datagen.c` as a shared library, the
same `moves_encode()` that writes `.mv`, byte-identical on 500,000 val-006 records - because a
stored `.mv` for 2.5B records would be ~640 GB.

gen-6-pw's own recipe, from scratch, 100M positions of gen-006, val-006, 8 workers:

| `--prior-weight` | val value loss | vs no prior | prior val CE | prior top-1 |
|---|---|---|---|---|
| 0 (no prior) | 0.005862 | - | - | - |
| **0.0005** | 0.005876 | **+0.2%** | 2.704 | 19.6% |
| 0.002 | 0.005975 | +1.9% | 2.584 | 22.1% |
| 0.008 | 0.006217 | +6.1% | 2.473 | 24.5% |

A real trade, priced: every step of weight buys prior and costs value. At 0.0005 the value cost
is inside single-seed noise, and after 100M positions the joint prior already equals the frozen
head that needed a fully trained net under it. Throughput at steady state: 1.16M positions/s
without the prior, ~820k with it (8 workers; 4 is too few to keep up, 12 oversubscribes 16
threads). `--prior-fraction` labels only a share of each batch, but bought back only ~10% at
0.25 - the extra columns cost as much as the generation - so the first run labels everything.

A first full run: gen-6-pw's command plus `--move-prior --prior-weight 0.0005 --workers 8` and
`--val val-006` (the old val set has no `.pol`). Then two SPRTs: the new net's VALUE against the
shipped net, with every use of the prior off (the default), and only if that holds, the prior's
uses one at a time - `-DPOL_ORDER_W_DEFAULT=5500` first, the table it is strongest on.

#### Pending

| item | why | what happened |
|---|---|---|
| a joint gen-6 run at `--prior-weight 0.0005`, and its value SPRT | above | E48: +7.47 vs gen-6-pw; E49: the same run without the prior is +22 better still |
| quiet ordering by the prior (Phase 3.2) | the table it is strongest on; one SPRT, after run 1 | E48: 14 shapes, flat at STC |
| LMP / futility exemption for the prior's top quiets (3.3) | only if 3.1 and 3.2 pass; z cannot do this, it needs the move made | E48: tried anyway, -24.94 at VSTC |
| sigma at LTC: the head on/off on one net | the user's thesis that sigma's uses fade at the highest Elo | **still not run.** The `UncertaintyHead` option it needs is now in the tree (built in E48, kept when the prior went) |
| a joint-trained or nonlinear prior | only if the frozen linear head is what caps it - top-1 flattened at 20% by epoch 4 | E48: joint 25.6% top-1; 64/256-unit heads no better than linear |

---

### E48: The joint move prior in the search - what it knows, and every use tried

**Date** 2026-10-04 · **Net** `ac080356d440` (`net-gen-6-prior.pt`: gen-006 + gen-006-u, `--move-prior
--prior-weight 0.0005`, `--unc-weight 0.0005`, gen-6-pw's recipe otherwise) · **Baseline** the same net
with every use of the prior off, bench 184909 / 5200865 (d13) · **Status** **no use of the prior gains
Elo**; ordering is flat at STC, every use that changes how much effort a move gets is negative. The
prior was removed in E49.
Binaries, queue and logs: `external/games/sprt-20261004-prior/` (`NOTES.md`).

E47's frozen-trunk prior lost 14 Elo in its one game test and plateaued at 20% top-1, which said
the head could only re-weight features learned for evaluation. This is the jointly trained one
E47 pointed to, and a day of asking what the search can do with it.

#### What the joint prior knows

Training: val-006 top-1 **25.6%** (frozen: 20.0%), CE 2.450 (2.717), still creeping up at epoch 25.
On the new net's own d13 bench tree (`make policy-stats`, 1,280,683 late quiets, 1.41% beat alpha):

| | frozen (E47) | joint |
|---|---|---|
| prior alone, AUC for "beat alpha" | 0.752 | **0.785** (history alone: 0.779) |
| prior inside history x z cells - the gate | 0.593 | **0.638** |
| beat-alpha rate, rank 1 vs rank 11+ | 5.72% / 0.52% | **7.35% / 0.38%** |
| failed quiets the prior keeps above the cutter (chance 0.50) | 0.37 | **0.33** |
| cutter ranked first after one failed quiet (chance 50%) | 62.8% | **66.9%** |

Refitted against history, one logit of prior is worth ~5,500-6,400 history units (7,300 at depth
3-4, 3,600 at 8+); LMR's own history divisor makes that ~0.7 ply.

**Where it knows more than history** - AUC inside bands of |main + continuation history|:

| \|history\| | share | history AUC | prior AUC |
|---|---|---|---|
| 0-250 | 32.7% | 0.624 | 0.768 |
| 250-1,000 | 27.3% | 0.704 | 0.774 |
| 1,000-2,500 | 21.9% | 0.746 | 0.774 |
| 2,500-5,000 | 11.3% | 0.783 | 0.774 |
| 5,000+ | 6.8% | 0.767 | 0.754 |

**The head's shape is not what limits it.** On the joint net's frozen trunk, 20M gen-006 records, 4
epochs: linear 25.2% top-1 (CE 2.469), 64 clipped-ReLU hidden units 22.7% (2.591), 256 units 24.8%
(2.483). The ceiling is in the activations, not in how they are read.

#### Every use, one at a time

Each candidate is the baseline plus one `-DPOL_*_DEFAULT` flag (src/search.c, all off by default),
benched at d7 and d13 to prove it fired. VSTC 2+0.02, 1200 games, [0, 5], against the baseline:

| use | flag | bench d13 | Elo |
|---|---|---|---|
| quiet ordering, 6000 history units per logit, depth >= 4 | `POL_ORDER_W=6000` | 4396186 | **+8.11 +/- 11.95** |
| ordering at 12000 | `POL_ORDER_W=12000` | 5335925 | -9.56 +/- 11.71 |
| ordering at 4000 | `POL_ORDER_W=4000` | 5802511 | -6.08 +/- 11.39 |
| ordering 6000, -600 per ply above depth 4 | `+POL_ORDER_SLOPE=600` | 5582972 | -1.45 +/- 11.84 |
| ordering 6000 from depth 5 | `+POL_MIN_DEPTH=5` | 5802783 | -3.47 +/- 11.55 |
| ordering 6000, cut-nodes only | `+POL_ORDER_CUTONLY=1` | 5083218 | +2.90 +/- 11.91 |
| ordering 8000 tapered to 0 by \|history\| 3000 | `POL_ORDER_GAP=3000` | 5056522 | -10.14 +/- 12.17 |
| the same at 12000 | | 5014636 | **-21.16 +/- 12.28** |
| the same at 8000, cut-nodes only | | 4276226 | -5.50 +/- 12.08 |
| LMR: prior read like history, 12/16 ply per logit, both ways | `POL_LMR_K=12` | 5800868 | -13.32 +/- 11.90 |
| LMR: +1 ply 2+ logits below the node's mean | `POL_LMR_MORE=32` | 4813531 | -9.27 +/- 11.87 |
| LMR: -1 ply for the prior's top 2 | `POL_LMR_RANK=2` | 5496960 | +3.47 +/- 11.55 |
| top-2 exempt from late-move and futility pruning | `POL_PRUNE_RANK=2` | 4623684 | **-24.94 +/- 11.41** |
| history orders the first 2 quiets, then the prior re-sorts the rest at 12000 | `POL_LATE_AFTER=2` | 4791621 | **+12.75 +/- 11.85** |
| the same after 1 quiet | `POL_LATE_AFTER=1` | 5585792 | +0.87 +/- 11.42 |
| after 2, at 32768 (the prior dominates the rest) | `+POL_LATE_W=32768` | 5923642 | 0.00 +/- 11.85 |

Ordering from depth 2 was dropped before playing: 10% slower to d13 than the baseline, where
ordering from depth 4 was 14% faster (2.16 s against 2.51 s, interleaved). LMR at 24/16 ply per
logit (`POL_LMR_K=24`, d7 bench 218899) was skipped once 12/16 had lost 13. A "ship" test -
the joint net with ordering at 6000 against the old shipped engine - was queued and never played,
since ordering itself was flat.

**STC 8+0.08, ordering at 6000: -4.79 +/- 9.44 over 1596 games** (ptnml [16, 198, 386, 188, 10]),
stopped by hand once +5 was out of reach. The VSTC +8 was the best of five ordering variants
and did not survive fresh games - the selection-bias trap E45 also fell into.

**STC, the late switch after 2 quiets: +1.56 +/- 8.58 over 2000 games** (ptnml [17, 249, 455, 266,
13]). It is the use the offline cutoff table pointed at - the prior only where history's own first
picks have failed - and its VSTC +12.75 was the best of three. It too did not survive fresh games.

#### What it says

**The prior's knowledge does not convert, in any of the shapes tried.** It separates late quiets as
well as history, it orders cutoffs better than chance where history has failed, and it buys 14%
time-to-depth on the bench - and the games are flat. What the static tables measure is mostly
what history learns DURING the search anyway: history is weak on a move the search has never
tried, but within a few hundred nodes it has tried it.

**Every use that decides how much effort a move gets is negative**, and the harder the prior pushes,
the worse: pruning exemption -25, reading it into LMR -13, a gap-filling weight of 12000 -21. A move
the prior likes and history does not is usually a move the search has not yet tried - promoting it
delays the cutter history already knows.

**Ordering is the only use that is not clearly harmful**, and at STC both shapes that screened best
- a blend at the fitted weight, and the late switch - are flat: -4.79 +/- 9.44 and +1.56 +/- 8.58.

**Process.** Twice today a best-of-N VSTC screen (+8.11, +12.75) measured zero on fresh STC games.
At 1200 games a screen resolves +/-12 Elo, and the effects being looked for are a few Elo: a screen
here can drop a clear loser (prune2, gap12k) but cannot pick a winner. A future attempt should go
straight to STC, or screen at 3000+ games.

#### The net itself: better, and correctly centred

**The joint net against the old shipped one, every use of the prior off: +7.47 +/- 8.64 over 2000
STC games** (ptnml [11, 242, 466, 255, 26], LLR +1.30 on non-regression bounds [-5, 0], capped).
The 95% interval is about -1 to +16: at least as strong, probably a few Elo stronger. Two changes
went into it at once - the prior's loss on the shared feature transformer, and gen-006-u's 596M
extra positions - so this does not say which one paid; it says the joint run is a better VALUE
net, which is the thing a net is shipped for. **(E49 split them: the prior's loss was costing ~22
Elo, and the same run without it is +25.41 over gen-6-pw.)**

**Its sigma margins need no re-centring.** The run trained the uncertainty head at a twentieth of
gen-6-pw's weight (`--unc-weight 0.0005` against 0.01), which is the change NNUE.md 5c warns can
silently de-centre `unc_scale()`. Probed on today's search under today's constants (99/9/145):
old net mean scale 128.7, sd 14.7, 33.1% at the cap; new net 128.4, sd 14.9, 31.4%. Matching the
two solves to exactly the shipped 99 and 9.

#### Two of E46's open items, on the old net against HEAD's tree

Both re-added as off-by-default knobs, built on the shipped gen-6-pw net, against a base that
reproduces HEAD exactly (bench 237294 / 5314637):

| change | knob | games | Elo | |
|---|---|---|---|---|
| quiet checks into LMR at r-1 | `QCHECK_LMR=1` (search.c) | 2000 | 0.00 +/- 8.80 | with E46 run 35's +5.21 over 3000: ~+3 +/- 5 over 5000. Settled: not worth shipping |
| spend the whole increment, not 3/4 | `TM_INC_QUARTERS=4` (timeman.c) | 1800 | **+7.53 +/- 8.81** | ptnml [13, 201, 432, 242, 12], LLR +0.83, **no time losses**. Promising, not proven |

#### Where this leaves the prior

The second model is built, exact, toggleable at every stage, and measurably informative - and the
search cannot yet use what it knows. What would change that is a prior that knows something
history cannot LEARN within a search, not one that predicts the same thing a little earlier: a
much stronger head (more weight, more data, a policy-sized net), or a use outside the move loop.
Every `POL_*` knob stays at 0, so the shipped behaviour is unchanged. (E49 tried both of those
routes and the prior was then removed.)

#### Evening follow-up: is there room, and can the model get better? (cut short - machine needed)

**Ordering has almost no room at the nodes the prior is asked.** From the d13 dump: at depth >= 4
only **2.0-2.8% of nodes** end on a quiet cutoff that came after a failed quiet (mean 2.6 failed
quiets before the cutter); everywhere else, quiet order cannot change which move cuts, only which
moves LMR reduces. So ordering's 15% smaller bench tree was reductions moving around, not earlier
cutoffs - which is why it was flat in games. Depth 1-3 nodes have more (1.8-4.5%) but are 82% of
the tree, where the prior costs too much.

**Joint training weight is a weak lever.** Fine-tunes of `net-gen-6-prior.pt`, 250M-position
epochs at lr 5e-5, identical but for `--prior-weight`:

| weight | epochs | val value loss | prior CE | top-1 |
|---|---|---|---|---|
| 0.0005 (control) | 2 | 0.003889 | 2.450 | 25.6% |
| 0.002 | 2 | 0.003916 (+0.7%) | 2.417 | 26.2% |
| 0.008 | 1 (stopped) | 0.004013 (+2.9% vs control's epoch 1) | 2.382 | 27.0% |

Sixteen times the weight buys about 1.5 points of top-1 and visibly costs the value head.

**The prior's edge is not classical tactics.** The stats dump now records each late quiet's facts
(polstats v2: origin/destination attacked, SEE-safe, escaping or walking into a lesser attacker,
piece type). Beat-alpha rates: SEE-safe 1.61% vs 0.20%; escaping a lesser attacker 4.40% vs 1.30%;
into a lesser attacker 0.09% vs 1.51%. But the prior's AUC inside history x z cells only falls
from 0.638 to **0.623** once facts x piece are held fixed as well - what it knows beyond history is
mostly not these. (The facts-only and logistic-fit rows did not finish.)

**Not run:** a head trained on the search's own labels. 4,000 gen-006 roots were searched to depth
12 by the stats build (~8 GB of late-quiet labels; `intree_collect.py` / `intree_train.py` in the
session scratchpad): a residual 448-row head on the frozen trunk, scored against the joint prior on
held-out roots. That is the next experiment if the prior is pursued. (Run in E49: nothing once the
baselines are fitted properly.)

---

### E49: The move prior's last tries, and what its stats harness found instead

**Date** 2026-10-04/05 (overnight) · **Nets** joint `ac080356d440` for prior work; shipped gen-6-pw
`48428bbbd57e` for everything played against HEAD · **Status** the prior gains nothing in the
search and **costs ~22 Elo as a training loss**; the same retrain without it, `net-gen-6-noprior` (`49e4301d6224`), is **+25.41 +/- 8.68** over the shipped net.
Binaries, queue and logs: `external/games/sprt-20261004-prior/` (`NOTES.md`).

#### Training the prior on the search's own question: no

The joint prior imitates datagen's root best move. The search's question is different: which late
quiet, inside its own tree, deserves depth. The stats build (`make policy-stats`, polstats v2)
answers that directly, so it was run over 4,000 gen-006 roots to depth 12 (UCI, `ucinewgame`
between roots): ~8 GB of late-quiet outcomes. Depth >= 4 nodes, one node in four: 18.5M late
quiets from five shards for fitting, 3.75M from a sixth (different roots) for scoring.

| model of "this late quiet beats alpha" (held-out roots) | AUC | AUC within a node |
|---|---|---|
| history alone | 0.799 | 0.397 (*) |
| joint prior alone | 0.780 | 0.761 |
| (a) logistic: history, cont, z, moveCount, r, depth, tactical facts | 0.937 | 0.733 |
| (b) (a) + joint prior | 0.940 | **0.768** |
| (b) held fixed + a 448-row head trained on these labels | 0.9435 | 0.769 |

(*) below chance by selection: a cut node stops at its first move above alpha, so within a node
the mover is the last one tried - the one history ranked lowest.

The head trained on in-tree labels adds +0.003 AUC, all of it node-level (within node +0.001), and
overfits by its third epoch. A first run reported 0.928 and "0.741 inside history x z" for it -
an artifact: its (a) and (b) were SGD fits that had not converged (BCE above a constant
predictor's), and the head's rows absorbed what the linear part had not learned. **Fit the
baselines to convergence before crediting a new signal.**

What survives: within a node, the joint prior is the only signal that separates moves, worth
+0.035 AUC over everything the search already reads. That is real per-move knowledge, and small.

#### The prior as a training aid for the value net: no

E48's joint net played +7.47 +/- 8.64 against gen-6-pw, but that run changed three things at once:
gen-006-u added to the data, `--unc-weight` 0.0005 instead of 0.01, and the prior loss. A retrain
with the joint run's exact recipe and seed but no `--move-prior` (`net-gen-6-noprior`) has the
lower validation value loss at every matched epoch so far - e1 0.004819 vs 0.004862, e2 0.004623
vs 0.004657, e3 0.004373 vs 0.004392 - and the fine-tunes agree: more prior weight, worse value
head. Whatever the joint net gained, the prior did not give it.

#### Gating singular extensions on the prior: no signal

polstats v3 adds an S line per singular verification (the table move, the result against the
singular beta). Over the d13 bench tree, 28,064 verifications, 44.6% extend: the prior's
log-probability of the table move separates "extends" from not at AUC **0.514** (0.53-0.58 by
depth). A third of table moves get under 2% from the prior and still extend 46% of the time;
only the 2.4% it rates above 0.7 stand out (76%). The prior does not understand the positions
singular search is asked about, so it cannot skip or force the verification.

#### What the stats harness found instead: a quiet move's own safety

polstats v2 records each late quiet's tactical facts. History keys only "origin attacked" and
"destination attacked"; inside every history quintile of the d13 bench tree:

| late quiet | share | beats alpha (history q0-q3) | mean r today |
|---|---|---|---|
| all | 100% | 0.36-0.96% | 2.20 |
| SEE calls it losing | 14% | **0.06-0.10%** | 2.78 |
| walking into a lesser attacker | 7% | 0.03-0.09% | 2.83 |
| escaping a lesser attacker | 3.5% | **1.5-2.2%** (q4: 12.5% vs 4.6%) | 2.05 |

Each became an off-by-default knob in src/search.c, with two standard terms the survey of the move
loop turned up missing. All STC 8+0.08, shipped gen-6-pw net, against `oldnet.exe` (HEAD's tree,
bench 237294; `thr-base` with every knob off reproduces it):

| change | knob | d13 bench | first 3000 | fresh 3000 | pooled 6000 |
|---|---|---|---|---|---|
| LMR +1 at an expected cut node | `LMR_CUTNODE=1` | 5458505 | +9.27 +/- 6.92 | +0.58 +/- 6.63 | **~+4.9** |
| threat-aware quiet ordering, 8192 (x2 rook, x3 queen) | `THREAT_ORDER=8192` | 5244546 | +6.02 +/- 6.98 | +0.81 +/- 6.86 | **~+3.4** |
| LMR +1 for a late quiet SEE calls losing | `LMR_SEE_BAD=1` | 4950065 | +3.36 +/- 7.10 | | |
| LMR -1 for a quiet escaping a lesser attacker | `LMR_ESCAPE=1` | 4978283 | -2.66 +/- 7.07 | | |
| quiet ordering +8192 for a quiet check | `CHECK_ORDER=8192` | 5592627 | -4.29 +/- 6.99 | | |
| spend the whole increment (E48's +7.53) | `TM_INC_QUARTERS=4` | 5314637 | | +0.46 +/- 6.81 | ~+3 over 4800 |

**The two leaders were the best of five, and both shrank to near zero on fresh games** - the same
selection effect E45 and E48 met. What remains is two small positives, neither proven alone.

The facts that separate moves best did not pay as reductions: escaping moves beat alpha 2-5x more
often, yet reducing them less lost; the same pattern as every prior-based LMR use in E48. A
signal that predicts which moves matter is not, by that alone, a better reduction than the tuned
LMR it is added to.

#### The result of the night: the same retrain without the prior

`net-gen-6-noprior` finished at 09:05: the joint run's recipe and seed (gen-006 + gen-006-u,
`--unc-weight 0.0005`, 24 x 500M + a finishing pass), no `--move-prior`. Its validation value loss
was lower than the joint net's at **all 24 matched epochs and the finish** (0.003789 vs 0.003812).
Exported and exact on 10,000 vectors (`make nnue-test ARGS="net-gen-6-noprior"
EVALFILE=external/nets/net-gen-6-noprior.nnue`, sha in `net-gen-6-noprior.json`).

Its sigma runs ~2% wide under the shipped 99/9 (`make unc-probe`: mean scale 131.6, 39.7% at the
cap, against gen-6-pw's 128.7 / 33.1%); 100/7 matches it, and every game below gives it those.
`UNC_SIGMA_BASE` / `UNC_SIGMA_SLOPE` gained `-D..._DEFAULT` overrides for that, defaults unchanged.

| test | games | Elo | |
|---|---|---|---|
| no-prior net vs shipped gen-6-pw (SPRT [0, 5]) | 1600 | **+26.54 +/- 9.49** | **PASSED**, LLR +2.95, ptnml [7, 153, 371, 249, 20], no time losses |
| the same, fixed length (no early-stop bias), 100/7 compiled in | 2000 | **+25.41 +/- 8.68** | ptnml [13, 199, 431, 330, 19], no time losses |
| **no-prior net vs the joint net** (same recipe and seed, each centred) | 2000 | **+22.27 +/- 8.51** | ptnml [8, 208, 440, 316, 19] - what the prior loss cost |
| threat ordering + cut-node LMR on top of the no-prior net (SPRT [0, 5]) | 3000 | +3.24 +/- 6.88 | LLR +0.42 - the classical pair, still not proven |

**The prior was not a free passenger in training: it cost the value net about 22 Elo.** The joint
net's +7.47 over gen-6-pw was the extra data and the 20x smaller sigma weight, net of that loss;
without the prior the same run is +25. The validation loss saw it (-0.6% at every epoch, from the
first) but undersold it badly - 0.6% of loss is not what 22 Elo usually looks like. E48 read the
joint net's +7.47 as "the prior is at worst harmless to the value"; it was the opposite.

#### Where this leaves the move prior

Every route E47-E49 could think of was measured: 17 search shapes (E48), training it on the
search's own labels, gating singular extensions, more training weight, and the prior as a training
aid. **None gains, and the last one loses ~22 Elo.** What it knows that the search does not is
real but small - +0.035 AUC within a node - and every use that spends effort on that knowledge
costs more than it returns. The infrastructure stays (`--move-prior`, `make policy-lib`,
`make policy-stats`, the `POL_*` knobs, `PolicyHead`), all off; **do not train with
`--move-prior`.**

What the work produced instead is the recipe the joint run carried: gen-006 + gen-006-u at
`--unc-weight 0.0005`, without the prior - `net-gen-6-noprior`, +25 at STC, and the night's three
new knobs, `THREAT_ORDER` and `LMR_CUTNODE` small unproven positives (~+3 to +5 each over 6000
games), the rest settled negative.

**Shipped:** `net-gen-6-noprior` is the pinned net, with `UNC_SIGMA_BASE` 100 / `UNC_SIGMA_SLOPE` 7 as
the defaults; the default build benches **207432** (d13 5853074), node for node `np-base-c`, the
binary the +25.41 was measured with. An LTC check is still to run. The no-prior recipe is also the
right baseline for any later gen-7 run.

#### Built, and never played

Recorded so nobody rebuilds them thinking they are new. All in `external/games/sprt-20261004-prior/bin/`:
- `thr4k` / `thr16k`: threat ordering at 4096 and 16384 (d13 5593295 / 5292029) - the dose
  follow-up for 8192, dropped once 8192's fresh games came back +0.81.
- `combo3`: threat ordering + cut-node LMR + SEE-losing LMR on the shipped net (d13 5020522).
- `31-stc-newnet-confirm`: fresh games for the joint net vs gen-6-pw, held in favour of the cleaner
  no-prior test. `36-stc-combo2` (the pair on the shipped net) was stopped at its start for the
  same pair on the no-prior net (39).

#### The removal

With every route measured, the move prior is taken out of the tree entirely: the search's
`PriorCache` and every `POL_*` knob, `nnue_policy_*` and the net-file payload (a net flagged in
`reserved[1]` is refused by the loader's reserved-byte check, and a checkpoint carrying the
prior's `prior.*` weights no longer loads in the trainer - no shim for either), `PolicyHead`,
the exporter's `--policy` / `--drop-prior`, the trainer's `--move-prior` path (`legal.py`,
`policy.py`, their tests), `datagen legal` and the `.mv` sidecar, `make policy-lib`, and the stats
build `make policy-stats` (`src/test/polstats.*`) with it. `.pol` sidecars stay: they predate the
prior. Kept: `UncertaintyHead` and `--drop-uncertainty`, built alongside the prior but about the
other head; the night's classical knobs (`THREAT_ORDER`, `LMR_CUTNODE`, `LMR_SEE_BAD`, `LMR_ESCAPE`,
`CHECK_ORDER`, `QCHECK_LMR`, `TM_INC_QUARTERS`), all off; and `-DUNC_SIGMA_*_DEFAULT` overrides.
The default build benches 237294, exactly the pre-prior HEAD; `nnue verify`, `make trainer-test`
(188), `make datagen-test` and `make format-check` pass; `net-gen-6-noprior.pt` re-exports to the
same bytes (`49e4301d6224`) with the restored exporter.

Still open and untested: the thesis that started E47 - that sigma's uses fade at the highest Elo -
has never been measured. `UncertaintyHead=false` against the same net at LTC is the test.

---

### E50: One night against berserk-8.5 - a repetition detector, two extension changes and a correction key

**Date** 2026-10-05/06 (overnight) · **Net** `49e4301d6224` (gen-6-noprior) · **Baseline** HEAD `9c6275d`,
bench 207432 (d7) / 5853074 (d13) · **Final bench** 207253 / 4606293 · **Status** **stack B shipped
(uncommitted): +16.42 +/- 7.29 over 2626 STC games against HEAD, SPRT [0, 5] passed (H1).** Against
berserk-8.5 it measured **-20.17 +/- 7.80** over 3000 fixed STC games (implied CCRL 3555 +/- 14), and
HEAD **-20.99 +/- 7.79**: **the gap is not closed, and the self-play gain did not visibly transfer.**

The brief was to match berserk-8.5 (CCRL 3575) in one night. Its measured gap at the start was
**-23.39 +/- 19.18** over 488 STC games (PGN `20261005-201305-STC.pgn`). Queue, logs, binaries and run
notes are in `external/games/sprt-20261005-night/` (`NOTES.md`); every candidate, rejected ones
included, is in `external/patches/e50-night-candidates.patch` as `#ifdef CAND_*` blocks.

#### Where the gap is not

**Time.** Over those 488 games dev used 155.6 ms a move against berserk's 157.6 and reached depth
19.37 against 19.32. The median clock at the end was 0.91 s against 0.73 s. Neither allocator
starves the other.

**Foresight.** Most decisive games are decided early, as UHO openings make them: in 80 of 100
decisive losses the winner already saw +1.5 within 40 plies of the book exit. In its losses dev
saw the result a median 5 plies after berserk did. In its wins berserk lagged by 9.

**Speed, partly - and more than it first looked.** Measured again on an idle machine the next
morning (CPU ~6%, no match running), at `go movetime 2000` on five positions, single thread, 16 MB,
three interleaved rounds, medians: berserk searches **2.98M nps in middlegames to HEAD's 1.93M -
54% faster** (all five positions: 3.12M to 2.06M; the endgame 3.69M to 2.64M). Stack B runs at
HEAD's speed, 1.88M. Both engines reach about the same depth in the middlegames (HEAD 18-25,
berserk 19-20), so this engine is getting there on fewer nodes. Nps across two engines compares
how each counts nodes as well as how fast it is, so read the ratio as approximate. Measured first
during the night's screens, with 14 games sharing the cores, both figures were less than half of
these (0.84M and 1.22M) and the ratio was only 1.30-1.45: the contention hid part of the gap.

Sampled on the search thread on the idle machine (`bench 18`, two 30 s runs, 17.3k samples each,
agreeing to within a point): `nnue_stack_trunk` 17.0-18.0%, negamax 16.7%, `eval_state_push`
12.8-13.0%, `tt_probe` 9.0%, move scoring 5.4%, `nnue_refresh` 4.2-4.5%, `nnue_stack_outputs`
3.5-3.9%, `board_attackers_to` 3.5-3.8%, `corrected_eval` 1.8%. Under load, `corrected_eval` had
read 5.0% and `tt_probe` 5.8%: cache contention moves the misses around, which is a reason never
to profile beside a match. Two levers were tried and neither paid:

- A lazy accumulator (`CAND_LAZYACC`): each move's feature delta recorded at push and applied only
  when an evaluation needs it, with the recorded outputs answered before any accumulator is built.
  Bench identical and asserted clean, but **3.7% slower** on the idle machine - five interleaved
  `bench 13` pairs, median 1.999M nps against base's 2.076M, maxima 2.014M against 2.079M. (During
  the screens it had read ~10% slower; that was contention.) The eager update overlaps with the
  work around it; done late, it sits on the evaluation's critical path waiting on cold weight rows.
- `___chkstk_ms` takes ~1% of samples on the idle machine (1.6% under load). `MAX_MOVES` 512 puts negamax at 4.7 KB of frame and
  qsearch at 4.3 KB. `nnue_stack_trunk` sits at 12.7 KB, with `NnzList` sized for
  `NNUE_MAX_HIDDEN`, and `nnue_uncertainty` at 8 KB, for a fallback accumulator it almost never
  uses. Worker stacks are committed in full (`CreateThread` without the reservation flag), so the
  probes are redundant there. The main thread runs bench's and datagen's searches, though, and
  needs them. Not pursued for ~1%.

#### The screens (VSTC 2+0.02, 1200 games, against HEAD)

| candidate | Elo |
|---|---|
| **triple extension**: a quiet singular move with v < singularBeta - 80 gets a third ply | **+7.82 +/- 11.94** |
| **-1 singular extension at cut nodes** (not singular, ttValue < beta) | **+7.24 +/- 12.23** |
| **upcoming repetition** (cuckoo tables): alpha raised to a draw the side to move can force | **+6.37 +/- 11.74** |
| **RFP returns (eval + beta) / 2** | **+5.79 +/- 12.64** |
| **history pruning**: late quiet at depth <= 4 with main + cont history < -4096 x depth | **+4.34 +/- 11.84** |
| **second continuation correction key**, [ply-4 move][ply-1 move], full weight | **+4.34 +/- 12.08** |
| LMR history terms in 1024ths over the table's fractional part | +3.18 +/- 11.73 |
| no null move under a TT upper bound below beta | +2.90 +/- 12.36 |
| PV recapture of the TT move extended a ply | +2.90 +/- 12.20 |
| minor-piece (N, B, K) correction key, weight 128 | +2.03 +/- 11.76 |
| history bonus a ply deeper when the cutoff beats beta by 80 | +1.45 +/- 11.76 |
| eval swing across the parent's quiet move into its main history (x6, clamp 1000) | -2.32 +/- 12.04 |
| LMR at the root (none before: d13 -20%) | -3.47 +/- 11.82 |
| history pruning at -2048 x depth | -3.76 +/- 12.29 |
| killers and counter-move reduced a ply less | -4.63 +/- 12.25 |
| correction tables 16384 -> 65536 entries | -4.92 +/- 11.84 |
| TT cutoff at depth <= 5 only when the bound agrees with the node's cut expectation | -13.90 +/- 12.97 |
| small ProbCut: a TT lower bound >= beta + 416 at ttDepth >= depth - 4 returns beta + 416 | **-20.87 +/- 12.78** |

Built and never played, held for time: opponent-worsening RFP, improving from four plies back,
IIR only at PV and cut nodes, a second IIR ply at deep cut nodes, ProbCut returning v less its
margin, RFP depth 9. Also held: the 10-byte TT from E46, built on both stacks (`stackAtt`,
`stackBtt`). Its VSTC screen cannot see table capacity.

#### STC (8+0.08, against HEAD)

| # | build | games | W-L-D | Elo | LLR |
|---|---|---|---|---|---|
| 25 | **stack A**: the six bold screens | 3000 | 751-665-1584 | **+9.96 +/- 7.09** | 1.85 |
| 26 | **stack B**: A + `THREAT_ORDER` 8192 + `LMR_CUTNODE` 1 (E49's knobs) | 2626 | 696-572-1358 | **+16.42 +/- 7.29** | 2.96, **H1** |
| 27 | **stack B vs berserk-8.5**, fixed length | 3000 | 765-939-1296 | **-20.17 +/- 7.80** | - |
| 28 | stack C: B + no-null-under-TT-bound + recapture + minor key + fractional LMR, against B | 3000 | 722-738-1540 | -1.85 +/- 6.91 | -0.73 |
| 29 | **HEAD vs berserk-8.5**, fixed length | 3000 | 792-973-1235 | **-20.99 +/- 7.79** | - |

Ptnml: 25 [24, 342, 691, 410, 33]; 26 [15, 268, 632, 374, 24]; 27 [65, 437, 657, 289, 52];
28 [23, 378, 715, 360, 24]; 29 [69, 419, 696, 256, 60]. In 27 (`20261006-024025-gauntlet.pgn`) berserk
lost three games on time and an abandoned one; in 29 (`20261006-055624-gauntlet.pgn`) seven and one.
Without them 27 is about 1.5 Elo worse and 29 about 2.5. Run 25 went to its cap,
so its estimate is unbiased. Run 26 stopped on H1 and carries the early-stopping bias. Against
run 25 it says E49's two knobs, measured at ~+3 and ~+5 over 6000 games each, still pay on this
tree. 1 thread, 16 MB, `UHO_Lichess_4852_v1`, concurrency 14.

#### What shipped

**Upcoming repetition.** `board_upcoming_repetition()` is van Kervinck's cuckoo method, as
Stockfish uses it. Every reversible move of a non-pawn piece on an empty board is keyed by the
Zobrist difference it makes, 3668 of them in two 8192-slot tables. Walking back two plies at a
time, once the opponent's moves cancel, the difference between now and that position is one move
of the side to move's own. If the path is clear, it can repeat the position. Only cycles inside
the search count (i < ply), and the scan stops at the nearest null move. A node whose alpha is
below the draw score is then raised to it.

**Singular outcomes.** A quiet table move every alternative fails more than `TEXT_MARGIN` (80)
below the singular window gets a third ply, under the same cap as the double extension. A table
move that is NOT singular at a cut node, and whose stored value is below beta, gets one ply less.

**RFP returns halfway to beta.** The margin says the node fails high, not by how much.

**History pruning.** A late quiet at depth <= 4 whose main and continuation history sum below
`-HIST_PRUNE` (4096) per ply is skipped. Quiet checks are exempt, as from LMP and futility. Twice
as aggressive measured -3.76.

**A second continuation correction key.** `contCorrHist4`, keyed on the move four plies up and the
move that led here. Same update and clamp as the two-ply key, full weight (`CORR_W_CONT`), 2 MB a
thread, reset by `search_clear()`.

**`THREAT_ORDER` 8192 and `LMR_CUTNODE` 1** are now the defaults.

`TextMargin` and `HistPrune` have sweep seats. Gates on the clean tree, which was written on HEAD in
a separate worktree and reproduces the tested binary's bench to the node: perft (standard,
tricky, both Chess960 suites) 0 failures; `movepick selftest` 14/0; `chess960 selftest`;
`smp selftest` 11/0; asserted debug bench clean; `openbench-check` PASS; `make format`;
`make classical` (bench 221962), the tuner and datagen build.

#### What it says

**The self-play gain did not reach berserk.** Stack B is +16.42 against HEAD in self-play, yet
against berserk-8.5 the two measured -20.17 and -20.99 over 3000 games each: +0.82 +/- 11, about
+2 once the forfeits are taken out. The interval does not rule out a partial transfer - half of
+16 is inside one sigma - but the central estimate is near zero, where 50-100% is the usual
expectation. Self-play SPRTs here are measuring something berserk does not pay for: the stack
reshapes this engine's tree against an opponent with the same tree. **Before the next batch of
search changes, gauntlet Elo has to be in the loop, not only self-play** - an SPRT against
berserk-8.5 itself, or `make gauntlet` against the ladder, with self-play only as the screen.

**The gap is about 20 Elo at STC, and it is not where it is cheap to look.** Not in time
management - the two engines spend the clock alike. Not visibly in foresight. It is partly speed:
berserk searches 54% more nodes per second in middlegames on an idle machine, and this engine
reaches the same depth on fewer. The 488-game -23.39 that started the night was right.

**Stockfish's pruning rules transplant badly here again; extensions and evidence transplant well.**
Every candidate that cut harder lost: small ProbCut (-21), TT cutoffs gated on node type (-14),
root LMR, killer LMR, and history pruning at twice the threshold. The winners extend forced lines
(the triple extension), take the draw a line can force, or add evidence to the evaluation (the
four-ply correction key). That is E46's lesson, a night later. The exception is the -1 at cut
nodes, which takes back a ply only where the singular test has just shown the move is not forced.

**`make debug` ignores `CFLAGS` from the environment.** Its recipe rebuilds `CFLAGS` from parts. A
candidate built for an asserted bench the usual way, through `build.ps1`'s environment variable,
is therefore the base build, and its "clean" run proves nothing. The tell is the same debug node
count across different candidates. Pass the flags as `INCLUDES="-Isrc -DCAND_X"`, which the
debug recipe does read.

#### Pending

| item | why |
|---|---|
| stack B against the gauntlet ladder | one opponent says the self-play gain did not transfer; the ladder says whether that is berserk or everyone |
| LTC confirmation | STC-only, like every search result since E20 |
| SPSA of `TextMargin`, `HistPrune` | chosen, not fitted |
| the 10-byte TT at STC on top of stack B | `stackBtt` is built; VSTC cannot see capacity |
| the held screens | six built candidates never played |
| the four +2/+3 screens | together on top of B: -1.85 +/- 6.91 over 3000 (run 28) - not worth retrying as a bundle |
