# trainer

The NNUE trainer is a separate Python project with its own virtual environment.
See [../docs/NNUE.md](../docs/NNUE.md) for the network design; this document
covers setup and operation.

```
nnue/format.py    the 32-byte record, and the features the network sees
nnue/dataset.py   batches: memmap slices, or shuffled chunks at size
nnue/model.py     the network and the loss
nnue/train.py     the training loop
nnue/sanity.py    score positions whose evaluation is known
tests/            the gates, run by `make trainer-test`
```

---

## Setup

```powershell
pwsh tools\trainer-setup.ps1          # creates trainer\.venv, installs torch
pwsh tools\trainer-setup.ps1 -CudaTag cpu     # no NVIDIA GPU
```

The script selects an interpreter with an available PyTorch wheel and installs
PyTorch from a CUDA index. The PyPI `torch` wheel for Windows is CPU-only. The
setup output reports whether CUDA is available.

Run everything from this directory so `nnue` is importable:

```powershell
cd trainer
.venv\Scripts\python.exe -m nnue.train --help
```

---

## The pipeline

```powershell
# 1. generate. One process per worker; each writes its own shard.
#    Tree sampling is OFF by default: `-tree 1` makes roughly half the records
#    interior search nodes rather than game-line positions, `-tree 2` more.
#    -resume checkpoints every 30s at a game boundary and picks a killed run
#    back up - a generation run is days, and it WILL be interrupted.
#    See `datagen selfplay -help`.
.\datagen.exe selfplay -o external\data\shard%02d.cnn -games 20000 -nodes 100000 -threads 14 -resume

# 2. label games that already exist. `tuner extract` turns PGNs into quiet
#    FENs carrying the game result; `datagen label` searches each one.
#    -resume makes the command safe to repeat: it checkpoints every 30s and
#    picks up where an interrupted run stopped, instead of starting the whole
#    multi-hour pass again. Note that -threads 14 writes human00.cnn ..
#    human13.cnn, not human.cnn - shards concatenate, and step 4 globs them.
.\tuner.exe extract external\training\*.pgn -o external\training\human.epd
.\datagen.exe label external\training\human.epd -o external\data\human.cnn `
    -source human -nodes 100000 -threads 14 -resume

# 3. prove the data before training on it. -relabel re-searches from scratch,
#    so it needs the SAME -nodes the shard was written with - every record
#    looks wrong otherwise. The shard's .json says which.
.\datagen.exe verify external\data\shard00.cnn -relabel 500 -nodes 100000
.\datagen.exe stats  external\data\shard00.cnn

# 4. shuffle on disk, across every shard of every source. Not optional - see
#    below. This pass also DEDUPLICATES, and it is the only one that can:
#    workers are separate processes, so a position two of them reach is in
#    the dataset twice until they meet here. It reports how many it dropped.
.\datagen.exe shuffle external\data\shard*.cnn external\data\human*.cnn `
    -o external\data\train.cnn -seed 1

# 5. hold a shard back, then fit
cd trainer
.venv\Scripts\python.exe -m nnue.train `
    --train ..\external\data\train.cnn `
    --val   ..\external\data\val.cnn `
    --epochs 10 --batch-size 16384 --out ..\external\nets\net

# Gen 1 net ~2800 -> ~3000 Elo
.venv\Scripts\python.exe -m nnue.train --train ..\external\data\gen-001.cnn --val ..\external\data\val.cnn --epochs 3 --out ..\external\nets\net --output-buckets 4 --lr 0.001 --lambda-start 0.9 --lambda-end 0.9 --hidden 1024
# Gen 2 + 1 hybrid net ~3000 -> ~3050 Elo
.venv\Scripts\python.exe -m nnue.train --train ..\external\data\gen-001.cnn ..\external\data\gen-002.cnn --val ..\external\data\val.cnn --epochs 3 --out ..\external\nets\net --output-buckets 4 --lr 0.0005 --lambda-start 0.9 --lambda-end 0.9 --hidden 1024
# Gen 3 ~3100 -> ~3300 Elo
.venv\Scripts\python.exe -m nnue.train --train ..\external\data\gen-003.cnn ..\external\data\gen-004.cnn --val ..\external\data\val.cnn --epochs 4 --out ..\external\nets\net --output-buckets 4 --lr 0.0005 --lambda-start 0.95 --lambda-end 0.95 --hidden 512 --uncertainty

