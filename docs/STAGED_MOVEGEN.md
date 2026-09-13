# Main-search staged move generation

Implemented against `defc301`. **Behavioural change, provisionally kept after an
owner-run STC test stopped for machine-time budget. SPRT inconclusive; LTC pending.**

## Scope and contracts

Normal main-search nodes use an invocation-local picker in search.c:

1. Validated TT move, unless excluded.
2. Generate/score `GEN_TACTICALS`, select high-scoring tacticals.
3. Validate/try killer 1, killer 2, then counter-move.
4. Generate/score `GEN_NON_TACTICALS`, select quiets by history.
5. Select the retained losing captures; never regenerate them.

The new generators partition `GEN_ALL` exactly. All promotions, including quiet
underpromotions, are tactical. Existing `GEN_CAPTURES`/`GEN_QUIETS` semantics and
emission order are unchanged. Quiescence and ProbCut retain their old generators,
ordering, SEE thresholds, delta pruning and TT exceptions. Checked main nodes
retain eager `GEN_EVASIONS`. Root ordering is unchanged.

The TT move is pseudo-legally validated at the probe; refutations at delivery.
Unused promotion bits on non-promotion encodings are rejected, so direct moves
cannot bypass the canonical generator membership the old list supplied.
King safety stays in the common search loop before `moveCount` and make-move.
Returned refutations, the TT move and the excluded move are suppressed from
later batches by full encoded identity, even if a returned move was pruned.

Picker state is on the C invocation stack, NOT `SearchThread.picker[ply]`:
singular verification calls search again at the same ply. The buffer is not
cleared on entry and does not allocate. The bad-capture and quiet ranges share
one `MAX_MOVES` buffer. A GCC native `-O3 -fstack-usage` check reported about
5 KB for negamax; thread.c reserves 8 MiB per search thread. This is not a
replacement for sanitizer checks of deep searches.

Killers/counter are snapshotted when the main picker is constructed, after
ProbCut. Capture history is read when tactical scoring is reached; quiet
histories when quiet scoring is reached. Scores remain frozen within each
batch. No large history snapshot is copied. The search's existing live history
reads for LMR/pruning stay where they were. Move counting, pruning, reductions,
history credit/maluses, singular verification, stop handling, make/undo and NNUE
push/pop are otherwise unchanged. There is no quiet-tail skipping optimisation.

This does NOT preserve the old tree. Descendant searches update histories before
deferred scoring, and stage-local selection swaps break equal-score ties
differently from the old single list. Determinism is required; equality with
the original node count is not. The eager-GENERATION control below does require
node equality because it preserves both stage order and scoring timestamps.

## Verification and controls

From the repository root:

```sh
make EXE=stormbreaker-staged
make EXE=stormbreaker-staged movepick-test perft chess960-test history-test
make EXE=stormbreaker-staged smp-test THREADS=4
make EXE=stormbreaker-staged staged-eager
make EXE=stormbreaker-staged staged-profile
```

`movepick selftest` exercises the actual production picker via an isolated
test bridge, not a copied implementation. It checks 65,536 move encodings per
curated position, generator partitions, duplicate/excluded/invalid candidates,
all promotions, bad captures, same-ply lifetime, history-scoring timing and
stage laziness. Picker-driven perft runs all four EPD suites (depth 3, depth 2
for all 960 starts), with deterministic random legal walks as additional input.
Ordinary perft retains the independently sealed counts and its original code.

The `-eager` binary generates both normal-node batches on construction but
scores at the same times as the lazy binary. Its node count must match exactly.
Do not use it as the SPRT baseline: use the preserved original engine.

The `-profile` binary adds per-thread counters only under `MOVE_PICKER_PROFILE`.
It prints after helpers park and leaves the final bench line intact. It separates
main tactical/quiet/evasion, qsearch and ProbCut generation/scoring; reports
main picked/legal/searched moves, ordering/pruning SEE calls, and cutoff stages.
Counts reset per search. These are work counts, NOT sampled CPU cycle costs;
instrumented wall times are not release timings. A TT cutoff can avoid main
generation even though ProbCut already generated captures earlier at that node.

## Local measurements (Windows, native GCC, NNUE f2886d3e2c71)

The original binary was built before source edits as `stormbreaker-staged-base.exe`.
The generator/validation-only preparation retained its depth-10 count exactly.

