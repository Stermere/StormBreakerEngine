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

Dependencies point downward only. Nothing below `search.c` knows the UCI
protocol exists, which is what lets perft and bench drive the same code the GUI
does.

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

Both are maintained together by `board_put_piece` / `board_remove_piece` /
`board_move_piece`, and **nothing else may touch them**. The most common
representation bug in a young engine is these two views silently disagreeing;
routing every mutation through three functions makes that impossible by
construction. `board_is_consistent()` asserts they still agree.

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
the king and rook can start almost anywhere. `move_to_str` translates back to
the `e1g1` form GUIs expect unless `UCI_Chess960` is set.

---

## Threading

`search_start()` hands the search to a worker thread and returns immediately;
the main thread goes straight back to reading stdin.

This is not an optimisation, it is a correctness requirement. `stop` and
`ponderhit` arrive *while the engine is thinking*. An engine that searches on
its main thread cannot see them, overruns its clock, and loses games on time in
every tournament it enters. Retrofitting this later means rewriting the search
entry point, so it is built in from the start.

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

This matters more than it looks. A randomly seeded table changes which
transposition entries collide, which changes the search tree, which changes the
bench node count between runs. That makes:

- pure-speedup patches impossible to verify by node count,
- test results irreproducible,
- the engine ineligible for OpenBench.

`search_clear()` exists for the same reason: anything carrying information
between searches (transposition table, history heuristics, killers) must be
reset on `ucinewgame`, or a game's result depends on which games preceded it.

---

## Known performance work

The bring-up placeholders are all gone. Sliding attacks go through magic
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
- **Chess960 is representation-only.** Moves are encoded
  king-captures-own-rook so the format is ready, but move generation hard-codes
  standard castling geometry and `board_set_fen` rejects Shredder-FEN.
  `UCI_Chess960` currently changes only how castling is spelled to the GUI.

---

## Build model

All sources are compiled in a single compiler invocation rather than to separate
object files. At engine scale a full rebuild takes about a second, and it gives
whole-program optimisation without LTO plumbing or any dependency on shell
builtins that MinGW `make` may not provide. It also keeps the Makefile short
enough that the OpenBench contract stays obvious.
