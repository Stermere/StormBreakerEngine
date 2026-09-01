# NNUE

Design notes and implementation status for the neural-network evaluation.

See [TESTING.md](TESTING.md) for the testing process. Lower training loss does
not by itself establish an Elo gain.

---

## Purpose and limits

The evaluation today is 13,684 weights, linear, fitted to game results. It is
worth roughly +270 Elo over the untuned term set. A network is worth more for
one structural reason: the linear model cannot represent an interaction it was
not given a term for, and every term it has was written by hand. "Knight on f5
is good, *unless* the enemy has the light-squared bishop and a pawn on g6" is
not a term anyone wrote, and there are thousands of them.

The label limits what the network can learn. A network trained to reproduce
`eval_evaluate()` cannot beat `eval_evaluate()`; it can only approximate it
more cheaply, which we do not need. The network gets stronger than the current
evaluation only because it is trained on something stronger than the current
evaluation — a **search score**. A 10,000-node search is worth several hundred
Elo over the static evaluation that search calls. Training a static function to
predict that search's output is distillation, and the distilled function
inherits most of the gap. Then the search calls *it*, and the whole thing
bootstraps: net *n+1* is trained on searches that used net *n*.

The training pipeline therefore repeats across generations.

**One turn of that loop has now been run against the human corpus, and it
returned much less than the turn before it.** `gen-003` re-labelled 178.8M
human positions with a much stronger engine than `gen-001` had used. The net it
produced is the one the engine ships, and it is ahead of the `gen-012` hybrid
net it replaced - but by a margin that does not separate from noise, where the
step from `gen-001` to `gen-012` was worth ~50 Elo (E18 in
[EXPERIMENTS.md](EXPERIMENTS.md)).

The distillation argument above is not wrong, but it has a premise it does not
state: the *positions* have to be worth labelling. A better label on a position
the net already predicts well teaches it nothing, and a corpus of human games
is fixed in what it covers however good the labeller gets. Read the loop as
being about position selection at least as much as about label quality.

---

## Task order

Each task is a separate piece of work with its own acceptance gate. Do not
start the next one until the current one's gate is green — this is the same
"one change per test" discipline that applies to search patches, and it matters
more here, because a training pipeline has four places to be silently wrong and
no symptom for any of them except "the net is a bit weak".

| # | Task | Gate | State |
|---|---|---|---|
| 1 | **Data generation** — `tools/datagen.c`, the record format, the shuffler | Regenerated positions round-trip to identical FENs; label reproducible across runs | **built** — `make datagen-test` |
| 2 | **Trainer** — `trainer/`, PyTorch, its own venv | Loss curve on held-out shard; sanity scores on known positions | **built** — `make trainer-test` |
| 3 | **Export + inference** — `tools/export_net.py`, `src/nnue.c` | C inference matches the quantised Python reference **exactly** on 10k positions | **built** — `make nnue-test` |
| 4 | **Integration** — NNUE replaces `eval_evaluate`, margins re-tuned | SPRT vs the current build | TODO |
| 5 | **Search integration** — correction history, then an uncertainty head for the margins | SPRT each | corrhist **shipped** (E14); head TODO |

Tasks 1–3 produce no Elo and cannot be SPRT'd. That is normal and it is why
they need their own hard gates: they are the tasks where a bug survives.

---

## Task 1 — data generation

### What gets labelled, and how

One record is one position plus one score plus one game result. The score is a
**fixed-node search from that position**, not a static evaluation and not a
score lifted out of a tree.

`tools/datagen.c` links the engine the way `tools/tuner.c` does — every source
except `main.c`, plus its own driver — so it plays and searches with exactly
the engine in the working tree.

```
datagen selfplay -o external/data/shard%02d.cnn -games N -nodes 10000 -threads 16
datagen label    external/training/human.epd -o external/data/human.cnn -nodes 10000 -resume
```

Two subcommands because there are two jobs: generate positions by playing, and
label positions that already exist.

### Position sources

| Source | Where from | Why it is in the mix |
|---|---|---|
| Self-play game line | `datagen selfplay` | The distribution the engine actually plays into |
| **Search-tree samples** | interior nodes of the same searches | The distribution the evaluation is actually *called on* — see below |
| Human FENs | `external/training/human.epd`, 22.6M lines | Real positions, structurally unlike engine self-play, already on disk |
| Strong-engine FENs | `external/training/ccrl_*.epd`, 7.2M + 45M lines | Positions from play far above our level, which self-play cannot reach |

The last two are already extracted and sitting in `external/training/`. They
cost nothing but the labelling pass, and they are the cheapest diversity
available.

**On the strong-engine positions and licensing.** We take *positions* from
those PGNs and label them with *our* search. We never take another engine's
evaluation. That distinction is the entire licensing story for the resulting
net, and it is worth being pedantic about: a net trained on Stockfish's output
is a derivative of Stockfish. A net trained on positions Stockfish happened to
play, scored by us, is not.

### Search-tree sampling, and why every sample gets a fresh search

The idea is to sample positions from inside the search tree rather than only
from the game line. This is right, and for a better reason than diversity: **it
is the correct training distribution.** The evaluation is called at the leaves
of an alpha-beta tree, where one side has frequently just hung a piece, a
capture sequence is half-finished, or a null move has just been passed. Those
positions do not look like positions from a game line between two engines
playing well. Training only on game-line positions trains the evaluation on a
distribution it is never used on.

The tempting version of this is to also take the *scores* the search already
computed at those nodes, for free. **Do not.** Almost every interior node in an
alpha-beta tree returns a bound, not a score: a fail-low is an upper bound, a
fail-high is a lower bound, and only PV nodes are exact. Worse, the bound is
relative to a window inherited from the parent, so the error is not noise
around the truth — it is a systematic, one-sided offset whose sign depends on
where the node sat in its parent's move ordering. [TUNING.md](TUNING.md) argues
that symmetric label noise is nearly free because a uniform attenuation is
absorbed by `K`. That argument does not extend to asymmetric noise, and this
noise is asymmetric by construction. Mixing exact labels with bounds also mixes
labels of wildly different remaining depth, so the net learns the average of a
depth-2 opinion and a depth-12 opinion.

