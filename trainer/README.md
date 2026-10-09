# trainer

The NNUE trainer is a separate Python project with its own virtual environment.
See [../docs/NNUE.md](../docs/NNUE.md) for the network design; this document
covers setup and operation.

```
nnue/format.py    the 32-byte record, and the features the network sees
nnue/dataset.py   batches: memmap slices, or shuffled chunks at size
nnue/sparse.py    the feature transformer as a sparse matrix product
nnue/model.py     the network and the loss
nnue/train.py     the training loop
nnue/sanity.py    score positions whose evaluation is known
nnue/calibrate.py label scale per shard, and validation with each net's scale refitted
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
#    The label is the score the game's own search gave the position, so a
#    position costs one search and not two. -resume checkpoints every 30s at a
#    game boundary and picks a killed run back up. See `datagen selfplay -help`.
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

# 3. prove the data before training on it. -relabel applies to `label` shards,
#    whose scores really are a fresh fixed-node search; a self-play shard is
#    checked by regenerating it with the same -seed instead, and verify refuses
#    -relabel on one by name. The shard's .json `labels` field says which.
.\datagen.exe verify external\data\shard00.cnn
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
# Gen 2 + 1 hybrid net ~3000 -> ~3100 Elo
.venv\Scripts\python.exe -m nnue.train --train ..\external\data\gen-001.cnn ..\external\data\gen-002.cnn --val ..\external\data\val.cnn --epochs 3 --out ..\external\nets\net --output-buckets 4 --lr 0.0005 --lambda-start 0.9 --lambda-end 0.9 --hidden 1024
# Gen 3 + 4 ~3100 -> ~3350 Elo (Additional search improvments contributed to this gain net-0ba56166ba9c)
.venv\Scripts\python.exe -m nnue.train --train ..\external\data\gen-003.cnn ..\external\data\gen-004.cnn --val ..\external\data\val.cnn --epochs 4 --out ..\external\nets\net --output-buckets 4 --lr 0.0005 --lambda-start 0.95 --lambda-end 0.95 --hidden 512 --uncertainty

# Gen 5 ~3350 -> ~3450
.venv\Scripts\python.exe -m nnue.train --train ..\external\data\gen-005.cnn --val ..\external\data\val.cnn --epochs 4 --out ..\external\nets\net --output-buckets 8 --lr 0.0005 --lambda-start 0.95 --lambda-end 0.95 --hidden 512 --uncertainty --lambda-progress -0.4 --lambda-pieces -0.2 --score-clip 2000

# Gen 5 + factorization + finish pass ~3450 -> ~3500 (net-f2886d3e2c71)
.venv\Scripts\python.exe -m nnue.train --train ..\external\data\gen-005.cnn --val ..\external\data\val.cnn --epochs 4 --checkpoint-every 1 --out ..\external\nets\net --output-buckets 8 --lr 0.0005 --lambda-start 0.95 --lambda-end 0.95 --hidden 512 --uncertainty --lambda-progress -0.2 --lambda-pieces -0.0 --score-clip 2000 --unc-weight 0.01 --feature-factorization --finish-epochs 1 --finish-lr 0.00001

# Gen 6 + search improvements ~3500 -> ~3550 elo
.venv\Scripts\python.exe -m nnue.train --train F:\chessData\gen-006.cnn --val ..\external\data\val.cnn --out ..\external\nets\net-gen-6-pw --epochs 24 --finish-epochs 1 --finish-lr 1e-5 --batch-size 16384 --hidden 512 --l1-size 16 --l2-size 32 --output-buckets 8 --feature-factorization --uncertainty --unc-weight 0.01 --lr 0.0005 --lr-gamma 0.87 --lambda-start 0.95 --lambda-end 0.95 --sigmoid-k 400 --lambda-progress -0.2 --lambda-pieces 0 --score-clip 2000 --positions-per-epoch 500000000 --workers 4 --seed 0 --log-every 50 --checkpoint-every 1 --pairwise

# Gen 6 + gen-6-u, unc-weight 0.0005 ~3550 -> ~3575 (net-gen-6-noprior, +25.41 +/- 8.68 STC vs gen-6-pw, E49)
.venv\Scripts\python.exe -m nnue.train --train F:\chessData\gen-006.cnn F:\chessData\gen-006-u.cnn --val ..\external\data\val-006.cnn --out ..\external\nets\net-gen-6-noprior --epochs 24 --finish-epochs 1 --finish-lr 1e-5 --batch-size 16384 --hidden 512 --l1-size 16 --l2-size 32 --output-buckets 8 --feature-factorization --uncertainty --unc-weight 0.0005 --lr 0.0005 --lr-gamma 0.87 --lambda-start 0.95 --lambda-end 0.95 --sigmoid-k 400 --lambda-progress -0.2 --lambda-pieces 0 --score-clip 2000 --positions-per-epoch 500000000 --seed 0 --log-every 50 --checkpoint-every 1 --pairwise --workers 8

# E52 ~3575 -> ~3600 (-7 -> +24 vs berserk-8.5). Two runs, not one: see "Training a net that ships" below.
# Pretrain on gen-005 (scores scaled onto gen-006's) + gen-006 + gen-006-u, then a 45-minute finish at lambda 0.75
.venv\Scripts\python.exe -m nnue.train --train ..\external\data\gen-005.cnn F:\chessData\gen-006.cnn F:\chessData\gen-006-u.cnn --score-scale gen-005.cnn=1.122 --val ..\external\data\val-006.cnn --out ..\external\nets\net-g6-P1-pre --epochs 24 --batch-size 16384 --hidden 512 --l1-size 16 --l2-size 32 --output-buckets 8 --feature-factorization --uncertainty --unc-weight 0.0005 --lr 0.0005 --lr-gamma 0.87 --lambda-start 0.95 --lambda-end 0.95 --sigmoid-k 400 --lambda-progress -0.2 --score-clip 2000 --positions-per-epoch 500M --seed 0 --checkpoint-every 4 --pairwise --workers 8
.venv\Scripts\python.exe -m nnue.train --train F:\chessData\gen-006.cnn F:\chessData\gen-006-u.cnn --val ..\external\data\val-006.cnn --out ..\external\nets\net-g6-P1 --init-from ..\external\nets\net-g6-P1-pre.pt --epochs 5 --lr 2e-5 --lr-gamma 0.5 --batch-size 16384 --unc-weight 0.0005 --lambda-start 0.75 --lambda-end 0.75 --sigmoid-k 400 --lambda-progress -0.2 --score-clip 2000 --positions-per-epoch 500M --seed 0 --workers 8

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
opening knobs, the filters and the dedup table size are all there, and none of
them are in the one-line usage.

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

## Training a net that ships

The pipeline above produces data. This section is the process that turns data
into a net worth pinning: what to run, in what order, and why each step is
there. It is the recipe E52 measured (October 2026, gen-006 data). That recipe
took the engine from **-7 to +24 Elo against berserk-8.5** without touching the
architecture or the search. Re-measure before trusting it on a new generation;
the numbers below say which parts are measured and which are carried over.

### The short version

```
check label scales  ->  pretrain from scratch  ->  short finish, lambda 0.6-0.75
     (calibrate)          (all data, ~3.5 h)          (newest data, ~45 min)
                                                              |
