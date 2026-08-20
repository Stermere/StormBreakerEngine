# Evaluation tuning

How the evaluation's weights are fitted to real games, and why the procedure is
shaped the way it is.

Read [TESTING.md](TESTING.md) first. Nothing here replaces an SPRT: a tuning run
that lowers its own loss has demonstrated that it fits the data better, which is
not the same claim as "the engine is stronger", and the two come apart often
enough that the difference is the whole reason the SPRT exists.

---

## The pipeline

```powershell
powershell tools\fetch-training-data.ps1 -Months 10   # human games (CC0)
make tuner
.\tuner.exe extract external\training\*.pgn -o external\training\human.epd
.\tuner.exe tune external\training\human.epd -o src\evalparams.c
make && make perft && make bench                     # rebuild and re-verify
pwsh tools\sprt.ps1                                  # and only now, measure
```

| Stage | Input | Output |
|---|---|---|
| `fetch-training-data.ps1` | — | monthly PGN archives in `external\training\` |
| `tuner extract` | PGN | `<fen> [result]` lines, filtered to quiet positions |
| `tuner tune` | those lines | a regenerated `src\evalparams.c` |

---

## What is actually being fitted

Every weight the evaluation owns lives in a table in `src/evalparams.c`, and
every table is registered exactly once in `EVAL_PARAM_TABLES` in
`src/evalparams.h`. There are **6,842 weights**, each a midgame/endgame pair, so
**13,684 free numbers**.

The evaluation is *linear* in all of them. Every term is a weight multiplied by
a coefficient that depends only on the position, so a position's score is a dot
product of a sparse coefficient vector with the weight vector, with the midgame
and endgame halves blended by game phase. That is what makes the whole thing
fittable by plain gradient descent, and it is a property worth protecting: a
term whose *weight* changes which *coefficients* are produced would break it.

`eval.c` reports the coefficients itself. Under `-DTUNE` the `TERM()` macro
records, for each weight it applies, the signed coefficient it applied — so the
tuner reads out what the evaluation did rather than reimplementing it. Add a
term to `eval.c` and the tuner fits it with no changes here.

### Where the parameters go

| Tier | Weights | |
|---|---:|---|
| Material and base piece-square tables | 391 | the classical core |
| **King-relative placement** | **6,144** | 8 king buckets × 6 pieces × 64 squares, twice (own king, enemy king) |
| Mobility | 66 | per piece type, by squares reached |
| Pawn structure | 88 | isolated, doubled, backward, connected, phalanx, passed ×5, candidate |
| King safety | 105 | shelter, storm, attacker count, attack weight, safe checks, open file |
| Pieces | 17 | bishop pair, bad bishop, outposts, rook files |
| Threats | 30 | by pawn/minor/rook/king, hanging, restricted |
| Tempo | 1 | |

The king-relative tier is where nearly all the capacity is, and it is
deliberately NNUE-shaped — a factorised HalfKA feature set. A plain
piece-square table has to average a knight on f5 over positions where the enemy
king is on g8 and positions where it is on a1, which are not the same square at
all. Conditioning on the king splits that average eight ways. It also means the
feature extraction transfers directly to the network's input layer later, so
none of this work is thrown away when NNUE arrives.

---

## The label, and why the games are human

Each position carries the result of the game it came from — 1, ½ or 0 from
white's point of view. Fitting a sigmoid of the score to that result is
logistic regression on win/draw/loss.

The obvious objection is that human games contain blunders, and engine self-play
would give cleaner labels. It would — but cleaner in a way that does not help,
and dirtier in the way that does.

**Symmetric label noise is nearly free.** If a blunder flips a label roughly
independently of the position's features, the effect is a uniform attenuation of
every fitted weight toward zero. A uniform rescale is exactly what the free
parameter `K` absorbs, so the *relative* weights — the only thing the evaluation
uses — survive intact. What noise costs is variance, and variance shrinks as
1/N. N is the cheap thing here: another 200,000 games is a download.

**Error correlated with the features is not free, and more data cannot fix it.**
That is what self-play produces. Every build in `external/games/` shares one
evaluation family; none of them has any notion of king safety. In games between
two such engines both sides shatter their own king shelter and neither charges a
price until it becomes tactically visible at depth. The label then reports that
a broken shelter costs nothing — not as noise around the truth, but as the same
systematic pull toward zero in every position, on precisely the weights being
added. Self-play data is also generated *by* the function being replaced, so it
is dense where the evaluation is already right and thin exactly where it is
wrong.

Once there is a labeller worth trusting this reverses completely, because the
label stops being a game outcome and becomes a search score — and a depth-N
score does not care what got played next. That is the NNUE stage, and there
positions-from-anywhere plus labels-from-our-own-search is correct. It cannot
bootstrap this stage: labelling with the current evaluation's search would only
teach the new weights to imitate the old ones.

The `extract` filters exist to protect the label rather than to clean it:

| Filter | Default | Why |
|---|---|---|
| `-minply` | 12 | opening theory is memorised, not evaluated |
| `-stride` | 6 | consecutive positions in one game are near-identical; counting them all would overstate the evidence |
| `-maxscore` | 2000 | a position this decided teaches only that winning positions win |
| `-minelo` | 0 | raise it to trade volume for label quality |
| `-minbase` | 0 | seconds of base time; a 2300 rapid game is a better label than a 2300 bullet game |

and one filter that protects the *model*: a position is kept only if standing
pat already beats every capture, checked with a real quiescence search. Fitting
on tactical positions would ask the evaluation to predict the outcome of a
capture sequence, which is the search's job and which a linear function of
piece placement has no means to represent. Those positions are pure noise here.

---

## Two decisions in the fit that are not obvious

**`K` is fitted first, then frozen.** `K` is the sigmoid's scale in centipawns.
It and the overall magnitude of the weights are the same degree of freedom —
doubling every weight and doubling `K` gives an identical loss. If both are left
free the fit is happy to drift the score off the centipawn scale, and every
margin in `search.c` is written in centipawns: razoring, futility, the delta
pruning bound, the aspiration window. Pinning `K` to the value that best fits
the *starting* weights keeps the fitted evaluation on the same scale the search
already assumes. Expect `K ≈ 350–400`.

**Weight decay applies only to the king-relative tables.** Those 6,144 weights
are collinear with the base piece-square tables by construction: anything they
can express, the base tables can express on average. Decay resolves the
ambiguity in the direction we want — the base tables carry the average, the
buckets hold only the deviation from it. That is both the interpretation that
makes `eval_trace` readable and the one that generalises, since a bucket with
few training positions decays back toward the base table rather than fitting
noise. The hand-written terms are left undecayed; there is nothing sensible for
them to shrink towards.

The held-out split is the **last** *N*% of the file, not a random sample. The
file is in game order, so a contiguous tail splits by game. Splitting randomly
would put positions from the same game — sharing a label, and nearly identical
to each other — on both sides, and the validation loss would then measure
memorisation rather than generalisation.

---

## How much data

13,684 free numbers. The overfitting is not subtle when there is too little
data — on 11,000 positions the training loss falls by more than half while the
validation loss rises from the first epoch, and early stopping correctly returns
the weights it started with.

| Positions | Per number | Verdict |
|---|---:|---|
| 10 thousand | 0.7 | overfits immediately; the tuner will refuse to improve |
| 1 million | 73 | the classical terms fit; the king buckets are noise |
| 10 million | 730 | workable |
| 25 million+ | 1,800+ | what the king-relative tier actually wants |

One month of Lichess Elite is roughly 250,000 games, and `-stride 6` yields
about twelve quiet positions per game — call it 3 million positions per month
downloaded. Memory is about 68 features per position at 4 bytes, so 25 million
positions needs roughly 7 GB of RAM for the feature pool. `-max N` caps the
count if that does not fit.

---

## Adding a term

1. Add a line to `EVAL_PARAM_TABLES` in `src/evalparams.h`.
2. Add its defaults to `src/evalparams.c`.
3. Apply it with `TERM()` in `src/eval.c`.

Nothing else changes. The flat index space, the extern declarations and the
descriptor list the tuner walks are all generated from that one registry entry,
so they cannot disagree with each other — which matters, because if they did,
the tuner would optimise one weight while the evaluation applied a different
one and nothing would crash.

Then re-run the fit, rebuild, and SPRT it. A new term is a behavioural change
like any other.

---

## Sanity checks before trusting a fit

- **Colour symmetry.** Mirroring a position — swap colours, flip the board, swap
  the castling rights and the en-passant rank — must score identically, because
  the evaluation is side-to-move relative. This is the check that catches sign
  errors and normalisation bugs in the king-relative indexing, which are
  otherwise close to invisible.
- **`make perft` still exact.** Evaluation changes cannot legitimately affect
  move generation; if perft moves, something is badly wrong.
- **The rounding cost.** The tuner reports the validation loss after rounding
  the fitted doubles back to `int16`. It should be a rounding error. If it is
  not, some weight has been fitted to a magnitude the table cannot hold.
- **Read the highlights.** The tuner prints fitted material, bishop pair, tempo,
  the passed-pawn curve and the king-attacker curve. Material near
  100/320/330/500/900 and a monotone passed-pawn curve mean the fit is sane.
  Material at 40/700 means it is not.
