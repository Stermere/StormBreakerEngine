# Status and roadmap

Current implementation status, measured results, and planned work. Supporting
test results are recorded in [EXPERIMENTS.md](EXPERIMENTS.md).

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
| Search: iterative deepening, alpha-beta, quiescence | complete |
| Transposition table probe / store | complete |
| Killers, history, counter-moves, null move, LMR, PVS | complete |
| Aspiration windows, IIR, reverse futility, LMP, futility | complete |
| Razoring, SEE pruning, quiescence delta pruning | complete |
| Singular extensions, multi-cut, negative extensions | complete |
| Capture history, multi-ply continuation history | complete |
| Time management: phase curve + best-move stability | complete |
| Evaluation: material + tapered piece-square tables | complete |
| Evaluation: pawn structure, mobility, king safety, threats | complete |
| Evaluation: king-relative placement (factorised HalfKA, 6144 weights) | complete |
| Evaluation tuning on real game data (`make tuner`) | complete |
| NNUE data generation, record format, shuffler (`make datagen`) | complete |
| NNUE trainer: features, dataset, model, training loop (`trainer/`) | complete |
| NNUE export + C inference, bit-exact against the reference (`make nnue-test`) | complete |
| NNUE inference: int16 accumulator, AVX2, SCReLU, output buckets | complete |
| NNUE integration: incremental accumulator, per-ply stack, refresh on king bucket | complete |
| Correction history (pawn-structure keyed, +25.8 Elo) | complete |
| Correction history: minor / non-pawn / continuation keys | tried, neutral (E16), reverted |
| NNUE: re-tuned search parameters (21 by SPSA, +66.1 Elo, E17) | complete |
| Search: ProbCut, ttPv, history split (E19, E19b) | **+7.50 ± 8.71** |
| Search: cut-node retry | tried twice, costs ~19 Elo (E6, E19a, E19b), removed |
| Search: uncertainty-scaled margins (corrhist-magnitude probe) | **+25.61 ± 9.85** (E20) |
| NNUE: uncertainty head (trainer, export, inference, search) | code complete, bit-exact |
| NNUE: uncertainty-head net + σ-scaled margins | **+21.29 ± 9.22** (E21) |
| Search: 28 parameters re-fitted by SPSA, `Unc*` seats included | **+14.24 ± 7.10** (E22) |
| Search: same re-fit narrowed to nine seats (SEE, delta, σ mapping) | **+12.88 ± 6.71** (E22a) |
| **Uncertainty margins: LTC confirmation (E20-E22a are STC-only)** | **TODO** |
| NNUE: the default evaluation is the network (`make`; `make classical` for the other one) | complete (E11) |
| Data re-labelled by the network (`gen-003`, human corpus) | shipped; marginal, not a step change (E18) |
| Syzygy tablebases in search and datagen; all adjudication removed | built, off by default (E23) |
| Datagen: game progress per record, `selfplay -resume`, one seed = one dataset | built, gated by `make datagen-test` (E26) |
| Datagen: cross-shard dedup in `shuffle` — `gen-004` was 10x redundant | built (E26); re-shuffle `gen-004` before mixing it into gen-5 |
| Trainer: per-record lambda (game progress, phase, source), score clip, source weights | built, every dial defaults to no effect |
| Syzygy prober rewritten for this engine, Fathom removed | built, verified over 4.1M positions (E24) |
| **Self-play data generation with deliberate variation** | **TODO** |
| **gen-5 data generated with tablebases and no adjudication** | **TODO** |
| **Lazy SMP (`Threads` is capped at 1)** | **TODO** |
| Staged movegen 1: try the TT move before generating anything | tried, neutral (E15), reverted |
| **Staged movegen 2: full staged picker, captures and quiets deferred** | **TODO** |
| **Staged movegen 3: the ordering changes staging enables, one SPRT each** | **TODO** |
| Chess960: per-position castling geometry, both FEN spellings, unambiguous notation | shipped; verified against an independent engine (E25) |

`make perft` and `make perft-all` pass exactly — standard chess and Chess960
alike, since the 960 suites are part of the same gate. `make chess960-test`
adds the structural checks a node count cannot make, and `make openbench-check`
passes, so the engine can be registered with a distributed testing cluster.

## Notes on staged move generation

Staged move generation is listed after the NNUE integration deliberately: a more
expensive evaluation shrinks whatever share generation occupies, so it should be
measured against the eval that ships. At bench depth 12, generation and ordering
are ~25% of search cycles, 61% of generated moves are never picked, and 52% of
generating nodes cut on the first move tried - 15% on the TT move, which need not
generate at all.

Those four figures are profile measurements. The ~5-9 Elo once attached to them
was not a measurement, and step 1 went looking for it and did not find it.

Steps 1 and 2 were planned as **pure speedups gated on an unchanged bench node
count**, on the reasoning that the ordering bands in `src/search.c` are strictly
separated and so a staged picker can reproduce today's order exactly. **That is
measurably wrong, and each step needs an SPRT.** Band separation governs the
order in which a *scored* list is walked, but staging also moves *when* the
scoring happens: `score_moves()` currently runs before any child is searched,
whereas a staged picker runs it after the table move's subtree has already
updated `History`, `ContHist`, `CaptureHist` and `CounterMoves`. The remaining
moves are then scored against different tables and can reorder.

**Step 1 was then built, measured and reverted - see E15.** It benched 277624
against 299634, a 7.3% node reduction, with a control isolating that entirely to
scoring time. But nps was flat (the only nodes that skip generating are the ones
that cut on the table move, ~3.75% of cycles gross, and the picker spends part of
it back), and the node reduction did not convert: +0.47 ± 8.52 Elo, stopped
undecided at 2958 games.

