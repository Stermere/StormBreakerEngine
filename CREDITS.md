# Credits and provenance

StormBreaker uses established chess-programming techniques and was developed
with substantial AI coding assistance under human direction. This document
records direct reuse, technical influences, training-data sources, and the
steps taken to check that the implementation is distinct from other engines.

## Direct reuse

### Syzygy tablebase probing — derived from Fathom

[`src/syzygy.c`](src/syzygy.c) is StormBreaker's own Syzygy prober, but it is a
**derivative work**, not an independent one. It was written by studying Jon
Dart's [Fathom](https://github.com/jdart1/Fathom) at commit
`c9c6fef0dddc05d2e242c183acf5833149ab676d`, itself derived from Ronald de Man's
reference implementation. Copyright © 2013-2018 Ronald de Man, © 2015 basil00,
© 2016-2025 Jon Dart, under the MIT licence, whose text is retained at
[`LICENSE.fathom`](LICENSE.fathom). MIT is GPL-3.0 compatible, so the
combined work remains distributable under StormBreaker's GPL-3.0.

Calling it derived is not a formality. A file format is not something an
implementation gets to choose: the constant index tables (`KKIdx`, `Triangle`,
`Flap`, `PawnTwist`, `OffDiag`, `FlipDiag`, `Lower`, `Diag`), the material-key
primes, and the shape of the index encoder and the re-pair decompressor could
not be written differently and still read a Syzygy file. Those parts follow
Fathom line for line in substance.

What is StormBreaker's own is everything that touches a position. Fathom
carries its own board representation, move generator and attack tables
(`tbchess.c`, ~1,050 lines); this engine already has all three, so placement
reads our bitboards directly, capture resolution uses our generator and our
make/unmake on the caller's board, and no position is marshalled through a
second representation on the way into a probe. Depth-to-mate support (~350
lines, for `.rtbm` files no distribution ships) and the `TbRootMoves` helper
API are gone, and the per-table `EncInfo` arrays are compacted accordingly.

**Fathom was also the test oracle.** The two implementations were linked side
by side and compared position by position across every material configuration —
see docs/EXPERIMENTS.md E24 for the campaign and its result. Nothing about that
verification survives in the shipped tree except its verdict, frozen as a
checksum manifest that `make syzygy-test` re-checks without Fathom present.

**On the no-new-dependencies rule.** The engine still links only libc and the
platform threading API; the tablebase *files* are data, downloaded into
gitignored `external/` and never redistributed. The reason this was worth
writing rather than vendoring is the same reason it was worth verifying so
heavily: the `.rtbw` format is re-pair-compressed with symbol tables and sparse
block indices, and a decoder with a subtle bug does not crash — it returns a
plausible result that the search believes and the data generator writes into
every low-piece label.

### Magic seeds

`MagicSeeds` array in [`src/bitboard.c`](src/bitboard.c):

```c
static const uint64_t MagicSeeds[8] = {8977, 44560, 54343, 38998, 5731, 95205, 104912, 17020};
```

These values come from Stockfish's 64-bit `seeds[][RANK_NB]` row in
`init_magics`. The source comment attributes them to Stockfish. Stockfish and
StormBreaker are distributed under GPL-3.0.

The values seed the search for valid magic multipliers during startup. They do
not define the resulting sliding-piece attacks; each candidate is checked
against the reference `slide()` implementation for every occupancy.

## Published techniques

The engine implements standard techniques described in chess-programming
literature and used by many other engines:

| Area | Techniques |
|---|---|
| Search | principal variation search, iterative deepening, aspiration windows |
| Pruning | null move, reverse futility, futility, razoring, ProbCut, SEE pruning, late move pruning, delta pruning |
| Reductions | late move reductions, internal iterative reduction |
| Extensions | singular extensions, multi-cut, negative extensions |
| Ordering | MVV-LVA, killers, counter-moves, butterfly, capture, and continuation history |
| Evaluation | tapered evaluation, HalfKA-style NNUE features, SCReLU, output buckets |
| Correction | pawn-structure-keyed correction history |
| Tables | magic/PEXT sliding attacks, Zobrist hashing, lockless transposition table |

