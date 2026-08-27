/*
 * zobrist.h - incremental position hashing.
 *
 * DETERMINISM CONTRACT: the key tables are generated from a hard-coded seed,
 * never from time(), rand() or /dev/urandom. OpenBench rejects an engine whose
 * `bench` node count varies between runs or machines, and a randomly seeded
 * Zobrist table is the classic cause - it perturbs transposition table hits
 * and therefore the entire search tree. Do not "improve" this with entropy.
 */
#ifndef ZOBRIST_H
#define ZOBRIST_H

#include "types.h"

extern Key ZobristPiece[PIECE_NB][SQUARE_NB];

/* All-ones for the two pawn codes and zero for every other piece. The board
 * mutators fold a piece into the pawn key by ANDing its piece key with this,
 * which keeps a pawn test out of the hottest function in make/unmake for 128
 * bytes that never leave L1. */
extern Key ZobristPawnSelect[PIECE_NB];

extern Key ZobristEnPassant[8]; /* indexed by file */
extern Key ZobristCastling[16]; /* indexed by the CastlingRights bitmask */
extern Key ZobristSideToMove;

void zobrist_init(void);

#endif /* ZOBRIST_H */
