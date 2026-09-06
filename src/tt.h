/*
 * tt.h - transposition table.
 *
 * A shared, lossy cache of searched positions: the same position is reached by
 * many move orders, and reusing the earlier result collapses the tree.
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
    BOUND_UPPER = 1,
    BOUND_LOWER = 2,
    BOUND_EXACT = BOUND_UPPER | BOUND_LOWER
} Bound;

/* 16 bytes exactly, so four entries share a cache line and a cluster probe costs
 * one miss. A static assertion in tt.c holds the size. */
typedef struct {
    uint16_t key16;
    uint16_t move;
    int16_t value;
    int16_t eval;
    uint8_t depth;
    uint8_t genBound;
    uint8_t pv;
    uint8_t padding[5];
} TTEntry;

/* Go through these: the packing of `genBound` is an implementation detail. */
static inline Move tt_entry_move(const TTEntry *e) { return (Move)e->move; }
static inline Value tt_entry_value(const TTEntry *e) { return (Value)e->value; }
static inline Value tt_entry_eval(const TTEntry *e) { return (Value)e->eval; }
static inline Depth tt_entry_depth(const TTEntry *e) { return (Depth)e->depth; }
static inline Bound tt_entry_bound(const TTEntry *e) { return (Bound)(e->genBound & 3); }

/* Searched with a full window at some point, so it sat on somebody's PV. */
static inline bool tt_entry_is_pv(const TTEntry *e) { return e->pv != 0; }

/* Allocates and clears; driven by the Hash option. False if allocation failed. */
bool tt_resize(size_t mb);

void tt_free(void);

/* Called on `ucinewgame`, so one game's results cannot leak into the next. */
void tt_clear(void);

/* Bumps the generation counter so older entries become preferred replacement
 * victims without paying for a full clear. */
void tt_new_search(void);

/* Permille of the table in use - `info hashfull`. */
int tt_hashfull(void);

size_t tt_size_mb(void);

/*
 * On a hit, copies the entry into `out` and refreshes its generation.
 *
 * Two things the caller must do, both silent corruption if skipped: run the raw
 * `value` through tt_value_from_tt(), and validate tt_entry_move() with
 * movegen_is_pseudo_legal(). key16 is 16 bits, so collisions are routine and the
 * move that comes back may belong to an entirely different position.
 */
bool tt_probe(Key key, TTEntry *out);

/* `value` is absolute and `ply` makes mate scores relative on the way in; pass
 * VALUE_NONE for `eval` when none was computed. `pv` is sticky - an entry loses it
 * only to a different position claiming the slot. */
void tt_store(Key key, Move m, Value value, Value eval, Depth depth, Bound bound, bool pv, int ply);

/* The inverse of what tt_store does to mate scores on the way in. */
Value tt_value_from_tt(Value v, int ply);

/* Called just after do_move, well before the probe, to hide the memory latency. */
void tt_prefetch(Key key);

#endif
