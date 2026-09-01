/*
 * perft.h - move generation correctness testing.
 *
 * Perft counts the leaf nodes of the legal move tree to a fixed depth. The
 * counts for well-known positions are published and exact, so comparing
 * against them detects essentially every move generation and make/unmake bug
 * there is. A single wrong number means the engine plays or accepts illegal
 * moves, which surfaces as forfeited games and as noise that makes every Elo
 * measurement meaningless.
 */
#ifndef PERFT_H
#define PERFT_H

#include "board.h"

uint64_t perft(Position *pos, int depth);

/* Per-root-move breakdown, the standard way to bisect a wrong total: compare
 * against another engine's divide output and recurse into the move that
 * disagrees. Stockfish prints the same format via `go perft <depth>`. */
void perft_divide(Position *pos, int depth);

/*
 * Runs an EPD suite. Each line is:
 *     <fen> ;D1 <count> ;D2 <count> ...
 * Prints a per-position pass/fail line. Returns true only if every depth of
 * every position matched, so it can be used directly as a CI exit status.
 */
bool perft_run_suite(const char *path, int maxDepth);

#endif /* PERFT_H */