So: **the tree is a position sampler, and every sampled position is re-searched
from scratch at the same fixed node count as everything else.** Every record in
the dataset then has an identical label definition, which is worth more than
the searches the re-labelling costs.

That does mean tree sampling is not free — a position costs a search, wherever
it came from. The arithmetic says we can afford it comfortably:

```
bench on this machine        991,512 nps, single-threaded
16 threads, own TT each      ~12M nps aggregate (memory-bound, not 16x)
10,000 nodes per label       ~1,200 labels/second
                             ~4.3M/hour, ~100M/day
```

100M positions is a comfortable first dataset, and it arrives in a day. There
is no throughput pressure here at all, which is exactly why the budget should
go on label quality rather than on squeezing free labels out of a tree.

The sampler itself:

- **Reservoir-sample** a small fixed number (1–2) of interior nodes per played
  move, over the whole tree. Not "every node", not "the first N": an alpha-beta
  tree is overwhelmingly leaves, so any sampler that does not stratify returns
  almost nothing but quiescence positions.
- **Require a minimum remaining depth** (start at 4) so samples come from
  interior nodes rather than the leaf shell.
- **Never sample inside quiescence**, and never sample a position in check.
- **Deduplicate by Zobrist key** across the whole shard. Siblings in a subtree
  differ by one piece and are near-duplicates; without dedup a large fraction
  of the dataset is the same position wearing a hat.
- Re-search sampled positions **after the game**, with the TT cleared, so a
  label does not depend on what the game search happened to leave in the table.
  Reproducibility here is the same property `search_clear()` protects.

The tree also offers move-ordering labels for free — which move caused each
cutoff. The policy head that would have consumed them was Task 5b and has been
**cancelled**; the `.pol` sidecar machinery in datagen stays (see the record
format below), but new generation runs may pass `-nopolicy` rather than paying
4 bytes per record for a consumer that no longer exists.

### The mixture is an experiment, not a decision

How much of the dataset should be self-play line, tree samples, human FENs and
CCRL FENs is an empirical question with a surprising answer more often than
not. Generate the four sources into separate shards, train nets on different
mixtures at a *fixed total position count*, and play them against each other.
Record the result in [EXPERIMENTS.md](EXPERIMENTS.md) like anything else.

Starting mixture, to be beaten: 50% self-play line, 25% tree samples, 15%
human, 10% CCRL.

### Self-play settings

| Setting | Value | Why |
|---|---|---|
| Opening | 8 random plies, or the existing book | Without randomisation every game is the same game |
| Nodes per move | 10,000 | Fixed nodes, never fixed time — time makes labels machine-dependent |
| Adjudication | win at ±2000 held 4 plies; draw at 0 held 8 plies after move 60 | Endgame technique is not what we are training |
| Filter | not in check, best move not a capture, abs(score) < 2000 | Same quiet-position argument as the tuner's extractor |
| Hash | 8 MB per worker | Small, private, cleared between games |

`board_is_draw`, the existing repetition and fifty-move handling, and the
adjudication all have to agree, or self-play produces games that end in states
the engine thinks are still going.

### Record format

A 32-byte packed record. Text is not an option at this scale — 100M positions
as EPD is ~7 GB of parsing; packed it is 3.2 GB that memory-maps.

```
offset size  field
0      8     occupied bitboard
8      16    piece nibbles, one per occupied square, LSB-first square order
24     1     bit 7: side to move;  bits 0-6: en passant square (127 = none)
25     1     halfmove clock
26     2     fullmove number
28     2     score, centipawns, side-to-move relative (int16)
30     1     WDL from the side to move: 0 loss, 1 draw, 2 win
31     1     flags: bits 0-2 source tag, bit 3 in-check, bits 4-7 reserved
```

Castling rights ride in the nibbles: piece codes are `type | color << 3` with
types 0–5, and a rook that still has castling rights is type 6. Three codes
spare, no extra bytes, and it encodes Chess960 castling correctly for free —
which matters because Chess960 is on the roadmap and a data format is expensive
to change once 100M records exist.

**No file header.** Shards must concatenate with `cat`, which a header breaks.
Metadata — engine commit, node count, filter settings, record count — goes in a
sidecar `.json` next to each shard. A shard whose manifest is missing is a
shard that cannot be trusted, and that is the correct default.

**Policy sidecar**: `shardNN.pol`, 4 bytes per record in the same order — best
move and cutoff move as two `uint16` in the engine's existing move encoding.
Its intended consumer, the cancelled policy head, is gone. The machinery stays
because existing shards carry sidecars and every tool that permutes records
must keep the two files aligned; `-nopolicy` turns it off for new runs.

### Shuffling

Positions from one game are correlated, and a training batch drawn from one
region of the file is a batch of near-duplicates. Shuffle **on disk**, once,
after generation: read shards in a random order into K bucket files by random
assignment, then shuffle each bucket in memory and concatenate. Two passes over
3 GB, and then the trainer can read sequentially forever.

Do not rely on a DataLoader shuffle buffer. A 100k-position buffer over a file
where consecutive records come from the same game is not a shuffle.

### Gate

- Every record round-trips: unpack → FEN → `board_set_fen` → repack → identical
  bytes, over the whole dataset.
- Labelling the same position twice, in different runs, gives the same score.
- The distribution of scores, phases and piece counts is dumped and eyeballed.
  A dataset that is 60% endgames or has a score histogram spiked at zero has a
  filter bug, and this is the last point where that is cheap to find.

### Running it

```sh
make datagen           # builds ./datagen
make datagen-test      # the gate above, on a few games. Seconds.
```

```
datagen selfplay -o external/data/shard%02d.cnn -games N -nodes 10000 -threads 16 -book external/books/<book>.epd -opening 2-3 -tree 0
datagen label <in.epd>... -o out.cnn -source human -nodes 10000 -resume
datagen shuffle <in.cnn>... -o train.cnn -seed 1
datagen verify <shard.cnn>... -relabel 500     # the gate; exits non-zero on failure
datagen stats <shard.cnn>...                   # the histograms to eyeball
datagen dump <shard.cnn> -n 20 [-pol]          # records as text
```

