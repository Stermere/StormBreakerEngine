/*
 * tt.h - transposition table.
 *
 * A shared, lossy cache of previously searched positions. It is the single
 * highest-value data structure in the engine: the same position is reached by
 * many move orders, and reusing the earlier result collapses the search tree.
 *
 * Allocation and lifetime are implemented; probe/store are TODO.
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
    uint8_t padding[6];
} TTEntry;

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
 * TODO(engine): implement probe and store.
 *
 * Two details that are easy to get wrong and expensive to debug:
 *   - MATE SCORES must be stored relative to the current ply and converted
 *     back on probe. A mate score is "mate in N from here"; storing it
 *     absolutely makes the engine announce mates it cannot deliver.
 *   - Always validate a probed move with movegen_is_pseudo_legal() before
 *     playing it. key16 is only 16 bits, so collisions are routine.
 */
bool tt_probe(Key key, TTEntry *out);
void tt_store(Key key, Move m, Value value, Value eval, Depth depth, Bound bound, int ply);

/* Hints the CPU to start loading this key's cluster. Called just after
 * do_move, well before the probe, to hide the memory latency. */
void tt_prefetch(Key key);

#endif /* TT_H */