ship  <-  confirm vs berserk  <-  SPRT vs current best  <-  export, gate,
          (idle machine)          (STC, 3000-game cap)       re-centre UncSigma
```

### 1. Check the label scale of every shard you are going to mix

Each datagen generation is labelled by the net that generation's engine
carried, and **a net trained with this target comes out ~13% more confident
than the labels it was trained on**. The next datagen inherits that. So shards
from different generations disagree about how big the same advantage is:

| shard | label scale relative to gen-006 |
|---|---|
| gen-005 | 0.892x |
| gen-006 | 1 |
| gen-006-u | 1.137x |

Mixed raw, the net cannot fit all of them, and its confidence drifts toward
whichever shard its last few million positions came from. Measure the scales
with any recent net as the ruler (the ratios barely depend on which):

```powershell
.venv\Scripts\python.exe -m nnue.calibrate scale ..\external\nets\net-g6-P1.pt `
    F:\chessData\gen-006.cnn ..\external\data\gen-005.cnn
```

The last column is the factor that puts each shard on the first one's scale.
Pass it as `--score-scale gen-005.cnn=<factor>`. E52 ran 1.122; this command
reads 1.121, and the difference is noise. Mates and tablebase scores are left
alone, and validation is never rescaled.

### 2. Pretrain from scratch on everything

