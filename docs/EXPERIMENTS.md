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


## Absolute strength

Every Elo figure above is relative to another build in `external\baselines`,
none of which is itself rated. This anchors them. `make rating` plays a
ladder of Stockfish `UCI_Elo` rungs and fits one rating by inverse-variance
weighting.

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