| Build | Depth 10 nodes | Depth 12 nodes | Depth 13 nodes |
|---|---:|---:|---:|
| Original defc301 | 1,268,268 | 3,448,787 | 5,016,992 |
| Staged | 1,452,486 | 3,519,471 | 5,589,919 |
| Eager-generation control | same staged tree | 3,519,471 | 5,589,919 |

Four alternating depth-13 runs, elapsed ms (bench's own timer):

| Original | Eager control | Lazy staged |
|---:|---:|---:|
| 2009 | 2213 | 2182 |
| 2084 | 2207 | 2183 |
| 2024 | 2240 | 2175 |
| 1993 | 2199 | 2176 |

Median NPS is about 2.488M original, 2.529M eager, 2.565M lazy. That is a modest
~3.1% NPS gain over original and ~1.4% over the matched-tree control, not a
strength result or a statistically established cross-machine speedup. The
staged tree is larger at depth 13 and takes LONGER than original to finish.

At depth 12, 826,965 nodes constructed a main picker, including 84,395 checked
nodes. Of the remaining 742,570, only 346,009 generated quiets: **53.4% avoided
quiet generation/scoring**. Tactical generation was reached 610,804 times.
Qsearch generated at 451,095 nodes and ProbCut at 32,813; those remain eager.
Raw local logs are gitignored in build/staged-profile.txt and build/staged-timing.json.

Release/debug picker gates, all four ordinary perft suites, Chess960 structural
tests, history and 4-thread SMP gates passed. GCC native, GCC popcnt, Clang and
debug agree on the staged depth-10 count. Classical and tuning builds passed
the picker gate, including maximum pawn-history weight/minimum capture-history
divisor. NNUE verification matched all 10,000 existing reference vectors.
OpenBench compliance and the datagen round-trip/relabel gate also passed.
ASan/UBSan are wired into Linux CI; they were NOT run locally (MinGW has no
ASan runtime and the available WSL distribution is Docker's internal one).

## Owner-run STC — provisional keep, not a passed SPRT

At 8+0.08 with normalized bounds [0,5], the owner reported **+5.72 ± 4.96 Elo**,
LOS 98.8%, and **LLR +1.920** against stopping boundaries of ±2.944, then elected
to stop for machine-time budget. The panel showed 6,280 games; its W/L/D totals
account for 6,260. [E36](EXPERIMENTS.md#e36-full-main-search-staged-generation--provisional-keep-stc-stopped-undecided)
records the supplied statistics and the count discrepancy in full.

The result favours an improvement and supports a practical provisional keep,
but does not establish a formal SPRT pass, non-inferiority verdict, or proven
+6 Elo gain. The reported interval and LOS do not replace the stopping boundary.
LTC confirmation remains pending. No additional matches are being run by the
coding agent.

## Manual SPRT commands for future testing — do not rebuild the baseline

Both named binaries have been built already. These explicit paths avoid the
runner choosing an unrelated newest baseline. `sprt` does not rebuild engines.
The coding agent checked the command syntax/configuration with `--dry-run` only;
the owner subsequently ran the STC reported above. The book is the
repository-configured opening book. These commands start new tests, not a resume
of the stopped run; further testing is deferred until machine time is available.

```powershell
make -C "c:/Users/colli/Desktop/Small_programing_stuff/ChessEngine" sprt ARGS="--dev c:/Users/colli/Desktop/Small_programing_stuff/ChessEngine/stormbreaker-staged.exe --base c:/Users/colli/Desktop/Small_programing_stuff/ChessEngine/stormbreaker-staged-base.exe --tc STC --concurrency 8"

# Only after STC passes; run separately, not simultaneously:
make -C "c:/Users/colli/Desktop/Small_programing_stuff/ChessEngine" sprt ARGS="--dev c:/Users/colli/Desktop/Small_programing_stuff/ChessEngine/stormbreaker-staged.exe --base c:/Users/colli/Desktop/Small_programing_stuff/ChessEngine/stormbreaker-staged-base.exe --tc LTC --concurrency 8"
```

Adjust concurrency to your machine budget. Both default to one engine thread
and 16 MB hash. STC is 8+0.08 with normalized bounds [0,5]; LTC is 40+0.4 with
bounds [0.5,4.5]. Do not run other heavy work during the match, stop early based
on a favourable result, or promote this patch as an Elo gain without a verdict.

Baseline SHA-256:
`f07f956b98d51fbc2455befd1a30e5d31c834308925d573c970ae13b3ee73aa9`.

Future qsearch/ProbCut staging, quiet-tail skipping and changes to capture SEE
are separate experiments, not hidden parts of this patch.