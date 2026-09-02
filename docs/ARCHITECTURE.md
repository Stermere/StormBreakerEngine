# Architecture

## Module map

```
main.c        startup, initialisation order, argv dispatch
  |
  +-- uci.c        protocol: parses commands, owns the root Position
  |     |
  |     +-- search.c     search driver + worker thread
  |     |     |
  |     |     +-- movegen.c   move generation
  |     |     +-- eval.c      static evaluation
  |     |     |     +-- evalparams.c  the 6842 tunable weights
  |     |     +-- tt.c        transposition table
  |     |     +-- timeman.c   clock allocation
  |     |
  |     +-- perft.c     movegen correctness testing
  |     +-- bench.c     deterministic node-count benchmark
  |
  +-- board.c      position state, FEN I/O, hashing, make/unmake
        |
        +-- bitboard.c   attack tables
        +-- zobrist.c    fixed-seed hash keys

thread.c      portable Win32/pthreads shim
types.h       squares, pieces, colours, values, CPU intrinsics
move.h        16-bit packed move encoding

tools/tuner.c   offline weight fitting; links everything above except main.c
```

Dependencies point downward only. Code below `search.c` does not depend on the
UCI protocol, so perft, bench, and the GUI all use the same engine code.

`tools/tuner.c` is the one thing outside that tree. It is not part of the engine
binary - `make tuner` builds it separately, with `-DTUNE` - but it links the
same `eval.c`, which is the point: the weights it fits are fitted against the
evaluation that will actually run, not a reimplementation of it. See
[TUNING.md](TUNING.md).

---

## Board representation

A **hybrid**: bitboards plus a mailbox array.

- `byType[]` / `byColor[]` — one `uint64_t` per piece type and colour. Set-wise
  operations (all knight attacks, all pawn pushes) become single instructions.
- `board[64]` — piece on each square. Answers "what is on e4?" with one load
  instead of scanning six bitboards.

Both are maintained by `board_put_piece`, `board_remove_piece`, and
`board_move_piece`. `board_is_consistent()` checks that the two representations
agree.

Squares are little-endian rank-file: `A1 = 0`, `H8 = 63`, `square = rank*8 +
file`. This is the near-universal convention, so published bitboard techniques
translate without adaptation.

Pieces are encoded `(color << 3) | type`, making colour extraction `p >> 3` and
type extraction `p & 7`, with `NO_PIECE == 0` so an empty square is falsy.

---

## Move encoding

16 bits: 6 for origin, 6 for destination, 2 for promotion piece, 2 for move
type. Small moves keep move lists cache-resident and let the transposition
table store a move in half a word.

Castling is encoded **king-captures-own-rook**. This looks strange for standard
chess, but it is the only encoding that remains unambiguous in Chess960, where
the king and rook can start almost anywhere: a king that starts on b1 castles
long to c1, and `b1c1` is also an ordinary king step. `move_to_str` translates
back to the `e1g1` form GUIs expect for positions that cannot be Chess960, and
takes the position's own `chess960` flag rather than reading a global — the two
could otherwise drift, and the drift is silent, because every count and score
stays correct while the engine hands the GUI a string naming two legal moves.

---

## Castling geometry

Standard chess has two castling geometries per colour and Chess960 has
hundreds, so the squares involved are **derived per position** rather than
looked up in a constant table. `board_set_fen` resolves the castling field into
four entries on `Position` — the rook's origin, the squares that must be empty,
and the squares the king occupies or crosses — and move generation reads only
those. Standard chess resolves to exactly the squares the old constant table
held, so there is one code path and no variant switch anywhere below the FEN
parser.

Three consequences are worth knowing:

- **The king's castling origin is not stored.** A surviving right means the
  king has not moved, so `king_square()` already is that square. A second copy
  would be one more thing that could drift.
- **The geometry is never saved in `Undo`.** Rights are only ever removed, and
  a removed right never consults its geometry again. The corollary is that the
  entries for a *dead* right go stale, deliberately, and nothing may read them
  without checking the right first.
- **The castling rook can screen its own king.** With the king on c1, its rook
  on b1 and an enemy queen on a1, castling long leaves the king exactly where
  it stood — in check — because the rook that was blocking the queen has just
  left. The king's path scans as safe while the rook is still on the board, so
  `movegen_is_legal` additionally rejects a castle whose rook is pinned. This
  is impossible in standard chess, where a rook on a1 or h1 has no square
  behind it to be pinned from.

Both FEN spellings are accepted: `KQkq` (X-FEN, naming the outermost rook on
each side) and `AHah` (Shredder, naming the rook's file). Shredder is required
when a colour has two rooks on one side of its king, because "the outermost
one" cannot then say which may castle, and it is what `board_to_fen` emits for
a Chess960 position for the same reason.

---

## Threading

`search_start()` hands the search to a worker thread and returns immediately;
the main thread goes straight back to reading stdin.

This allows the main thread to process `stop` and `ponderhit` while a search is
running, which is required for correct UCI time control and pondering.

`thread.c` is a thin shim over Win32 threads and pthreads. C11 `<threads.h>` is
deliberately avoided: MinGW-w64 does not ship it, and depending on winpthreads
would mean shipping an extra DLL.

The worker also honours a subtle protocol rule: during a ponder or an infinite
search it must **not** send `bestmove` until the GUI sends `stop` or
`ponderhit`. Replying early desynchronises the GUI.

---

## Determinism

Zobrist keys are generated from a hard-coded seed with xorshift64*, never from
`time()` or `rand()`.

A random seed changes transposition-table collisions and can change the search
tree and bench node count between runs. Fixed keys provide:

- pure-speedup patches impossible to verify by node count,
- test results irreproducible,
- the engine ineligible for OpenBench.

`search_clear()` exists for the same reason: anything carrying information
between searches (transposition table, history heuristics, killers) must be
reset on `ucinewgame`, or a game's result depends on which games preceded it.

---

## Known performance work

Sliding attacks use magic
bitboards (or PEXT where `USE_PEXT` is defined — it is behind its own flag
because it is microcoded and very slow on AMD Zen 1/2); ray-walking survives
only as the reference `slide()` that the magic tables are *built* from and
verified against. Check and pin information is cached in `Undo` rather than
recomputed. En passant is folded into the hash only when a pawn can actually
capture, so positions that differ by an unusable ep right share an entry.

What is left is structural rather than incremental:

- **The search is single-threaded.** `Threads` is advertised as `min 1 max 1`
  and rejects anything else. Lazy SMP is the standard answer and needs the
  ordering tables, which are currently file-scope, moved into a per-thread
  block first.
- **Move generation is not staged.** Every node generates its whole move list
  and scores all of it, including at nodes where the transposition move cuts
  immediately. A staged picker — table move, then captures, then quiets,
  generated only when reached — avoids that work.
- **No correction history.** Nothing feeds the difference between the static
  evaluation and the searched score back into later static evaluations.

---

## Build model

All sources are compiled in a single compiler invocation rather than to separate
object files. At engine scale a full rebuild takes about a second, and it gives
whole-program optimisation without LTO plumbing or any dependency on shell
builtins that MinGW `make` may not provide. It also keeps the Makefile short
enough that the OpenBench contract stays obvious.