# 6. quantise the checkpoint into the file the engine embeds
cd ..
make nnue-export        # net.pt -> net.nnue, plus .vectors and .sha256

# 7. build the engine carrying it, and prove the C inference reproduces the
#    reference. nnue-test re-exports and rebuilds first, so it is also the
#    shortest way to do step 6 and this one in a single command.
make                    # stormbreaker.exe - the default build carries the net
make nnue-test          # 10,000 positions, exact equality, non-zero on a diff
make nnue-info          # which net the binary is actually carrying, by hash
```

`train.py` writes `net.pt`, `net-history.json`, and prints the sanity table
after every epoch.

`datagen <subcommand> -help` lists every option with its default — the
adjudication thresholds, the tree-sampling knobs, the filters and the dedup
table size are all there, and none of them are in the one-line usage.

**The shuffle is not optional.** Positions from one game are correlated, and a
batch drawn from one region of an unshuffled file is a batch of near-duplicates.
A DataLoader shuffle buffer does not fix it: a 100k-record window over a file
whose consecutive records come from the same game is not a shuffle. Glob every
source into it, too: `shard*.cnn` on its own quietly trains on self-play only,
and the loss curve of a run missing half its data looks exactly like the loss
curve of a run that has it.

**It is also where duplicates die.** `selfplay` and `label` dedup within a
shard, but their workers are separate processes that share nothing, so a
position two of them reach is written twice and nothing downstream looks.
`gen-004` is 122.7M records over **11.7M distinct positions** - ten copies of
everything - because the build that made it seeded workers so that each
replayed its neighbours' games. Its loss curve looked completely normal.
Shuffle now drops duplicates by default and says how many; `-nodedup` makes it
a pure permutation again. Read the number it prints: a large one on a fresh
generation means the workers are not playing different games.

---

## Labelling games you already have

Step 2 is optional and worth doing anyway — human and strong-engine positions
are structurally unlike self-play, and they cost nothing but the labelling
pass. `external/training/` already holds the extracted EPDs, so `tuner extract`
is only needed for PGNs that are not in there yet.

`datagen label` reads one FEN per line, optionally followed by `[1.0]`,
`[0.5]` or `[0.0]` — the result from **white's** point of view, which is what
`tuner extract` writes. A line with no result still labels; its WDL is recorded
as unknown, and the trainer drops the game-result term for that record rather
than pretending it into a draw.

Two flags matter more than the rest:

- **`-source`** — one of `selfplay tree human engine book other`. It is stored
  per record, and it is what lets `--sources` train a run on part of the
  mixture (`--sources 0 1` is self-play line plus tree samples) without
  regenerating anything. It defaults to `other`, which tells a later run
  nothing; set it.
- **`-nodes`** — must match what the other shards were labelled at. A
  self-play position and a human one mean the same thing only if they were
  labelled the same way, which is why the label is a fixed-node search from a
  cleared engine rather than whatever the search happened to have handy.

`-threads N` splits the input by line, so any worker count reads the same lines,
and the output pattern gains a `%02d` on its own: `-o external\data\human.cnn
-threads 14` writes `human00.cnn` through `human13.cnn`. Those are ordinary
shards — verify them and shuffle them in like any other, remembering that
`verify -relabel` needs the `-nodes` this pass used, not its own default.

**We label other engines' positions with our own search, never with their
evaluations.** A net trained on Stockfish's output is a derivative of
Stockfish; a net trained on positions Stockfish happened to play, scored by us,
is not. See [../docs/NNUE.md](../docs/NNUE.md#position-sources).

---

## Getting the net into the engine

Steps 6 and 7 need no arguments because step 5's `--out ..\external\nets\net`
lands on their defaults: `make nnue-export` quantises `NET`
(`external/nets/net.pt`) into `EVALFILE` (`external/nets/net.nnue`) and writes
`.vectors` and `.sha256` beside it. Another run has to name both, and the
variables carry into the sub-make:

```powershell
make nnue-export NET=external/nets/run7.pt EVALFILE=external/nets/run7.nnue
make nnue-test   NET=external/nets/run7.pt EVALFILE=external/nets/run7.nnue
make             EVALFILE=external/nets/run7.nnue    # stormbreaker.exe
```

Forward slashes and no spaces: `EVALFILE` is embedded as an assembler string
literal, so a Windows path breaks it — `\net` becomes a newline rather than a
directory.

**The net is linked in, not loaded at run time.** Re-exporting therefore does
nothing at all to a binary that already exists. `make` gets this right —
`EVALFILE` is a prerequisite of the engine — but nothing stops you from
exporting and then benching yesterday's `stormbreaker.exe`. `make
nnue-info` prints the hash the exporter printed; if those two disagree, the
binary is stale and every number it produced is about a different net.

`make nnue-test` is a gate, not a formality. It requires `src/nnue.c` to equal
the quantised numpy reference **exactly** on all 10,000 vectors and exits
non-zero on a disagreement of one, which is why it works as a CI check. Run it
after every export: it is the only thing that checks the net the engine plays
with is the net that was trained.

`make` builds the network engine as `stormbreaker.exe`, and `make classical`
puts the hand-written evaluation in `stormbreaker-classical.exe` beside it
rather than over it — comparing the two is still worth doing, and that is hard
when the second build overwrites the first. `make <target> EVAL=classical` —
`make bench EVAL=classical`, say — does not get that second name: it builds the
classical engine as plain `stormbreaker.exe`, over the network one. The rebuild
itself is correct, since a `.buildflags` stamp sees the flags change, but the
binary left sitting there afterwards is not the one its file name implies.

---

## The gate

Task 2 is done when the held-out loss curve is sane **and** the sanity table
looks right. The second matters more, because the first cannot see the bug that
actually happens:

```powershell
cd trainer
.venv\Scripts\python.exe -m nnue.sanity ..\external\nets\net.pt
```

```
position                   score    flip    null  expected
----------------------------------------------------------
start position                 8       8      -6         0
white minus a knight        -287    -287     291      -300
...