**Labelling runs are long, so `label` takes `-resume`.** It checkpoints every
30 seconds - a flush, plus a 64-byte `.resume` file beside the shard - and on a
rerun truncates back to that checkpoint and carries on from the input line it
recorded. Without it a rerun opens the shard `"wb"` and starts from the first
line, which on 22.6M positions is five hours.

The checkpoint is a lower bound, never an upper one: a shard may hold more
records than the checkpoint claims and never fewer, so resuming discards the
uncheckpointed tail rather than trusting it. The dedup key set is rebuilt by
replaying the records already written, so a resumed shard is byte-identical to
one that nothing interrupted - without that, the second half of a run cannot
see the first half's positions and the shard quietly holds duplicates. A
finished shard is left alone, so the command is safe to repeat until it says
complete; keep `-threads` the same, since workers split the input by line.

**`datagen <subcommand> -help` is the option reference.** Every flag, with its
default, in the same order the parser reads them; the list below only covers
the parts that are surprising.

### Tree sampling is off by default

`selfplay` does not sample the search tree unless asked, which is worth stating
plainly because the flag reads like a knob on something already running:

| Flag | Default | What it does |
|---|---|---|
| `-tree N` | 0 | interior nodes reservoir-sampled per played move; `-tree 0` is a game-line-only shard |
| `-treedepth N` | 4 | remaining depth a node must still have to be eligible. Lower reaches into the leaf shell, which is overwhelmingly quiescence positions — the thing the stratification exists to avoid |

A tree sample is a full record: it is re-searched, deduplicated and written
exactly like a game-line position, and it is tagged `tree` so a trainer can
re-weight the mixture without regenerating anything. `datagen stats` prints the
split, and it is the fastest way to see what a `-tree` setting actually did:

```
-tree 0    selfplay 100.0%
-tree 1    selfplay  42.5%   tree 57.5%
-tree 2    selfplay  17.8%   tree 82.2%
```

Those shares are not the sample count divided by the move count: the quiet
filter discards a fraction of the game-line positions and none of the tree
ones, so the tree share always runs higher than `-tree` alone suggests. Set the
mixture by generating the sources into separate shards and combining them.

### Openings: a book, and a deliberately tiny perturbation

Everything after the opening is deterministic. The search is fixed-node on one
thread and nothing randomises the move choice, so **a game is a function of its
start position and its random plies, and of nothing else.** That makes the
opening the entire coverage story.

| Flag | Default | What it does |
|---|---|---|
| `-book <file.epd>` | none | draw each game's start uniformly from this EPD — a FEN per line, anything after it ignored, which is exactly what `tuner extract` writes |
| `-opening N` or `MIN-MAX` | 2 with a book, 8 without | random legal plies played out of the start position; a range is drawn per game |
| `-openingscore N` | 800 | throw the game away if the opening is already decided by more than this |

Only the line offsets of a book are held in memory, so a 50 MB book costs ~6 MB
per worker rather than being loaded once per core.

The two settings are one decision. Without a book, 8 random plies are how a
game gets anywhere other than the same game every time — and they produce
positions no one would play into. With a book, they are a **perturbation**, and
they want to be small for a specific reason: two games that share a book line
and differ by a couple of plies play out deterministically from near-identical
positions, so the difference in their *results* is attributable to the
perturbation rather than to noise. That is the only thing that makes the
trainer's game-result term (`--lambda-end` below 1) worth anything. Long random
openings destroy it, because then no two games share a position to contrast.

**The count's parity is a second decision, and it wants a range.** A book built
the way the one below is — `-minply 20 -maxply 20`, a single ply — has the same
side to move on every line of it: white on all 793,495 CCRL entries and all
1,445,782 Lichess ones. A *fixed* even count therefore hands the engine white to
move at the start of every game in the generation, and a fixed odd one hands it
black. `-opening 2-3` draws the count per game and splits it: measured over 400
games out of the ply-20 book, `-opening 2` starts 95.8% of them with white to
move and `-opening 2-3` starts 50.5%.

It has to be fixed here, at generation time. The sampler keeps the positions the
games walked through, so a parity the games never had is not something a filter,
a shuffle or a re-weighting downstream can put back — and the aggregate
side-to-move share in the shard hides it, because a game alternates colours
whichever side starts. The manifest records both bounds (`opening_plies`,
`opening_plies_max`).

The book decides which positions a generation ever sees, so it is pinned by
hash in `job.env` (`BOOK_SHA`) exactly as the net is, and the shard manifest
records its path and how many entries it indexed.

**Building one.** `tuner extract` writes the format directly, and `-minply N
-maxply N` bounds the sampling window to a single ply, which is one position
per game:

```sh
tuner extract external/training/ccrl/split_4040/*.pgn -o raw.epd \
    -minply 20 -maxply 20 -stride 1 -maxscore 300
sed 's/ \[[0-9.]*\]$//' raw.epd | LC_ALL=C sort -u > external/books/ccrl4040_ply20.epd
```

The dedup is not optional. CCRL games are themselves played from a shared
opening book, so the raw extract repeats itself heavily — 15% unique at ply 12,
25% at ply 16, 42% at ply 20. 2.42M games give **793,495 unique** ply-20
positions, and a book with 6× duplicates is a sampler that visits a sixth as
much of the opening tree as its line count suggests.

Deeper is better on both counts: more unique, and more of each game already
decided by real play rather than by the engine's own preferences. Against
`-opening 8` from the start position, the ply-20 book measures *more* decisive
— 53.3% draws against 59.5% — and its games are 15 moves shorter.

Five things came out differently from the sketch above, each for a reason.

**`-threads N` runs N processes, not N threads.** `search.c`'s state is
file-scope and the transposition table is one global allocation, so two
searches in one process would share both — and a shared table is exactly the
contamination the label definition exists to prevent. POSIX forks; Windows
re-launches its own command line with `--worker N`. Each worker writes its own
shard, and shards concatenate, so nothing downstream notices.

