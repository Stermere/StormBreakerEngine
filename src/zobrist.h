/*
 * zobrist.h - incremental position hashing.
 *
 * The tables come from a hard-coded seed, never from entropy: a randomly seeded
 * table perturbs transposition hits and with them the bench node count.
 */
#ifndef ZOBRIST_H
#define ZOBRIST_H

#include "types.h"

extern Key ZobristPiece[PIECE_NB][SQUARE_NB];

/* All-ones for the two pawn codes, zero for everything else. The mutators AND a
 * piece key with this to fold it into the pawn key, which keeps a pawn test out
 * of make/unmake. */
extern Key ZobristPawnSelect[PIECE_NB];

extern Key ZobristEnPassant[8];
extern Key ZobristCastling[16];
extern Key ZobristSideToMove;

void zobrist_init(void);

#endif