flip: largest |score - flip| = 0.0 cp (exact, as it must be)
null: 6/6 decisive positions flip sign when the turn is passed (expected)
```

Three things to read off it, in decreasing order of how certain they are:

- **`flip` must equal `score` exactly.** The flip swaps both colours, the ranks
  and the side to move, so it is the same position seen from the other side —
  and the score is side-to-move relative, so it must come back identical. This
  is structural, not learnt: it holds on random weights, and any non-zero
  difference means the feature normalisation is not symmetric. (It is *not* the
  "negate the score" transform; that is what `null` is for.)
- **`null` should be roughly `-score`.** Same board, other side to move. This
  is the check that catches the bug `flip` cannot: reading the *non*-side-to
  -move accumulator first is still perfectly colour-symmetric and still
  evaluates every position from the opponent's point of view. It only means
  something once the net has learnt enough for the signs to be real, which is
  why the summary says so when there is not enough signal yet.
- **the right sign and order of magnitude** on the material rows, and the start
  position near zero.

---

## What the network is

```
feature transformer   24576 -> H, shared weights, one accumulator per side
concatenate           [stm accumulator ; non-stm accumulator] -> 2H
activation            SCReLU, clamp(x, 0, 1)^2
output                2H -> B, the row selected by piece count
```

The feature set is `(32 mirrored king squares, piece, square)` over both
colours' pieces, and the activation is SCReLU. Neither is a flag: `src/nnue.c`
implements one of each and rejects anything else by name. Only the shape is a
choice, and both halves of it are header fields the engine reads out of the net
file, so changing either is a retrain and an export with no C change:

| Flag | Values | Default |
|---|---|---|
| `--hidden` | a multiple of 16, up to 2048 | 1024 |
| `--output-buckets` | any divisor of 32 | 4 |

24576 feature rows is a lot to fit. On a few million positions most rows are
seen a handful of times, and `--hidden 512` is what to trade down first.

The normalisation is the classical evaluation's, verbatim — rank-flip for the
perspective's owner, file-mirror when that king sits on the kingside — after
which a king stands on one of exactly 32 squares. That reuse is the payoff for
having built the linear model as a factorised HalfKA: the feature extraction
already existed and was already the thing the tuner fits.

The checkpoint records its own shape, so `make nnue-export` is never told what
it is holding. A checkpoint without an `arch` field predates this network and
is **refused**, not guessed at — it was trained on a different feature set with
a different activation, and there is nothing in the file that says which.

**Weights are clipped after every step**, to the range the engine's integer
arithmetic can represent (`WEIGHT_CLIP` in `nnue/format.py`). That is not
regularisation: the engine's SCReLU multiplies the clamped activation by the
weight as int16, and the export refuses a net that would wrap.
`--no-weight-clip` turns it off, which is useful for measuring what it costs
and for nothing else.

### Units, and why they are fixed here

The float model's output is in units where **1.0 = 400 centipawns**
(`NET_TO_CP`). That is not arbitrary: it is what the Task 3 quantisation
produces, since `eval_cp = raw * SCALE / (QA * QB)` with `SCALE = 400`. Pinning
the two definitions together now means the exporter is a pure re-scaling rather
than a re-interpretation, and a mismatch there is worth about 30 Elo and looks
like nothing at all.

The training target is

```
target = lambda * sigmoid(score / K) + (1 - lambda) * wdl
```

with `K = --sigmoid-k` in centipawns (default 400, the scale the classical
evaluation was fitted at) and `lambda` annealed from `--lambda-start` to
`--lambda-end` (both 0.9) across the run. Records whose game result is unknown
— a labelled EPD with no `[x.x]` on it, or a game the ply cap stopped — use
`lambda = 1` for that record rather than being dropped or being pretended into
a draw.

`lambda` is a real hyperparameter and worth two or three runs, not a guess to
be lived with. Record what each one scored in
[../docs/EXPERIMENTS.md](../docs/EXPERIMENTS.md).

### lambda per record, not per run

`lambda` prices the game result against the search score, and **that price is
not the same for every record**. A result twenty moves away is nearly a coin
flip about the position in front of it; three plies away it is the truth. The
search score's *systematic* errors — fortresses, compensation, an ending it
cannot convert — are what the result term exists to correct, and they live in
the endgame. A tree sample's game result belongs to a game that never went
through it — the one outright mislabel. A `human` or `engine` record's result
is real, but it is someone else's continuation.

One number for all of that pays the average of prices that differ by a lot. So
the base lambda above is a starting point that per-record dials move:

| Flag | Effect | Try |
|---|---|---|
| `--lambda-progress DELTA` | shift lambda by `DELTA` at the end of a game, tapering to zero 112+ plies away | `-0.2` |
| `--lambda-pieces DELTA` | shift lambda by `DELTA` at bare kings, tapering to zero at a full board | `-0.1` |
| `--lambda-source NAME=V` | override lambda outright for a source tag | `tree=1.0` (the default) |
| `--lambda-min` / `--lambda-max` | bounds the deltas are clipped to | |
| `--score-clip CP` | clamp the label before the sigmoid | `3000` |
| `--source-weight NAME=W` | per-record loss weight by source | `tree=0.5` |

They compose additively on top of the epoch anneal and are then clipped; a
source override replaces the result; an unknown game result is always
`lambda = 1`, whatever the dials say. **Every dial defaults to no effect except
`--lambda-source`**, which defaults to `tree=1.0` — and that is a no-op on any
shard made with `-tree 0`, datagen's default. Passing `--lambda-source`
replaces that default rather than adding to it; `''` turns it off.

`--lambda-progress` reads the record's game-progress nibble, which
`datagen selfplay` writes and `datagen label` cannot (an EPD line carries a
result but never the game it came from). Those records keep the base lambda
rather than being guessed at, so mixing shard types is safe.

`--score-clip` is the one to reach for on a gen-5 shard specifically. A mate or
tablebase score is a **proven result**, not an evaluation, and
`sigmoid(31500 / 400)` is 1.0 to every digit a float carries — a target no
clipped-weight net can reach and can only chase. `-maxscore 2000` used to drop
those records; now that games run to mate and tablebases label the endings they
are a real share of the data. `datagen stats` prints it as `decisive scores`.

Every epoch line reports the lambda the schedule asked for and, when they
differ, the one the batches actually got:

```
epoch   3  lambda 0.93 (applied 0.812)  lr 5.12e-04  train 0.041  val 0.043
```

If the second number is not roughly where it was meant to be, that is a flag
typo, and nothing else in the run would report it.

---

## Useful flags

| Flag | Why |
|---|---|
| `--limit-batches 20` | smoke-test the whole loop in seconds |
| `--sources 0 1` | train on self-play line + tree samples only |
| `--workers 6` | four to six saturates the loader; more does nothing |
| `--batch-size 32768` | bigger batches keep the 3070 busier — worth ~7% |
| `--hidden 512` | narrower and faster; what a small dataset wants |
| `--output-buckets 1` | one output row for every phase |
| `--no-weight-clip` | measure what the clip costs; the export will then refuse the net |
| `--device cpu` | force CPU, e.g. to reproduce a CI failure |
| `--seed 1` | shuffling and initialisation, so a run repeats |
| `--positions-per-epoch 50M` | see [Datasets too big for one epoch](#datasets-too-big-for-one-epoch) |
| `--resume` | continue an interrupted run from its last epoch |
| `--chunk-records 0` | force memmap slices at any dataset size |
| `--lambda-progress -0.2` | trust the game result more as the game's end approaches |
| `--lambda-source tree=1.0` | train a source on its search score alone, without dropping it (default) |
| `--score-clip 3000` | stop mate and tablebase labels asking for infinite confidence |
| `--source-weight tree=0.5` | set the mixture without regenerating |

Expect tens of minutes per 100M-position epoch on an RTX 3070, and 5–15 epochs.
At `--hidden 1024 --workers 6` that is about **390k positions/s**.

On the reference machine, the loader is not the bottleneck. It delivers
**1.2M positions/s** — three times what training consumes — because 1.2M
positions/s is only 38 MB/s of records, and the work is `unpack()` on the CPU
rather than anything the drive does. So raise `--workers` until it stops
helping, which is four to six, and then stop: past that the numbers that move
are `--batch-size` (32768 is worth about 7% over 16384) and `--hidden`.

---

## Tests

```powershell
make trainer-test          # generates a small shard first, then runs these
```

or directly:

```powershell
cd trainer
.venv\Scripts\python.exe -m pytest tests -q --shard ..\external\datagen-test\all.cnn
```

What they actually check:

- **`test_format.py`** — the contract between `tools/datagen.c` and
  `nnue/format.py`. It decodes the same records twice, once with `datagen dump`
  and once here, and requires identical FENs, scores, WDLs, source tags and
  check bits. If these two ever disagree, the network trains on positions that
  are not the positions the engine labelled, the loss curve is normal, and
  nothing else notices.
- **`test_features.py`** — the two symmetries the normalisation must have. A
  position and its file-mirror produce identical features; white's features of
  a position equal black's features of the same position colour-flipped. These
  are the tests that catch a mirror applied to the king but not the pieces, and
  a perspective that reads its own pieces as the enemy's.
- **`test_model.py`** — that the pipeline is connected: a batch reaches the
  model, the loss has a gradient, the optimiser reduces it, the padding slot
  stays pinned at zero, and a net fitted on a symmetric target scores flipped
  positions as negatives of each other.

They run in a few seconds and need no GPU.

---

## What is deliberately not here

*How* `tools/export_net.py` and `src/nnue.c` work — only how to run them,
above. The interface between this directory and them is small on purpose: the
checkpoint is a plain `torch.save` dict — `{"model": state_dict, "hidden":
int, "sigmoid_k": float, ...}` — so the exporter reads the architecture out of
the file rather than out of a constant, and `NNUE.accumulators()` is exposed
precisely so the quantised reference and the C inference can be compared
against the same quantity. The quantisation itself, the net file's header, and
why the equivalence test insists on exact equality are
[../docs/NNUE.md](../docs/NNUE.md), Task 3.