**Every record is re-searched, not only the tree samples.** The argument for
re-searching a sampled position is that its label must not depend on what the
game search left in the table — and that applies just as much to a position on
the game line, whose search inherited a table warmed by every move before it.
So the game is played with a warm table (that is play, and it should be
strong), candidates are collected, and once the result is known **every one of
them is re-parsed from its own FEN and re-searched from a cleared engine.**
Re-parsing matters as much as clearing: a `Position` carried out of a game
brings its repetition history with it, and `board_is_draw` would then score it
differently from the same FEN standing alone. That is what `verify -relabel`
checks, and it is the only reason a self-play record and a CCRL record can sit
in the same batch.

**The tree sampler is a compile-time hook.** `-DDATAGEN` switches on a node
visitor at `negamax`'s normal exit, where the position is restored and the
move that cut off is known. The shipped engine carries neither the hook nor the
branch that tests it, and `make bench` gives the same node count either way.

**The quiet filter applies to the game line only.** Filtering tree samples for
quietness would defeat the entire purpose of taking them — they are wanted
*because* they are mid-tactic. Promotions are excluded alongside captures,
which the sketch above did not say: a promotion moves as much material as a
capture does.

**The start position became an input.** The sketch listed four position sources
and started every game from the initial position with random plies for
variation, which quietly made the engine's own opening taste the only opening
the net ever sees — and made the game result nearly uninformative, since two
games share no position to attribute a differing result to. `-book` replaced
that; see "Openings" above.

### What the first dataset looked like

48 games, 10,000 nodes, defaults otherwise — 9,900 records:

| | |
|---|---|
| score histogram | a clean bell around zero, no spike at 0 |
| result, side to move | 25.5% loss / 47.0% draw / 27.4% win |
| side to move | 50.6% white |
| source mix | 42.5% game line, 57.5% tree samples |
| **pieces on the board** | **22.1% of records have seven or fewer** |

The last row is the one to watch. The adjudication exists because endgame
technique is not what we are training, but at `-winscore 2000` a fixed-node
search does not reach ±2000 in K+R vs K until mate is nearly in sight, so those
games grind on and every ply of the grind contributes two records. The
documented defaults are kept because they are the documented defaults; if a
trained net turns out weak in the middlegame, `-winscore 1200` is the first
knob to reach for, and one `stats` run confirms what it did.

---

## Task 2 — the trainer

`trainer/`, its own directory, its own virtualenv, not part of the C build and
not subject to the C style rules. `requirements.txt` pinned.

> The machine has Python 3.14 and no torch installed. PyTorch wheels lag new
> CPython releases; if 3.14 has no wheel, install 3.12 or 3.13 for the venv
> rather than building torch from source. The engine does not care what Python
> the trainer runs on. GPU here is an RTX 3070, 8 GB — comfortable for the
> architecture below.

### Architecture

```
feature transformer   24576 -> H, shared weights, one accumulator per perspective
concatenate           [stm accumulator ; non-stm accumulator]  -> 2H
activation            SCReLU, clamp(x, 0, 1)^2 float / clamp(x, 0, QA)^2 int
output                2H -> B, the row selected by piece count
```

**One architecture.** The feature set is `(32 mirrored king squares, piece,
square)` over both colours' pieces — 32 × 12 × 64 = 24576 — and the activation
is SCReLU. Neither is a flag. The inference path in `src/nnue.c` is written for
them: no branch per unit, no branch per piece, and the accumulator in int16
because that is what puts sixteen lanes in an AVX2 register.

Only the *shape* is a choice, and shape is a header field the engine reads out
of the net file:

| Flag | Values | Default |
|---|---|---|
| `--hidden` | any multiple of 16, up to `NNUE_MAX_HIDDEN` (2048) | 1024 |
| `--output-buckets` | any divisor of 32 | 8 |

The normalisation is the classical evaluation's, verbatim: rank-flip for the
perspective's owner, file-mirror when that king sits on the kingside. A
mirrored king then stands on one of exactly 32 squares, and the net indexes
that square. This is the payoff for having built the linear model as a
factorised HalfKA — the feature extraction already exists, is already tested,
and is already the thing the tuner fits.

What each part is for, and what it costs:

| Choice | What it buys | What it costs |
|---|---|---|
| SCReLU | A non-piecewise-linear unit: one unit says "more of this matters more", where clipped ReLU needs two and a bias | Range. The squared activation carries QA², so the output sum is rescaled by QA and the weights are clipped to keep the int16 multiply in bounds |
| Output buckets | Stops one row of weights answering for both a 32-piece opening and a 5-piece endgame | Data per bucket — eight buckets means each row sees an eighth of the dataset |
| 1024 wide | Capacity | Accumulator work, which is the evaluation |
| 32 king squares | "Good with the king on g1, bad on h1", which a bucket holding both cannot say | 24576 feature rows to fit, and a 50 MB net |

The last row is what a large dataset is for. With a few million positions most
of those rows are seen a handful of times; `--hidden 512` is the first thing to
trade down, not the feature set, which is not tradeable.

**Nothing supports the v1 architecture.** `NNUE_FORMAT_VERSION` is 2, the tags
were renumbered, and a v1 net or a v1 checkpoint is refused by name rather than
read as something it is not. There are no nets outside this repository, so
there was nothing to keep compatible with.

### Making the inference fast

The evaluation is two sums of at most 32 rows of `hidden` int16 weights — at
1024 wide, 65,536 int16 additions and ~128 KB of weights streamed per call.
Everything in the hot path serves that:

- **int16 accumulator.** Half the bytes moved, and sixteen lanes per AVX2
  register instead of eight. Nothing wraps, and that is proved rather than
  hoped: `tools/export_net.py` refuses to write a net whose bias plus 32 rows
  could leave int16, with a bound that holds over every legal position.