The main stage is the long run. Use all the data you have, older generations
included, scale-matched. gen-005's positions added **+10 Elo in self-play and
+14 against berserk** on top of gen-6 alone (E52, P1), even though gen-005 came
from a weaker engine. Coverage is worth more than label quality at the margin.

```powershell
.venv\Scripts\python.exe -m nnue.train `
    --train ..\external\data\gen-005.cnn F:\chessData\gen-006.cnn F:\chessData\gen-006-u.cnn `
    --score-scale gen-005.cnn=1.122 `
    --val ..\external\data\val-006.cnn --out ..\external\nets\net-g6-P1-pre `
    --epochs 24 --lr 0.0005 --lr-gamma 0.87 --positions-per-epoch 500M `
    --hidden 512 --l1-size 16 --l2-size 32 --pairwise --output-buckets 8 `
    --feature-factorization --uncertainty --unc-weight 0.0005 `
    --lambda-start 0.95 --lambda-end 0.95 --lambda-progress -0.2 --sigmoid-k 400 `
    --score-clip 2000 --batch-size 16384 --workers 8 --seed 0 --checkpoint-every 4
```

About 3.5 hours on the 3070 when nothing else is running, and noticeably longer
beside a match. No `--finish-epochs` here: the finish is step 3, as a separate
run.

Open question: this stage still runs at lambda 0.95. Whether a lower lambda
here, and not only in the finish, gains more is the next experiment.

### 3. Finish at lambda 0.6-0.75 on the newest data

A short fine-tune from the pretrained checkpoint does two jobs. It anneals the
LR to near zero, and it moves the target toward the game result.

```powershell
.venv\Scripts\python.exe -m nnue.train `
    --train F:\chessData\gen-006.cnn F:\chessData\gen-006-u.cnn `
    --val ..\external\data\val-006.cnn --out ..\external\nets\net-g6-P2 `
    --init-from ..\external\nets\net-g6-P1-pre.pt `
    --epochs 5 --lr 2e-5 --lr-gamma 0.5 --positions-per-epoch 500M `
    --unc-weight 0.0005 --lambda-start 0.6 --lambda-end 0.6 --lambda-progress -0.2 `
    --sigmoid-k 400 --score-clip 2000 --batch-size 16384 --workers 8 --seed 0
```

One pass over the data (5 x 500M), LR 2e-5 halving every epoch, about 45
minutes. `--init-from` brings the architecture with it; the target flags do
not, so repeat them.

