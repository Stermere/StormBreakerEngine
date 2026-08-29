/*
 * tt.h - transposition table.
 *
 * A shared, lossy cache of previously searched positions. It is the single
 * highest-value data structure in the engine: the same position is reached by
 * many move orders, and reusing the earlier result collapses the search tree.
 */
#ifndef TT_H
#define TT_H

#include <stddef.h>

#include "board.h"
#include "move.h"
#include "types.h"

/* How a stored score relates to the true value of the position. */
typedef enum {
    BOUND_NONE  = 0,
    BOUND_UPPER = 1, /* fail-low:  true value is at most `value` */
    BOUND_LOWER = 2, /* fail-high: true value is at least `value` */
    BOUND_EXACT = BOUND_UPPER | BOUND_LOWER
} Bound;

/*
 * 16 bytes exactly, so four entries share a 64-byte cache line and a cluster
 * probe costs one cache miss. Verified by a static assertion in tt.c - if you
 * add a field, keep the size at 16 or accept a real slowdown.
 */
typedef struct {
    uint16_t key16;   /* upper bits of the Zobrist key; full key is not stored */
    uint16_t move;    /* best move found, as a packed Move */
    int16_t value;    /* score, with mate scores made ply-relative */
    int16_t eval;     /* static eval, cached to avoid recomputing it */
    uint8_t depth;    /* search depth this entry was produced at */
    uint8_t genBound; /* generation in the high bits, Bound in the low 2 */
    uint8_t pv;       /* the position was on a principal variation at some point */
    uint8_t padding[5];
} TTEntry;

/* Field accessors. The packing of `genBound` is an implementation detail; go
 * through these so it can change without touching the search. */
static inline Move tt_entry_move(const TTEntry *e) { return (Move)e->move; }
static inline Value tt_entry_value(const TTEntry *e) { return (Value)e->value; }
static inline Value tt_entry_eval(const TTEntry *e) { return (Value)e->eval; }
static inline Depth tt_entry_depth(const TTEntry *e) { return (Depth)e->depth; }
static inline Bound tt_entry_bound(const TTEntry *e) { return (Bound)(e->genBound & 3); }

/*
 * True if this position was ever searched with a full window - so it sat on
 * somebody's principal variation, however long ago.
 *
 * Spends a whole byte of what was padding on a single bit, deliberately. The
 * alternative is stealing a bit from `genBound`, which halves the generation
 * cycle and so quietly changes how the replacement policy ages entries. The
 * padding was doing nothing; this way the flag costs neither size nor policy.
 */
static inline bool tt_entry_is_pv(const TTEntry *e) { return e->pv != 0; }

/* Allocates (or reallocates) the table to `mb` megabytes and clears it.
 * Driven by the Hash UCI option. Returns false if allocation failed. */
bool tt_resize(size_t mb);

/* Releases the table. Called at exit. */
void tt_free(void);

/* Zeroes every entry. Called on `ucinewgame` so results from the previous game
 * cannot leak into the next one and make matches irreproducible. */
void tt_clear(void);

/* Bumps the generation counter so entries from earlier searches become
 * preferred replacement victims without paying for a full clear. */
void tt_new_search(void);

/* Permille of the table in use - reported to GUIs as `info hashfull`. */
int tt_hashfull(void);

/* Current size in megabytes. */
size_t tt_size_mb(void);

/*
 * Looks `key` up. On a hit, copies the entry into `out` and refreshes its
 * generation so a live entry is not evicted by a merely deeper one.
 *
 * TWO THINGS THE CALLER MUST DO, both of which are silent corruption if
 * skipped:
 *
 *   - Run the raw `value` through tt_value_from_tt() with the current ply.
 *     Mate scores are stored as "mate in N from this node"; using one at a
 *     different distance from the root makes the engine announce - and play
 *     for - mates it cannot deliver.
 *
 *   - Validate tt_entry_move() with movegen_is_pseudo_legal() before playing
 *     it. key16 is 16 bits, so collisions are routine rather than exotic, and
 *     the move that comes back may belong to an entirely different position.
 */
bool tt_probe(Key key, TTEntry *out);

/* Stores a result. `value` is absolute (as the search sees it); `ply` is used
 * to make mate scores relative on the way in. Pass VALUE_NONE for `eval` when
 * no static evaluation was computed at this node. `pv` records that this node
 * was searched with a full window; it is sticky, so an entry never loses the
 * flag except to a different position claiming its slot. */
void tt_store(Key key, Move m, Value value, Value eval, Depth depth, Bound bound, bool pv, int ply);

/* Converts a probed score back to one that is meaningful at `ply`. The inverse
 * of what tt_store does on the way in. */
Value tt_value_from_tt(Value v, int ply);

/* Hints the CPU to start loading this key's cluster. Called just after
 * do_move, well before the probe, to hide the memory latency. */
void tt_prefetch(Key key);

#endif /* TT_H */
