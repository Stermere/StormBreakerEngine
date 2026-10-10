/* tt.h - the transposition table, shared by every search thread. */
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

/* 16 bytes, so a four-entry cluster is one cache line (asserted in tt.c). */
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

static inline Move tt_entry_move(const TTEntry *e) { return (Move)e->move; }
static inline Value tt_entry_value(const TTEntry *e) { return (Value)e->value; }
static inline Value tt_entry_eval(const TTEntry *e) { return (Value)e->eval; }
static inline Depth tt_entry_depth(const TTEntry *e) { return (Depth)e->depth; }
static inline Bound tt_entry_bound(const TTEntry *e) { return (Bound)(e->genBound & 3); }

/* The position was once searched as a PV node. */
static inline bool tt_entry_is_pv(const TTEntry *e) { return e->pv != 0; }

/* The Hash option. False if allocation failed. */
bool tt_resize(size_t mb);

void tt_free(void);

void tt_clear(void);

/* New generation: older entries become preferred replacement victims. */
void tt_new_search(void);

/* Permille in use, for `info hashfull`. */
int tt_hashfull(void);

size_t tt_size_mb(void);

/* On a hit, copies the entry to `out`. The caller must convert the value with
 * tt_value_from_tt() and validate the move with movegen_is_pseudo_legal(): 16-bit keys
 * collide. */
bool tt_probe(Key key, TTEntry *out);

/* `ply` makes mate scores node-relative on the way in; `eval` may be VALUE_NONE. `pv` is
 * sticky until another position takes the slot. */
void tt_store(Key key, Move m, Value value, Value eval, Depth depth, Bound bound, bool pv, int ply);

Value tt_value_from_tt(Value v, int ply);

/* Call right after do_move, well before the probe. */
void tt_prefetch(Key key);

#endif
