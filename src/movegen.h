/*
 * movegen.h - move generation.
 *
 * Generators write into a caller-supplied array and return the count. The
 * caller owns the storage (normally a stack array of MAX_MOVES), so generation
 * never allocates and never touches global state - a hard requirement for
 * running searches on multiple threads later.
 */
#ifndef MOVEGEN_H
#define MOVEGEN_H

#include "board.h"
#include "move.h"

typedef enum {
    GEN_CAPTURES, /* captures and queen promotions - the quiescence set */
    GEN_QUIETS,   /* everything else */
    GEN_EVASIONS, /* only moves that answer a check; generate these when in check */
    GEN_ALL       /* every pseudo-legal move */
} GenType;

/*
 * Generates PSEUDO-LEGAL moves: moves that respect piece movement rules but
 * may leave the mover's own king in check. Filtering those out with
 * movegen_is_legal() lazily - only for moves the search actually tries - is
 * measurably faster than generating a strictly legal list up front, because
 * most generated moves are pruned before they are ever played.
 *
 * Returns the number of moves written to `list`, which must hold MAX_MOVES.
 * Only the `m` field is written: `score` belongs to whoever orders the list.
 *
 * GEN_EVASIONS requires the side to move to actually be in check, and is both
 * narrower and faster than filtering GEN_ALL. GEN_CAPTURES and GEN_QUIETS
 * partition GEN_ALL exactly - no move appears in both, and none is missed.
 */
int movegen_generate(const Position *pos, GenType type, ScoredMove *list);

/* True if `m` leaves the side to move's own king safe. Assumes `m` is already
 * known to be pseudo-legal in `pos`. */
bool movegen_is_legal(const Position *pos, Move m);

/*
 * True if `m` is a pseudo-legal move in `pos`.
 *
 * This is the validation gate for moves that did not come out of the
 * generator: transposition table hits and killer moves may be stale, and a
 * hash collision can hand the search a move belonging to a different position.
 * Playing one unchecked corrupts the board and is the single most common cause
 * of crashes in a young engine. Never skip it.
 */
bool movegen_is_pseudo_legal(const Position *pos, Move m);

#endif /* MOVEGEN_H */
