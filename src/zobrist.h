/* zobrist.h - position hashing, from a fixed seed so bench is deterministic (invariant 1). */
#ifndef ZOBRIST_H
#define ZOBRIST_H

#include "types.h"

extern Key ZobristPiece[PIECE_NB][SQUARE_NB];

/* All-ones for the pawn codes, else zero: a branch-free mask for the pawn key. */
extern Key ZobristPawnSelect[PIECE_NB];

extern Key ZobristEnPassant[8];
extern Key ZobristCastling[16];
extern Key ZobristSideToMove;

void zobrist_init(void);

#endif