The magic-bitboard implementation follows the published "fancy" magic method,
including Carry-Rippler subset enumeration and the usual sparsity filter. The
[Chess Programming Wiki](https://www.chessprogramming.org/) documents these
techniques and the broader search methods listed above.

`history_update` in [`src/search.c`](src/search.c) uses the standard history
gravity formula:

```c
*entry += b - *entry * abs(b) / HISTORY_MAX;
```

Equivalent formulas appear in Ethereal, Weiss, and other engines. The bonus
and malus curves and their constants are StormBreaker parameters and were
fitted separately. Correction history uses a depth-weighted exponential moving
average instead of this formula.

## Parameters and network

- Classical evaluation weights in [`src/evalparams.c`](src/evalparams.c) are
  produced by `tools/tuner.c` from game outcomes. See
  [docs/TUNING.md](docs/TUNING.md).
- Search parameters are fitted with the SPSA driver in `tools/tune.py`. Results
  are recorded in [docs/EXPERIMENTS.md](docs/EXPERIMENTS.md).
- The NNUE is trained with the code in [`trainer/`](trainer/). Its file format
  is the project-specific `NnueHeader` layout defined in
  [`src/nnue.h`](src/nnue.h), not Stockfish's `SFNNv*` format.

## Training-data provenance

| Source | Material used |
|---|---|
| StormBreaker self-play games | positions and StormBreaker search labels |
| StormBreaker search-tree samples | positions and StormBreaker search labels |
| Lichess Elite database (CC0) | positions and game outcomes |
| CCRL game archives | positions |

Training labels are generated by StormBreaker's fixed-node search. The
`datagen verify -relabel` command checks labels by searching the stored
positions again. Dataset construction and verification are described in
[docs/NNUE.md](docs/NNUE.md) and [trainer/README.md](trainer/README.md).

## Compatible interfaces

Two output formats intentionally follow external conventions:

- `go perft <depth>` uses Stockfish's per-move breakdown format to support
  direct comparison.
- The final line of `bench` follows the `<nodes> nodes <nps> nps` format
  required by OpenBench.

The engine also uses the common little-endian rank-file square mapping
(`A1 = 0`, `H8 = 63`). These are interface and representation conventions, not
shared implementations.

## Development and source review

StormBreaker was developed with substantial AI coding assistance under human
direction. The following measures were used to establish a baseline of
independent implementation and to identify material requiring attribution:

- Tracked source files were compared with current Stockfish, Ethereal, Weiss,
  Berserk, and Viridithas sources for matching constants, identifiers, comment
  text, and structure.
- The review identified the Stockfish magic seeds and the standard history
  gravity formula described above. No other matching source block was found.
- Evaluation weights, search parameters, and network weights are generated by
  this repository's tuner, SPSA driver, and trainer.
- The network format, build system, data generator, and supporting tools are
  maintained in this repository.
- [docs/EXPERIMENTS.md](docs/EXPERIMENTS.md) records accepted and rejected
  changes, while deterministic bench counts provide a reproducible history of
  search behavior.
- Perft, sanitizer, and cross-platform CI checks verify implementation
  behavior. They are correctness checks, not evidence of authorship.

This review is a practical check rather than a formal proof of originality.
Common algorithms naturally produce similar formulas, terminology, and data
structures. Suspected missing attribution can be reported through the issue
tracker.

## Tools and dependencies

| Project | Use |
|---|---|
| [fastchess](https://github.com/Disservin/fastchess) | match runner and SPRT driver |
| [Cute Chess](https://cutechess.com/) | GUI and `cutechess-cli` |
| [En Croissant](https://encroissant.org/) | analysis GUI |
| [OpenBench](https://github.com/AndyGrant/OpenBench) | distributed SPRT testing |
| [Stockfish](https://stockfishchess.org/) | reference opponent and perft comparison |
| Halogen, Berserk, Weiss, Clover, Ethereal | CCRL-rated gauntlet opponents |
| [Fathom](https://github.com/jdart1/Fathom) | the Syzygy prober `src/syzygy.c` derives from it, and it was the oracle that verified it (see Direct reuse) |
| [Syzygy tablebases](https://github.com/syzygy1/tb) | Ronald de Man's endgame tables; `make syzygy-fetch` pulls 3-4-5-man from the lichess.org mirror |

The engine links against the C standard library and the platform threading API.
The tablebase *files* are data, not a dependency: they are downloaded into
gitignored `external/`, never redistributed, and the engine runs without them.
The trainer uses PyTorch and NumPy. The repository's Python tools otherwise use
the standard library.

## License

StormBreaker is distributed under GPL-3.0. See [LICENSE](LICENSE).
