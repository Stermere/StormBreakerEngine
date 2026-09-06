/*
 * movegen.h - move generation.
 *
 * Generators write into a caller-supplied array and return the count, so they
 * never allocate and never touch global state.
 */
#ifndef MOVEGEN_H
#define MOVEGEN_H

#include "board.h"
#include "move.h"

/* GEN_EVASIONS requires the side to move to actually be in check, and is narrower
 * than filtering GEN_ALL. GEN_CAPTURES and GEN_QUIETS partition GEN_ALL exactly. */
typedef enum { GEN_CAPTURES, GEN_QUIETS, GEN_EVASIONS, GEN_ALL } GenType;

/* Generates PSEUDO-LEGAL moves - filtering with movegen_is_legal() only for the
 * moves the search actually tries beats generating a legal list up front. `list`
 * must hold MAX_MOVES, and only its `m` field is written. */
int movegen_generate(const Position *pos, GenType type, ScoredMove *list);

/* True if `m` leaves the mover's own king safe; assumes `m` is pseudo-legal. */
bool movegen_is_legal(const Position *pos, Move m);

/* The gate for moves that did not come from the generator. A transposition hit
 * can be stale or collided, and playing one unchecked corrupts the board. */
bool movegen_is_pseudo_legal(const Position *pos, Move m);

#endif
