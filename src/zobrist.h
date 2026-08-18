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
extern Key ZobristEnPassant[8]; /* indexed by file */
extern Key ZobristCastling[16]; /* indexed by the CastlingRights bitmask */
extern Key ZobristSideToMove;

void zobrist_init(void);

#endif /* ZOBRIST_H */
