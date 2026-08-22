# trainer

The NNUE trainer. Python, its own virtualenv, not part of the C build and not
subject to the C style rules. The plan it implements is
[../docs/NNUE.md](../docs/NNUE.md) — read that first; this file is only how to
run it.

```
nnue/format.py    the 32-byte record, and the features the network sees
nnue/dataset.py   memory-mapped batches
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

The script picks an interpreter that has a PyTorch wheel and installs torch
from a **CUDA index**, not from PyPI — the PyPI `torch` wheel for Windows is
CPU-only, and installing it is a silent 50× slowdown rather than an error. It
prints whether CUDA came out available; if it says otherwise, believe it.

Run everything from this directory so `nnue` is importable:

```powershell
cd trainer
.venv\Scripts\python.exe -m nnue.train --help
```

---

## The pipeline

```powershell
# 1. generate. One process per worker; each writes its own shard.
#    Tree sampling is ON by default (-tree 1): roughly half the records come
#    from inside the search trees rather than from the game line. `-tree 0`
#    turns it off, `-tree 2` doubles it. See `datagen selfplay -help`.
.\datagen.exe selfplay -o external\data\shard%02d.cnn -games 20000 -nodes 10000 -threads 14

# 2. label games that already exist. `tuner extract` turns PGNs into quiet
#    FENs carrying the game result; `datagen label` searches each one.
.\tuner.exe extract external\training\*.pgn -o external\training\human.epd
.\datagen.exe label external\training\human.epd -o external\data\human.cnn `
    -source human -nodes 10000 -threads 14

# 3. prove the data before training on it. -relabel re-searches from scratch,
#    so it needs the SAME -nodes the shard was written with - every record
#    looks wrong otherwise. The shard's .json says which.
.\datagen.exe verify external\data\shard00.cnn -relabel 500 -nodes 10000
.\datagen.exe stats  external\data\shard00.cnn

# 4. shuffle on disk, across every shard of every source.
#    Not optional - see below.
.\datagen.exe shuffle external\data\shard*.cnn external\data\human*.cnn `
    -o external\data\train.cnn -seed 1

# 5. hold a shard back, then fit
cd trainer
.venv\Scripts\python.exe -m nnue.train `
    --train ..\external\data\train.cnn `
    --val   ..\external\data\val.cnn `
    --epochs 10 --batch-size 16384 --out ..\external\nets\net

# 6. quantise the checkpoint into the file the engine embeds
cd ..
make nnue-export        # net.pt -> net.nnue, plus .vectors and .sha256

# 7. build the engine carrying it, and prove the C inference reproduces the
#    reference. nnue-test re-exports and rebuilds first, so it is also the
#    shortest way to do step 6 and this one in a single command.
make nnue               # stormbreaker-nnue.exe, beside the classical binary
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
make nnue        EVALFILE=external/nets/run7.nnue    # stormbreaker-nnue.exe
```

Forward slashes and no spaces: `EVALFILE` is embedded as an assembler string
literal, so a Windows path breaks it — `\net` becomes a newline rather than a
directory.

**The net is linked in, not loaded at run time.** Re-exporting therefore does
nothing at all to a binary that already exists. `make` gets this right —
`EVALFILE` is a prerequisite of the engine — but nothing stops you from
exporting and then benching yesterday's `stormbreaker-nnue.exe`. `make
nnue-info` prints the hash the exporter printed; if those two disagree, the
binary is stale and every number it produced is about a different net.

`make nnue-test` is a gate, not a formality. It requires `src/nnue.c` to equal
the quantised numpy reference **exactly** on all 10,000 vectors and exits
non-zero on a disagreement of one, which is why it works as a CI check. Run it
after every export: it is the only thing that checks the net the engine plays
with is the net that was trained.

`make nnue` builds `stormbreaker-nnue.exe` beside the classical
`stormbreaker.exe` rather than over it, because comparing the two is the whole
point of Task 4 and that is hard to do when the second build overwrites the
first. `make <target> EVAL=nnue` — `make bench EVAL=nnue`, say — does not get
that second name: it builds the network engine as plain `stormbreaker.exe`,
over the classical one. The rebuild itself is correct, since a `.buildflags`
stamp sees the flags change, but the binary left sitting there afterwards is
not the one its file name implies.

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
feature transformer   6144 -> 512, shared weights, one accumulator per side
concatenate           [stm accumulator ; non-stm accumulator] -> 1024
activation            clipped ReLU, [0, 1]
output                1024 -> 1
```

The feature set is `(king bucket, piece, square)` per perspective over both
colours' pieces: 8 × 12 × 64 = 6144. It reuses the classical evaluation's
normalisation verbatim — rank-flip for the perspective's owner, file-mirror
when that king sits on the kingside, then `king_bucket()` from
[../src/eval.c](../src/eval.c). That is the payoff for having built the linear
model as a factorised HalfKA: the feature extraction already existed and was
already the thing the tuner fits.

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
evaluation was fitted at) and `lambda` annealed from `--lambda-start` (0.9) to
`--lambda-end` (0.7) across the run. Records whose game result is unknown —
a labelled EPD with no `[x.x]` on it — use `lambda = 1` for that record rather
than being dropped or being pretended into a draw.

`lambda` is a real hyperparameter and worth two or three runs, not a guess to
be lived with. Record what each one scored in
[../docs/EXPERIMENTS.md](../docs/EXPERIMENTS.md).

---

## Useful flags

| Flag | Why |
|---|---|
| `--limit-batches 20` | smoke-test the whole loop in seconds |
| `--sources 0 1` | train on self-play line + tree samples only |
| `--workers 8` | the loader, not the model, is usually the bottleneck |
| `--batch-size 32768` | bigger batches keep the 3070 busier |
| `--hidden 1024` | wider; measure the 512 one first |
| `--device cpu` | force CPU, e.g. to reproduce a CI failure |

Expect tens of minutes per 100M-position epoch on an RTX 3070, and 5–15 epochs.
If it is much worse than that the bottleneck is the loader — raise `--workers`
and `--batch-size` before touching anything else.

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