That is evidence against the ~10%-of-search-time estimate itself, not only
against step 1, since step 1 took the cheapest and most certain part of that
share. **Do not build steps 2 or 3 on the 5-9 Elo figure.** Measure first, on a
profile, what a full staged picker actually saves in nps - rather than inferring
it from the share of cycles generation occupies.

Promotions are a second, independent reason step 2 cannot reproduce today's
order for free. They straddle the bands three ways: a quiet underpromotion is
generated as a quiet but scores in the winning-capture band, and an SEE-losing
capturing promotion scores down into the quiet band. A tactical stage therefore
has to generate promotions and spill whatever scores below `SCORE_KILLER_1` into
the quiet stage - and unlike the scoring-time effect above, this one is at least
fixable by construction.

## Where to go next

**Every change from here is measured with `make sprt`** — read
[TESTING.md](TESTING.md) first, because the testing methodology is what
separates an engine that improves from one that drifts.

The open work, roughly in order of Elo per unit of effort:

1. **Self-play data with deliberate variation.** Label quality is no longer the
   binding constraint — **coverage is** (E18). A better label on a position the
   net already predicts well teaches it nothing, and a human corpus is fixed in
   what it covers however good the labeller gets. Self-play has the opposite
   problem: it concentrates on the lines the engine already likes, so a corpus
   without variation teaches the net its own blind spots. The levers are
   opening-book spread, randomised early plies and tree sampling (`-tree`, see
   [NNUE.md](NNUE.md)).

   `gen-004` is the first pass at the first two. `datagen selfplay` gained
   `-book`, and the generation is configured to start every game from one of
   793,495 unique ply-20 positions taken out of the CCRL 40/40 archive,
   perturb it by two random plies, and then play deterministically. Iterating
   this loop gives each generation labels from the engine produced by the
   previous generation. New training runs include the uncertainty head.

   The gen-5 preparation added the parts of that loop a generation run cannot
   be given afterwards. Each record now carries **how far it was from the end
   of its game**, in the record format's four reserved bits, because the
   trainer's lambda has to price the game result differently at move 12 and at
   move 80 and cannot know which it is looking at otherwise. `selfplay` takes
   **`-resume`**, which a multi-day run needs and only `label` had. And a game
   is now a function of `-seed` and its global ordinal alone, so `-threads` no
   longer changes the dataset a seed produces — which is what makes the resume
   correct and is worth having on its own. See [NNUE.md](NNUE.md).

2. **Finish the SPSA re-fit.** E22 swept 28 of the 30 seats for +14.24 ± 7.10,
   including the first gradient ever taken on the `Unc*` constants; E22a then
   re-swept only the nine that still had one, for +12.88 ± 6.71. After two
   passes the σ mapping is converged and just two seats are still climbing —
   `SeeQuietMargin` (z −5.6) and `DeltaMargin` (+4.2), the same two on both
   runs. Both states are intact — `external\tune\spsa.json` at 938/2500 for
   the 28-seat run, `external\tune\spsa-unc.json` at 781/1500 for the
   nine-seat one — and `python tools/tune.py --resume --state <file>` continues
   either; a later checkpoint must be tested against the **E22a** defaults, not
   the ones before them. Two groups remain unfitted, both for reasons SPSA at
   STC cannot fix: the depth thresholds `ProbCutDepth` and `HistBonusDepthMax`,
   which cannot bind at the depths STC reaches, and `UncScaleBase` /
   `UncScaleSlope`, which `unc_scale()` never reads when the net has an
   uncertainty head — no sweep against the shipped net can see them at all, so
   fitting them needs a classical or headless-net run (E22a).

3. **LTC confirmation of the uncertainty work.** E20, E21, E22 and E22a are
   all STC-only, and the ~3300 blitz claim is provisional on the same
   grounds. A 40+0.4 SPRT (or the 40+0.4 gauntlet) retires the debt.

4. **Lazy SMP.** `Threads` is capped at 1. The ordering tables in `search.c`
   and the accumulator stack in `src/nnue.c` are file-scope and must move into
   a per-thread block first. Worth nothing in a single-threaded SPRT and worth
   a great deal to anyone actually playing the engine — and it makes every
   future data generation run cheaper.

5. **Staged move generation**, in the three steps the status table breaks it
   into, each with its own SPRT — after reading the notes above.

6. **Uncertainty follow-ups**, one SPRT each: the same-net signal A/B (σ head
   vs corrhist magnitude on an identical net, the attribution E21 deliberately
   did not buy), and combining the two signals — they are not exclusive.

### Deferred or removed

These items are not currently planned:

- **A pawn hash table.** It accelerates `eval.c` and nothing else. With the
  network now the default evaluation, the code it speeds up is on its way
  out of the hot path, and the work would be spent the day before it stopped
  mattering.
- **Ablating the E1 and E10 batches.** Both went in as batches and neither
  attributes its gain to any individual term, which is a real gap in the
  record — but closing it *explains* Elo already banked rather than adding
  any. The pending table at the end of [EXPERIMENTS.md](EXPERIMENTS.md) is
  kept for the day the queue is empty; it should not compete with anything
  above.
- **A policy head for move ordering.** Cancelled: the track record in
  alpha-beta engines is thin, history heuristics are already very good, and
  the seat went to the uncertainty head instead ([NNUE.md](NNUE.md), Task 5b).
  The `.pol` sidecar machinery in datagen remains for shard alignment;
  `-nopolicy` turns it off for new runs.
