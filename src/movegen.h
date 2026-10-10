/* movegen.h - move generation into a caller-supplied array. */
#ifndef MOVEGEN_H
#define MOVEGEN_H

#include "board.h"
#include "move.h"

/* GEN_EVASIONS only in check. GEN_CAPTURES/GEN_QUIETS partition GEN_ALL, and so do
 * GEN_TACTICALS (all promotions included) and GEN_NON_TACTICALS (none). */
typedef enum {
    GEN_CAPTURES,
    GEN_QUIETS,
    GEN_EVASIONS,
    GEN_ALL,
    GEN_TACTICALS,
    GEN_NON_TACTICALS
} GenType;

/* Pseudo-legal moves; `list` must hold MAX_MOVES, and only `m` is written. */
int movegen_generate(const Position *pos, GenType type, ScoredMove *list);

/* Whether a pseudo-legal `m` leaves the mover's king safe. */
bool movegen_is_legal(const Position *pos, Move m);

/* Required for any move not from the generator, such as a TT move (invariant 6). */
bool movegen_is_pseudo_legal(const Position *pos, Move m);

#endif