- **AVX2** where the compiler reports it, plain C otherwise, chosen on
  `__AVX2__` so `ARCH=popcnt` still builds and still runs. The two must produce
  **identical integers**, which is why the SCReLU lanes are flushed into int64
  every 64 vectors: integer addition is associative, so the only way two
  summation orders can disagree is an intermediate that overflows in one.
  `make nnue-test` checks whichever path was built.
- **Rows resolved before they are summed**, so the address arithmetic does not
  block the adds and the next row can be prefetched. Worth about 4%.

Measured on the 42-position bench, one net, node count identical across all
three (257482 — a pure speedup, per [TESTING.md](TESTING.md)):

| Build | nps |
|---|---|
| scalar, int32 accumulator | 292k |
| AVX2, int16 accumulator | ~487k |
| AVX2 + rows resolved + prefetch | ~547k (median of 6) |
| classical evaluation, for scale | ~1.2M |

**The remaining gap is not arithmetic, it is the from-scratch accumulator.**
Every node pays for two full 24576 → 1024 sums, and the way not to pay is to
not recompute: an incremental accumulator updates the handful of features a
move actually changed. That is Task 4's first job, it needs make/unmake hooks
in the search, and it is worth several times what everything above was.

### The label

```
target = lambda * sigmoid(score / K) + (1 - lambda) * wdl
```

`K` is the evaluation's existing sigmoid constant so the units stay
interpretable. Start `lambda = 0.9` — mostly distilling the search, with enough
game result to stop the net inheriting the teacher's systematic errors. Anneal
toward 0.7 in later epochs. The loss is MSE on that blend, or cross-entropy;
both work, MSE is what most trainers use.

`lambda` is a real hyperparameter and worth two or three runs, not a guess to
be lived with.

### Making PyTorch fast enough

Plain PyTorch is 5–20× slower than a purpose-built NNUE trainer, and almost all
of the difference is the input pipeline rather than the GPU. Two things get most
of it back:

- **`nn.EmbeddingBag(mode='sum')` for the feature transformer.** A position has
  at most 32 active features per perspective out of 6144. A dense 6144-wide
  input matmul is ~200× wasted work; `EmbeddingBag` is exactly the sparse-sum
  primitive the accumulator is.
- **Memory-map the shard, build index tensors in DataLoader workers.** The
  records are fixed-size, so `np.memmap` plus arithmetic gives batch tensors
  with no per-record Python.

Expect tens of minutes per 100M-position epoch on the 3070, and 5–15 epochs.
If it is much worse than that, the bottleneck is the loader, not the model.

### Gate

Held-out shard loss, plus a sanity check that has caught more bugs than the
loss curve ever will: **run the net on the starting position and on a set of
positions with known evaluations, and look at the numbers.** A net that scores
the start position at 300cp has a perspective bug, and its loss curve will look
completely normal.

### Running it

```powershell
make trainer-setup               # trainer\.venv, torch from a CUDA index
make trainer-test                # the format, feature and pipeline tests

cd trainer
.venv\Scripts\python.exe -m nnue.train --train ..\external\data\train.cnn `
    --val ..\external\data\val.cnn --epochs 10 --out ..\external\nets\net
