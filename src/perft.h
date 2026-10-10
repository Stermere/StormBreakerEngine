/* perft.h - move generation correctness: leaf counts must match exactly. */
#ifndef PERFT_H
#define PERFT_H

#include "board.h"

uint64_t perft(Position *pos, int depth);

/* Per-root-move counts, in Stockfish's `go perft` format. */
void perft_divide(Position *pos, int depth);

/* An EPD suite of "<fen> ;D1 <count> ;D2 <count> ..." lines; true if every count matched. */
bool perft_run_suite(const char *path, int maxDepth);

#endif
