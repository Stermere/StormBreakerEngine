/*
 * perft.h - move generation correctness testing.
 *
 * Published leaf counts are exact, so one wrong number means the engine plays or
 * accepts illegal moves.
 */
#ifndef PERFT_H
#define PERFT_H

#include "board.h"

uint64_t perft(Position *pos, int depth);

/* Per-root-move breakdown, for bisecting a wrong total against another engine.
 * Stockfish prints the same format via `go perft <depth>`. */
void perft_divide(Position *pos, int depth);

/* Runs an EPD suite of "<fen> ;D1 <count> ;D2 <count> ..." lines. True only if
 * every depth of every position matched, so it doubles as a CI exit status. */
bool perft_run_suite(const char *path, int maxDepth);

#endif