.venv\Scripts\python.exe -m nnue.sanity ..\external\nets\net.pt
```

`train.py` prints the sanity table after every epoch rather than only at the
end, because a perspective bug is worth finding in epoch 1 and the loss curve
will not find it in epoch 10 either. The table adds two columns the gate above
does not ask for, and they check different things:

- **`flip`** — the position with both colours swapped, the ranks flipped and
  the side to move swapped. That is the same position seen from the other side,
  and since the score is side-to-move relative it must come back **identical**.
  This is a structural identity of the architecture: it holds on random weights,
  to floating-point precision, and a non-zero difference means the feature
  normalisation is not symmetric. It is worth being precise about, because the
  intuitive guess — that the flip should *negate* the score — is wrong, and
  a check written that way passes only when the net has been driven to zero.
- **`null`** — the same board with the other side to move, which should score
  roughly the negative. This is the check that catches what `flip` cannot:
  reading the *non*-side-to-move accumulator first leaves the net perfectly
  colour-symmetric and evaluates every position from the opponent's point of
  view, which is precisely the "plays reasonably but hates its own position"
  failure in the table at the end of this document.

`flip` is exact and is asserted in the tests. `null` is approximate — tempo is
real — so it is reported, and only over positions decisive enough for the sign
to mean anything.

**The units are pinned to Task 3's quantisation, deliberately.** The float
model's output is in units where `1.0 = 400 centipawns`, because
`eval_cp = raw * SCALE / (QA * QB)` with `SCALE = 400`. Fixing that now makes
the exporter a pure re-scaling rather than a re-interpretation. `K` defaults to
400 for the same reason — it is the scale the classical evaluation was fitted
at, so a target computed here means what a target computed by `tools/tuner.c`
means.

**A record whose game result is unknown uses `lambda = 1`** rather than being
dropped or pretended into a draw. `datagen label` writes WDL 3 for an EPD line
with no `[x.x]` on it, and its search score is worth exactly as much as any
other record's.

The three test files are the parts worth knowing about:

- `tests/test_format.py` decodes the same records twice, once with
  `datagen dump` and once with `nnue/format.py`, and requires identical FENs,
  scores, WDLs, source tags and check bits. **This is the failure with no
  symptom.** If the two ever disagree, the net trains on positions that are not
  the positions the engine labelled and the loss curve looks perfect.
- `tests/test_features.py` checks the two symmetries the normalisation must
  have: a position and its file-mirror give identical features, and white's
  features of a position equal black's features of it colour-flipped. Those
  catch a mirror applied to the king but not the pieces, and a perspective that
  reads its own pieces as the enemy's.
- `tests/test_model.py` checks the loop is connected — gradients flow, the
  optimiser reduces the loss, the padding embedding stays pinned at zero, and a
  net fitted on a symmetric target scores flipped positions as negatives.

---

## Task 3 — export and the equivalence test

This is the task where a bug is invisible, silent, and costs a month. Treat it
as a correctness task in the same class as movegen.

### Quantisation

```
feature transformer weights, biases   int16, scale QA = 255
output weights                        int16, scale QB = 64
output bias                           int32, scale QA * QB
eval_cp = (activations . w_out + b_out) * SCALE / (QA * QB)     SCALE = 400
```

**SCReLU rescales.** `clamp(x, 0, QA)^2` carries QA² where the bias carries QA,
so the weighted sum is divided by QA — truncating toward zero, C's `/` —
**before** the bias is added, and only then does the line above apply. Getting
that divide wrong is a net that is 255× too confident; getting its *position*
wrong is a net that is off by one everywhere, which is precisely the failure
`make nnue-test` exists to catch and a tolerance would have hidden. The engine
forms `v * w` as int16 before widening, so the exporter refuses any net whose
`QA * max|w_out|` would not fit — and the trainer clips the weights so it does.

`SCALE` puts the output in the same centipawn units the search's margins assume.
It is a free parameter worth one experiment on its own, because it sets how the
net's opinions interact with every threshold in `search.c`.

### The net file, and the repository's no-binaries rule

The net is 50 MB at 24576 × 1024 × int16, which the repository policy says must
not be committed. It also must be present at build time for the bench to be
deterministic and for OpenBench to build a self-contained binary.

Nets live in `external/nets/`, gitignored, and the Makefile embeds one with
`.incbin` from a path in `EVALFILE`. `make bench` prints the net's SHA-256 in
its header — never in the last line, which invariant 2 owns. A node count that
cannot be attributed to a specific net is not a measurement.

Both halves of the flexibility question got built, as planned: embedded by
default, and `setoption name EvalFile value <path>` overrides it at runtime.
The exporter writes a `<net>.sha256` sidecar.

That leaves the question of how a machine that cannot run the trainer gets a
net at all — a CI runner, an OpenBench worker, a fresh clone. A net is
published as a GitHub release of its own, tagged by its own hash (`net-` plus
the first twelve hex digits), and the Makefile pins one:

```make
NET_TAG    ?= net-1ee5325add50
NET_SHA256 ?= 1ee5325add50950b3b8fb34c742988436664615895f02504dc5e2be9ea15c418
```

`make net-fetch` downloads that net and verifies the hash before putting it in
place; `make net-publish` uploads one and prints the replacement pin. Neither
is part of the engine build — `EVAL=nnue` still just embeds whatever is at
`EVALFILE`. See [RELEASING.md](RELEASING.md).

A content-addressed tag cannot come to mean a different file, which is what
makes pinning it safe. The pin says **which** net a build embeds; the hash of a
net that gets *adopted* is still recorded in [EXPERIMENTS.md](EXPERIMENTS.md)
beside the SPRT that adopted it, which says **why** it is the one in use. Those
are two different questions, and only the first one a build system can act on.
Change them in the same commit.

### The test

The quantised network is integer arithmetic. Integer arithmetic is exactly
reproducible. Therefore the acceptance criterion is **exact equality**, not
"close":

1. The exporter computes, in numpy int32, the quantised forward pass for 10,000
   positions drawn from the dataset, and writes `(FEN, expected_int)` pairs.
2. `make nnue-test` runs `src/nnue.c` on the same FENs and compares. Any
   mismatch fails the build.

A tolerance here is a bug generator. If the two disagree by one, something is
rounding differently, and that something will be worth 20 Elo when it appears in
a position that matters.

### Running it

```sh
make nnue-export    # quantise NET into EVALFILE, + vectors + .sha256 + manifest
make nnue           # the engine with the network, as stormbreaker-nnue
make nnue-test      # export, build, and require exact equality on every vector
make nnue-info      # which net a build is carrying, by hash
make net-fetch      # download the pinned net - no trainer, no torch, no venv
make net-publish    # upload this net, and print the pin that names it
```

```
make EVAL=nnue                                  # any target, with the network
make EVAL=nnue EVALFILE=external/nets/cand.nnue # a specific net
make nnue-export NET=external/nets/run7.pt      # a specific checkpoint
python tools/export_net.py --help               # QA, QB, SCALE, vector count
```

`EVAL=classical` is the default and produces the engine that existed before the
network did — same bench node count, no net, none of `src/nnue.c` compiled. The
default stays there until Task 4's SPRT says otherwise: the switch exists so
the two can be compared, and a default flipped ahead of the measurement is how
an untested change ships.

### Upgrading the net, which is the point of most of the above

The architecture is data, not code, wherever it can be: the net file carries
its own feature count, hidden width, quantisation scales, activation tag and
feature-set tag, and the loader believes the file rather than a constant
compiled beside it. What that buys:

| Change | What it costs |
|---|---|
| Retrain the same architecture on better data | re-export, rebuild |
| Retrain **wider** (1024 → 1536 → …) | re-export, rebuild — no C change, up to `NNUE_MAX_HIDDEN`, width a multiple of 16 |
| Retrain with more or fewer **output buckets** | re-export, rebuild — no C change |
| Change QA / QB / SCALE | `--qa/--qb/--scale` on the exporter, rebuild |
| A **new** activation | a branch in `nnue_output()`, a case in the exporter's `forward()`, a new `NnueActivation` value |
| A **new** feature set | a slot count in `nnue_king_slots()` and a case in `nnue_perspective()`, plus a new `NnueFeatureSet` value. An indexing that is not "king slot × 12 planes × 64 squares" also needs `nnue_feature_index()` |
| Change the file layout, or what a tag means | bump `NNUE_FORMAT_VERSION`; every stale net then fails loudly instead of being misread |

The places that need a new `case` are marked `UPGRADE POINT` in `src/nnue.c`.
Every rejection names the field and both values, because a net that fails to
load is almost always someone mid-upgrade: *"hidden width 4096, this build
holds at most 2048"* is a fix, *"bad net file"* is a morning. What that looks
like, on nets with one header field each corrupted:

```
format version 1, this build reads 2 - re-export with the current tools/export_net.py
feature set 7 is not implemented (this build runs 1, halfka-32sq) - add its king slot
                                 count to nnue_king_slots() and its case to
                                 nnue_perspective() in src/nnue.c
