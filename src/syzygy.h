/*
 * syzygy.h - the engine's view of the Syzygy endgame tablebases.
 *
 * Inactive until syzygy_init() finds tables, and while inactive every query
 * answers "not probable" from one branch, so a machine with tables on disk
 * benches identically to one without.
 */
#ifndef SYZYGY_H
#define SYZYGY_H

#include "board.h"
#include "move.h"
#include "types.h"

/* Path list, ';'-separated on Windows and ':' on POSIX. False, prober left
 * inactive, when the path holds nothing usable. */
bool syzygy_init(const char *path);
void syzygy_free(void);

/* Largest piece count the loaded tables cover; 0 while inactive. */
int syzygy_max_pieces(void);

/*
 * VALUE_NONE when the position is not probable: inactive, too many pieces,
 * castling rights, or a nonzero halfmove clock. `pos` is non-const because
 * resolving captures means playing them out on the caller's board, which copying
 * per probe would cost more than the lookup; it is restored exactly.
 */
Value syzygy_probe_wdl(Position *pos, int ply);

typedef struct {
    Move move;
    Value value;
    /* Reported, not played: the one number a differential test can compare against
     * an oracle to prove the DTZ table was decoded, not just that its sign fell out
     * right. */
    int dtz;
} SyzygyRoot;

/*
 * The move is one that provably makes progress, where WDL rates every winning
 * move alike and lets the search shuffle the win away. The value matters too:
 * unlike WDL, DTZ is correct at any halfmove clock, so a root that would
 * otherwise be scored heuristically comes back from the tables.
 *
 * `move` and `value` fail independently - a real value with MOVE_NONE means every
 * child probe declined. Moves are matched against the generator's list, never
 * synthesised.
 */
SyzygyRoot syzygy_probe_root(Position *pos);

#endif