On the gen-6-only net, 0.6 beat 0.75 by 13 Elo (E52's F4). On the
gen-005-pretrained checkpoint, this exact run (**P2**) measured +2 +/- 7 over
the same finish at 0.75 (P1). Against berserk-8.5 they were +19.7 and +23.8: no
difference that could be measured. Either works. 0.75 keeps more of the
uncertainty head's signal; its matched UncSigma slope is 5 against 3 (step 6).

**The game-result weight is the biggest single lever found so far.** The
labels come from a 10,000-node search, a shallow teacher, and the game result
knows things its score does not. Fine-tuning the same net at each base lambda
(the applied mean is ~0.07 lower, because of `--lambda-progress`):

| base lambda | 1.0 | 0.95 (old recipe) | 0.75 | **0.6** | 0.45 |
|---|---|---|---|---|---|
| Elo vs the 0.95 net | -25 | 0 | +22 | **~+36** | ~+26 |

What did *not* help, so you need not re-try it: an extra low-LR pass on the old
target (-7), sigmoid K matched to the data (-2), and averaging epoch checkpoints
(worse validation at every window).

### 4. Screen offline before spending an SPRT

Raw validation loss **does not track Elo**. It mostly measures how confident a
net happens to be that epoch: E52's nets swung +-3% between epochs on a 0.1%
weight change. Refit each net's scale first, then read the WDL column, which
scores the eval as a predictor of the game result:

```powershell
.venv\Scripts\python.exe -m nnue.calibrate refit ..\external\data\val-006.cnn `
    ..\external\nets\net-g6-P1.pt ..\external\nets\net-g6-P2.pt
```

Across E52's nets, about **0.001 lower WDL MSE was worth ~23 Elo**. Its limits
are measured too:
- It stops tracking Elo below a base lambda of ~0.6. Lambda 0.45 had the best
  WDL MSE and was no better than 0.75.
- It is blind to gains from data unlike the validation set. gen-005's +10 read
  as zero.

Use it to drop clear losers. It does not replace step 7.

### 5. Export and gate

```powershell
cd ..
make nnue-test ARGS="external/nets/net-g6-P2.pt" EVALFILE=external/nets/net-g6-P2.nnue EXE=sb-p2
```

Exact on 10,000 vectors, or it does not go further (see "Getting the net into
the engine" below).

### 6. Re-centre the uncertainty constants for the new net

The search scales its pruning margins by the uncertainty head's prediction,
through `UNC_SIGMA_BASE` / `UNC_SIGMA_SLOPE` in `src/search.c`. Those constants
are centred on one net's head. A retrained head reads differently: lambda 0.75
put its mean prediction at 115 cp against 83. Under the old constants the new
net plays with different margins, and its SPRT measures net and margins
together. Match the new net's margin distribution to the current net's:

```powershell
# once, on the net the constants were fitted for
make unc-probe EVALFILE=external/nets/net.nnue EXE=sb-ref "PROBE_ARGS=-o external/nets/ref.unc"
# per candidate: prints "UNC_SIGMA_BASE b   UNC_SIGMA_SLOPE s" under "matched by"
make unc-probe EVALFILE=external/nets/net-g6-P2.nnue EXE=sb-p2 "PROBE_ARGS=-ref external/nets/ref.unc"
# build the candidate with them (they are not UCI options in a playing build)
make EXE=sb-p2u EVALFILE=external/nets/net-g6-P2.nnue `
    "INCLUDES=-Isrc -DUNC_SIGMA_BASE_DEFAULT=<b> -DUNC_SIGMA_SLOPE_DEFAULT=<s>"
```

Pass the defines through `INCLUDES=`; a command-line `CFLAGS=` replaces the
Makefile's own flags. The bench changes, which is how you know they took. As
lambda drops, the head reads wider and the matched slope falls: 98/5 at lambda
0.75, 102/3 at 0.6, 104/2 at 0.45, against the shipped 100/7.

### 7. Test: self-play first, then an external engine

1. **SPRT the candidate against the current best**, both binaries matched as
   in step 6. Use STC, bounds [0, 5], `--rounds 1500` (3000 games at most).
   Comparing against the current best rather than the shipped net isolates the
   one change:
   `make sprt ARGS="--dev sb-p2u.exe --base <current best>.exe --tc STC --bounds 0,5 --rounds 1500"`.
2. **Confirm against berserk-8.5 over 3000 fixed games**, on an idle machine:
   `make gauntlet ARGS="--engine sb-p2u.exe --opponents external/engines/berserk-8.5.exe --tc STC --games 3000"`.
   Not beside training, datagen or another match: berserk's time management
   keeps far less slack than ours, and under contention it flags (52 time
   losses in 1694 games, a fake +68 Elo). Grep the log for "loses on time" by
   side before believing the number. Net gains have transferred (+22 self-play
   was +17 against berserk); E50's search gains did not, which is why this step
   exists.

### 8. Ship

Three edits, made together:
1. `tools/publish-net.ps1` uploads the `.nnue` as a release asset and prints
   the replacement `NET_TAG` / `NET_SHA256` lines for the Makefile. Nets are
   content-addressed: the tag is `net-` plus the hash's first 12 digits.
2. Set `UNC_SIGMA_BASE_DEFAULT` / `UNC_SIGMA_SLOPE_DEFAULT` in `src/search.c` to
   the matched values from step 6. The net and its constants are one change;
   the net alone plays with someone else's margins.
3. Copy the net over `external/nets/net.nnue` (or `make net-fetch` once it is
   published), rebuild, and check `make bench` reads what the tested binary
   read.

Then record the run in [../docs/EXPERIMENTS.md](../docs/EXPERIMENTS.md) and
[../docs/STATUS.md](../docs/STATUS.md).

### Before the next datagen

Whatever net labels gen-007 will put its own ~13% inflation into the new
labels. Either stop the inflation at the source (the result term at K=400
pulls toward outcomes that fit K~230), or keep running step 1 every
generation. The second works today.

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
  mixture (`--sources 0 2` is self-play plus human) without regenerating
  anything. It defaults to `other`, which tells a later run nothing; set it.
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

**Use a distinct output prefix for each experiment.** New training runs refuse
to overwrite an existing run; use `--resume` only to continue that run, or
`--init-from` with a new prefix to start a different recipe. Do not reuse
`net.pt` while leaving an older `net.nnue` beside it and assume they match.

Each run writes `<prefix>-run.json` with a run ID, resolved data paths,
file sizes/mtimes and generation-manifest hashes, trainer source hashes,
PyTorch version, arguments, and the current checkpoint SHA-256. The large
training shards themselves are **not** content-hashed. Checkpoint and optimizer
state are cross-checked by epoch, run ID, and checkpoint hash on resume.

The export JSON links the **checkpoint SHA-256** to the **network SHA-256**,
architecture, stage, factorization setting, and metrics. Default header tags
include a checkpoint hash prefix. Re-exporting the same checkpoint is allowed;
replacing a different/unverified export requires explicit `-f`/`--overwrite`,
through make as `ARGS="-f"`. Prefer unique file names instead.

On a fresh output path, steps 6 and 7 need no arguments because step 5's `--out ..\external\nets\net`
lands on their defaults: `make nnue-export` quantises `external/nets/net.pt`
into `external/nets/net.nnue` and writes `.vectors` and `.sha256` beside it.

Another run is named through `ARGS`, which reaches `tools/export_net.py`
unchanged — the first word is the checkpoint, `-o` the net, and a bare name on
either side means `external/nets/<name>`:

```powershell
make nnue-export ARGS="run7 -o run7"                 # run7.pt -> run7.nnue
make nnue-export ARGS="run7 -f"                      # ... or straight onto net.nnue
```

`make nnue-test` exports to `EVALFILE` and then verifies *that* file's vectors,
so a candidate net names the checkpoint in `ARGS` and the net in `EVALFILE` —
both carry into its sub-make. Do not redirect the export with `-o` there, or it
checks a different file than it wrote:

```powershell
make nnue-test   ARGS="run7" EVALFILE=external/nets/run7.nnue
make             EVALFILE=external/nets/run7.nnue    # stormbreaker.exe
```

Forward slashes and no spaces: `EVALFILE` is embedded as an assembler string
literal, so a Windows path breaks it — `\net` becomes a newline rather than a
directory.

**The default net is linked in.** Re-exporting therefore does nothing at all
to a binary that already exists unless its UCI `EvalFile` is explicitly pointed
at the new network. `make` gets the embedded case right —
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
choice, and every part of it is a header field the engine reads out of the net
file, so changing any of them is a retrain and an export with no C change:

| Flag | Values | Default |
|---|---|---|
| `--hidden` | a multiple of 16, up to 2048 | 1024 |
| `--output-buckets` | any divisor of 32 | 4 |
| `--l1-size` | 0, or a multiple of 16 | 0 |
| `--l2-size` | 0, or a multiple of 16; needs `--l1-size` | 0 |

24576 feature rows is a lot to fit. On a few million positions most rows are
seen a handful of times, and `--hidden 512` is what to trade down first.

**The last two are the layer stack**, which replaces the flat output layer
with `2H -> l1_size -> l2_size -> buckets`, clipped ReLU between stages. It
exists because the flat layer is arithmetically pinched rather than merely
small: `src/nnue.c` forms `v * w` as int16, which caps a quantised output
weight at 128, and the net before it sat on 127. See **Task 6** in
[../docs/NNUE.md](../docs/NNUE.md) for the defect, the integer arithmetic the
stack quantises into, and what was measured.

`--l1-size 0` is the architecture that shipped, so a run that names neither
flag trains exactly what it trained before — and that is the point, because it
is the control a stacked net has to be measured against.

A stacked checkpoint exports and runs like any other: net format version 3
carries the two widths and their requantisation shifts, and `make nnue-test`
holds it to the same exact-equality gate as a flat one. Two things differ, and
`tools/export_net.py` picks both from the checkpoint rather than asking:

- **`--qa` is 256 rather than 255**, and must be a power of two. The stack
  applies the SCReLU rescale per element, to build the vector L1 reads, and per
  element a shift is a shift and a divide is a divide. A stacked net with any
  other `qa` is rejected by name, here and at load.
- **`--qb` is 512 rather than 64.** The int16 SIMD product that caps a flat
  output weight at 128 levels is not in the stack's path at all, so its final
  layer is free to carry a much finer scale.

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
[../docs/EXPERIMENTS.md](../docs/EXPERIMENTS.md). It was 0.95 from gen 3
through gen 6. E52 measured **0.6 as ~36 Elo better** in a finishing
fine-tune, the largest single training gain to date (see "Training a net that
ships").

### lambda per record, not per run

`lambda` prices the game result against the search score, and **that price is
not the same for every record**. A result twenty moves away is nearly a coin
flip about the position in front of it; three plies away it is the truth. The
search score's *systematic* errors — fortresses, compensation, an ending it
cannot convert — are what the result term exists to correct, and they live in
the endgame. A `human` or `engine` record's result is real, but it is someone
else's continuation.

One number for all of that pays the average of prices that differ by a lot. So
the base lambda above is a starting point that per-record dials move:

| Flag | Effect | Try |
|---|---|---|
| `--lambda-progress DELTA` | shift lambda by `DELTA` at the end of a game, tapering to zero 112+ plies away | `-0.2` |
| `--lambda-pieces DELTA` | shift lambda by `DELTA` at bare kings, tapering to zero at a full board | `-0.1` |
| `--lambda-source NAME=V` | override lambda outright for a source tag | `human=1.0` |
| `--lambda-min` / `--lambda-max` | bounds the deltas are clipped to | |
| `--score-clip CP` | clamp the label before the sigmoid | `3000` |
| `--source-weight NAME=W` | per-record loss weight by source | `human=0.5` |

They compose additively on top of the epoch anneal and are then clipped; a
source override replaces the result; an unknown game result is always
`lambda = 1`, whatever the dials say. **Every dial defaults to no effect**, so
a run that names none is bit-for-bit the run it would have been before they
existed.

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
| `--sources 0 2` | train on self-play + human only |
| `--workers 6` | four to six saturates the loader; more does nothing |
| `--batch-size 32768` | bigger batches keep the 3070 busier — worth ~7% |
| `--hidden 512` | narrower and faster; what a small dataset wants |
| `--output-buckets 1` | one output row for every phase |
| `--no-weight-clip` | measure what the clip costs; the export will then refuse the net |
| `--device cpu` | force CPU, e.g. to reproduce a CI failure |
| `--seed 1` | shuffling and initialisation, so a run repeats |
| `--positions-per-epoch 50M` | see [Datasets too big for one epoch](#datasets-too-big-for-one-epoch) |
| `--resume` | continue an interrupted run from its last epoch |
| `--init-from CHECKPOINT` | new run from weights, fresh optimizer; architecture inherited |
| `--feature-factorization` | share piece-square learning across king squares; no inference cost |
| `--finish-epochs 1` | one extra full data pass after the main epochs; default 0 disables |
| `--finish-lr 0.00001` | constant LR for finishing, independent of the main LR decay |
| `--chunk-records 0` | force memmap slices at any dataset size |
| `--lambda-progress -0.2` | trust the game result more as the game's end approaches |
| `--lambda-source human=1.0` | train a source on its search score alone, without dropping it |
| `--score-clip 3000` | stop mate and tablebase labels asking for infinite confidence |
| `--source-weight human=0.5` | set the mixture without regenerating |
| `--score-scale gen-005.cnn=1.122` | put one shard's labels on another's scale (see "Training a net that ships") |

Expect a few minutes per 100M-position epoch on an RTX 3070, and 5–15 epochs.
The reference configuration — `--hidden 512 --output-buckets 8 --uncertainty
--feature-factorization`, batch 16,384, four workers, on `gen-005.cnn` — runs
at **1.39M positions/s** sustained, measured as the steady-state window rate
over 1,000 batches. It was **447k** before the sparse feature transformer
landed, so a pass that took an hour takes twenty minutes.

Raise `--workers` until it stops helping, which is **four**: the loader
delivers 2.8M positions/s there and does not improve at six or eight, because
`unpack()` is the cost and four workers already saturate the memory bandwidth
it runs on. Past that the number that moves is `--hidden`.

Where a 16,384-position step goes at the reference configuration:

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

The loader runs concurrently at 5.8 ms and is not the constraint. That last row
is what a fusing compiler would take: the step is a chain of separate kernels
each streaming a 33-67 MB tensor in and back out, and nothing in eager PyTorch
merges them.

---

## Feature factorization and a low-LR finish

Both changes are **opt-in**, so they can be tested independently. None of the
lambda, source-weight, or uncertainty-weight defaults change.

### Training-only feature factorization

With `--feature-factorization`, each input embedding is
`king_specific[king, piece, square] + shared[piece, square]`. The additional
768 rows (plus a zero padding row) share piece-square information across all
32 normalized king locations. They start at zero, preserving the unfactorized
initial function and RNG state. Both perspectives use the existing normalization.

Clipping bounds the **sum**, rather than clipping each term independently and
hoping their sum fits. Export folds the two float terms **before rounding**.
The engine still runs the same HalfKA-32sq SCReLU net: identical file layout,
feature count, hidden width and inference cost. Training does extra embedding
work; the size of the strength gain must be measured, not assumed.

A short same-batch GPU diagnostic on the RTX 3070 (512 wide, batch 16,384,
uncertainty head, fused AdamW, 10 warm-up + 30 timed steps) measures about
1.60M positions/s without factorization and 1.35M with it — a 16% cost, where
before the sparse feature transformer it was 34%. The factor is folded into the
feature table once per step now, rather than run as a second embedding over
every piece of every position, so it costs one pass over the table instead of a
second pass over the batch. This excludes loading/validation/checkpointing and
is not a sustained throughput benchmark. The exported engine still pays **no
extra inference cost**.

Old checkpoints still load/export. `--init-from` can add a zero shared factor
to an old checkpoint; a factorized checkpoint automatically retains its factor.
For measuring factorization's learning benefit, prefer a from-scratch control
and factorized run with matching seed, data, update budget and loss settings.

### Finishing stage

`--epochs 10 --finish-epochs 1 --finish-lr 0.00001` means ten main epochs, then
**one additional whole pass** at LR 1e-5. Adam moments are retained, the main
exponential scheduler stops, and lambda is held at `--lambda-end` with the same
per-record adjustments. `--positions-per-epoch` affects only the main stage;
the finishing pass is not silently shortened to that virtual-epoch size.
`--limit-batches` still truncates both stages for smoke tests.

`<prefix>-pre-finish.pt` is always saved when finishing is enabled, allowing
the finishing pass to be tested against its exact starting weights. The final
checkpoint remains `<prefix>.pt`. `--checkpoint-every 1` additionally saves
each epoch, with the finishing epochs continuing the main epoch numbering.

Resume with the original recipe and `--resume`. The saved optimizer, LR stage,
and RNG state are restored; changed schedule/loss/data settings are rejected.
For a run begun with `--init-from`, remove that flag when resuming and explicitly
pass the inherited `--hidden`, `--output-buckets`, `--uncertainty` (if present),
and `--feature-factorization` (if present) recorded in its run manifest.
Loader prefetch state is not saved: **a resumed virtual epoch starts a fresh
shuffled pass**, so resume does not promise batch-for-batch equivalence to an
uninterrupted run. A finishing pass interrupted before its checkpoint is replayed
from the last completed checkpoint, rather than being partially skipped.

For an already trained checkpoint, use a **new output prefix**,
`--init-from CHECKPOINT --epochs 0 --finish-epochs 1 --finish-lr 0.00001`.
This reads the architecture and weights but creates a **fresh optimizer**.
Repeat the desired target/loss flags explicitly: those are not inherited from
the parent. The parent checkpoint path and hash are recorded in provenance.

For your existing command, append these flags and replace `--out` with a fresh
prefix such as `gen5-factor-finish`:

```text
--feature-factorization --finish-epochs 1 --finish-lr 0.00001
```

### Reading the new metrics

Train and validation now both report value MSE, raw uncertainty L1, its weighted
contribution, and total loss. Weighted value metrics use the **global sum of
record weights**, not an average of differently weighted batch means. Training
metrics remain online measurements while weights change; validation is at the
end-of-epoch checkpoint.

Fixed `score_mse` and `wdl_mse` diagnostics use K=400, no source weights, no score
clip, and no lambda schedule. WDL includes only known results. These support
comparisons across recipes on the same validation set; they are not extra losses
and do not replace engine matches. Read them after refitting each net's scale
(`python -m nnue.calibrate refit`): raw, they mostly measure that epoch's
confidence. `lambda_applied` includes the forced lambda=1
for unknown WDL records.

History/checkpoints carry `metrics_version: 2`. Legacy `train`/`train_loss` fields
now mean **value-only**, matching `val`/`val_loss`; older uncertainty runs stored
the combined training objective there. Do not overlay those old/new fields
without accounting for the version. Missing validation/WDL metrics are JSON
`null`, not a misleading zero.

Source filtering never substitutes an excluded record for an empty batch.
All-excluded datasets fail explicitly instead of training on the wrong source
or looping forever. Chunked-loader startup counts are upper bounds when filtering;
epoch metrics report the number of records actually consumed.

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
- **`test_factorization.py`** — shared gradients, zero padding, folded-weight
  clipping, old checkpoints, and folding before quantization without a shape change.
- **`test_training.py`** — filtering with multiple workers, metric denominators,
  fixed diagnostics, low-LR stage boundaries, weights-only initialization and resume,
  and `--score-scale` rescaling one shard and no proven score.
- **`test_calibrate.py`** — the scale fit and the refit recover scales they are
  handed, proven scores stay out of the fit, and both commands run on a shard.
- **`test_provenance.py`** — checkpoint/network hashes, stale export protection,
  dataset inventory, and mixed-checkpoint/optimizer rejection.

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