activation 5 is not implemented (this build runs 1, screlu) - add its case to
                                nnue_output() in src/nnue.c
3 output buckets - must be a divisor of 32, at most 32
hidden width 9999, this build holds at most 2048 - raise NNUE_MAX_HIDDEN in src/nnue.h
hidden width 100 is not a multiple of 16, which the vectorised accumulator requires
```

and from the trainer, on a checkpoint rather than a net:

```
this checkpoint carries no 'arch' field, so it predates the current network and
describes a model this build cannot run. Retrain: ...
```

### What came out differently from the sketch

**The accumulator sums in int32, not int16.** The stored weights are int16 and
the incremental accumulator Task 4 needs will be too — that is what SIMD wants.
But the from-scratch version sums in int32 so that it cannot silently wrap
where numpy would not, which would make the equality test pass on the ten
thousand positions it covers and fail in a game. Instead the *exporter* refuses
to write a net whose accumulator could leave int16 range, using a bound that
holds over every legal position rather than over a sample: bias plus the 32
largest positive weights in a column, since a position has at most 32 pieces.
This net's worst case is 1898 against int16's 32767, so the narrower future
version is already known to be safe.

**The division truncates, and that had to be said out loud.** `eval_cp =
raw * SCALE / (QA * QB)` in C truncates toward zero; numpy's `//` floors. Every
negative evaluation would have been one centipawn low — about half the vectors,
which is exactly the sort of failure a tolerance would have hidden and a
constant offset would have made look like a scale problem.

**`eval_evaluate` is chosen at link time, not by an `#ifdef` inside it.**
`eval.c` always defines `eval_classical()`; whichever of `eval.c` and `nnue.c`
is compiled for this build defines `eval_evaluate()` as a one-line forward to
its own. `search.c` is untouched, the classical model stays reachable by name
in every build, and `tools/tuner.c` calls `eval_classical()` explicitly — the
Makefile also filters the NNUE defines out of the tuner build, so `make tuner
EVAL=nnue` still fits the model it is supposed to be fitting rather than
quietly optimising against the network.

**Switching `EVAL` (or `ARCH`) now actually rebuilds.** Both produce the same
file name from the same sources, so make compared timestamps, found nothing
newer, and handed back the binary built with the *other* flags — which means an
ablation sweep re-benches the previous build and reports that the flag made no
difference. A `.buildflags` stamp holding the current flags is now a
prerequisite of every binary, written with `$(file ...)` rather than a shell so
the top-level build still needs no POSIX tools.

**The engine hashes its own net.** ~90 lines of SHA-256, no dependency, which
keeps the "links against nothing but libc" property intact. `make bench` prints
the first 12 hex digits in its header.

### What the first export looked like

`net.pt`, epoch 10, 6144 → 512×2 → 1, crelu — the v1 architecture, kept here
because the numbers are the ones the equivalence gate was first proved against.
Neither that net nor that checkpoint loads any more:

| | |
|---|---|
| net file | 6,294,628 bytes (96-byte header + int16 payload) |
| weight peaks | ft_w 99, ft_b 25, out_w 38, against int16's 32767 |
| accumulator bound | 1898 — an int16 accumulator is safe |
| output bound | 2,874,480 — an int32 output sum is safe |
| **C vs reference** | **10000/10000 exact, on both the raw int32 and the centipawns** |
| vs the float model | mean 6.2 cp, max 37 cp |
| flip symmetry in C | 0 cp on every sanity position |
| bench | 206859 nodes, 474k nps (classical: 287826 nodes, 1.24M nps) |

Two rows are worth reacting to. The **6.2 cp mean quantisation drift** is larger
than it needs to be: this net's output weights peak at 38 on a QB = 64 grid, so
the largest weight has 38 levels and the typical one has far fewer. `--qb 256`
costs nothing in headroom (out_w would peak at 152, the output bound at 11.5M)
and should cut the drift by roughly four. That is a cheap experiment and it
belongs in the Task 4 batch, not here — SCALE and QB both change how the net's
opinions interact with every threshold in `search.c`, so they are measured, not
assumed.

The **474k nps against 1.24M** is the from-scratch accumulator: every node pays
for two full 6144 → 512 sums. That is the incremental update's entire
justification and it is Task 4's first job. A 2.6× slowdown is roughly 100 Elo
of search depth, so the net is not worth SPRTing until it is paid back.

Second test, in the engine itself: in debug builds, assert at every node that
the incrementally-updated accumulator equals a from-scratch recomputation. That
one assertion covers the entire class of incremental-update bugs, which is the
other place NNUE integrations go quietly wrong.

### Incremental updates

Not built yet — Task 4 owns it, because there is nothing to hook an incremental
update *into* until the network is what the search calls. The from-scratch path
in `nnue.c` is correct and slow, and `nnue_accumulate()` is the function the
incremental version has to keep agreeing with.

The accumulator is a per-ply stack in the NNUE state, updated as moves are made
and unmade in the search. It does **not** go inside `board_put_piece` and
friends — invariant 5 is about the three board representations staying in
lockstep, and hanging network state off them widens that contract to include
the tuner, perft, and datagen, none of which want an accumulator.

A king move that changes the king's bucket, or crosses the mirror line,
invalidates that perspective's accumulator and forces a full refresh. Finny
tables (cached refresh points) make that cheap and are explicitly **not** in v1.

---

## Task 4 — integration

NNUE replaces `eval_evaluate`. The classical evaluation stays in the tree behind
a build flag, because it is the only reference for "is the net actually better"
and because the tuner still needs it.

Expect the first integration to be **slower in nps and stronger in Elo**, and
expect the margin structure in `search.c` to be wrong afterwards. Every
threshold there — razoring, futility, reverse futility, the SEE bands, the
singular margin — was tuned against an evaluation with a particular scale and a
particular noise profile, and the net has neither. Re-tuning those margins is a
follow-on batch of SPRTs, not part of this one, and it is where a meaningful
fraction of the total NNUE gain will actually be found.

Bench node count changes. That is expected — this is a behavioural change, and
it needs its SPRT like any other. Record the new baseline.

`search_clear()` must reset the accumulator stack (invariant 7).

---

## Task 5 — the network in the search

This is the part with the highest ceiling and the least certainty, so it goes
last and in increasing order of risk.

### 5a. Correction history

Shipped: track the observed difference between the static evaluation and what
the search actually returned, keyed by pawn structure, and correct the static
evaluation by it. E14 measured it at **+25.8** (STC). The obvious extensions
were tried and are dead: E16 added minor-piece, non-pawn and continuation keys
and measured neutral twice. The conditional **mean** of the eval-vs-search
residual is mined out.

### 5b. An uncertainty head for the pruning margins

The second moment of the same idea, and the replacement for the policy head
that used to sit here (cancelled: policy ordering's track record in
alpha-beta engines is thin, history heuristics are already very good, and the
seat is better spent).

Every margin-based prune in `search.c` — reverse futility, futility, razoring,
ProbCut, delta — is one claim in different clothes: that *k* centipawns cover
the gap between the static evaluation and what a deeper search would say, with
*k* a global constant fitted by SPSA to the average position. That residual is
measurably heteroscedastic: over the d12 bench tree the correction magnitude
has **median zero and mean ~6cp** — half the tree is structures where the
evaluation has never been caught drifting, a twentieth is pinned at the
correction clamp, and one margin serves both only by being wrong for both.
ProbCut's own original formulation (Buro) is explicitly a fixed-σ confidence
prune, so this generalises an idea the engine already runs rather than
importing a foreign one.

Order of operations, each SPRT'd:

1. **The probe, no network change**: `unc_scale()` in `search.c` scales the
   five margins by the corrhist magnitude, centred on the measured
   distribution so the node-weighted average margin is unchanged and the SPRT
   measures the conditioning alone. **Measured: +25.61 ± 9.85 at STC, H1
   (E20)** — the margin surface has real structure, and the head below is
   funded. The head's SPRT runs against the probe, which is now the baseline
   to beat.
2. **The head**: a second output on the accumulator trained to predict the
   scale of the residual `|search score − net value|`. The target is already
   in every record — the fixed-node search score is the label the value head
   trains against — so this costs a retrain, not a regeneration.

   **The code is complete end to end**; what remains is the retrain and the
   measurement. `python -m nnue.train --uncertainty` adds the head (L1
   against the detached absolute residual, weighted by `--unc-weight`; the
   value loss is untouched), the exporter writes it behind the header's
   `reserved[0]` flag so every headless net stays loadable by every engine,
   `src/nnue.c` computes it as one extra output-row pass over the
   accumulator the evaluation already keeps, and `unc_scale()` in search.c
   prefers it over the corrhist signal whenever the loaded net carries one.
   The whole path is gated the same way as the value head: the vectors gain
   two columns and the C inference must match them exactly — proven on a
   smoke net before this was written down.

   **Measured: +21.29 ± 9.22 at STC, H1 (E21)**, as a retrain-plus-signal
   batch against the E20 build. `UncSigmaBase`/`UncSigmaSlope` were centred
   on the bench tree's σ distribution (median 47cp, mean ~72cp — far below
   the exporter's vector-set figures, which come from training data rather
   than search). Still owed: LTC confirmation for both E20 and E21, the
   same-net signal A/B if attribution ever matters, and the combination of
   the two signals.
3. **Extensions, one at a time**: aspiration half-width, the null-move
   static-eval condition, LMR confidence.

**Prior art, stated precisely.** The probe's mechanism is not novel: Stockfish
merged corrhist-conditioned futility margins in early 2025 ("corrplexity",
PRs #5735/#5748, plus corrhist-conditioned extensions in #5767), worth 1-2 Elo
there on margins SPSA'd for a decade. E20's +25 against margins that had never
been conditioned at all is the same idea meeting a whole missing class, which
is also why the crude form extends weakly to engines that already carry it.
The **learned head** is the part with no known precedent in an alpha-beta
engine, and that cuts both ways: the seam is unmined, and unmined seams are
usually unmined for a reason. It is also a genuinely different signal from
corrhist - a position-only prior versus a running measurement that adapts
within the game - so the probe's result funds the head without proving it.
The two are not exclusive; if the head passes, combining them is a further
one-SPRT experiment.

---

## What can go wrong

| Failure | Symptom | Where it is caught |
|---|---|---|
| Perspective flipped in training | Net plays reasonably but hates its own position | Task 2 gate: score the start position |
| Quantisation rounding differs | Net is ~30 Elo weak, nothing looks wrong | Task 3: exact-match test |
| Incremental accumulator drift | Rare blunders, unreproducible | Debug assert vs from-scratch |
| Labels contaminated by warm TT | Nets plateau early, more data does not help | Task 1: clear TT before each label search |
| Dataset dominated by near-duplicates | Training loss great, Elo flat | Task 1: dedupe by key, on-disk shuffle |
| Search margins left as-is | NNUE measures neutral, gets abandoned | Task 4: re-tune margins before concluding |

The middle rows are the dangerous ones. They do not crash, they do not show up
in the loss, and they cost exactly enough Elo to make the whole approach look
like it did not work.

---

## Invariants this work must not break

All nine in [../CLAUDE.md](../CLAUDE.md) still apply. Three deserve restating
because NNUE is where they get broken:

- **Bench stays deterministic** (1). The net is fixed data, so this is easy to
  keep — but the node count now depends on which net is embedded, so the net's
  hash must be printed with it.
- **Board mutation goes through the three functions** (5). The accumulator is
  search state, not board state. Keep it out of `board.c`.
- **`search_clear()` resets everything that carries between searches** (7). The
  accumulator stack is now on that list.

Invariants 8 and 9 — `TERM()` and evaluation linearity — govern the *classical*
evaluation and the tuner that fits it. They do not constrain the network, which
is nonlinear by design. They continue to apply to the classical evaluation for
as long as it stays in the tree, and it should stay in the tree.
