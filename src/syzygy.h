/*
 * syzygy.h - the engine's view of the Syzygy endgame tablebases.
 *
 * Inactive until syzygy_init() finds tables, and inactive means every query
 * answers "not probable" from one branch - a build that never loads
 * tablebases behaves bit-identically to one built without this file, which is
 * what keeps bench deterministic across machines. The prober itself is
 * src/syzygy.c; CREDITS.md records what it derives from.
 */
#ifndef SYZYGY_H
#define SYZYGY_H

#include "board.h"
#include "move.h"
#include "types.h"

/* Load tables from a path list (';'-separated on Windows, ':' on POSIX).
 * Whatever was loaded before is released first. Returns false - with the
 * prober left inactive - when the path holds nothing usable. */
bool syzygy_init(const char *path);
void syzygy_free(void);

/* Largest piece count the loaded tables cover; 0 while inactive. */
int syzygy_max_pieces(void);

/*
 * WDL probe for interior nodes. VALUE_NONE when the position is not probable:
 * prober inactive, too many pieces, castling rights, or a nonzero halfmove
 * clock - WDL tables assume the fifty-move counter is fresh, so only the
 * capture or pawn move that entered the tablebase region probes.
 *
 * A win is VALUE_TB_WIN - ply, a loss the mirror; cursed wins and blessed
 * losses score VALUE_DRAW, because the fifty-move rule is in force.
 *
 * `pos` is NOT const, and that is the honest signature rather than an
 * oversight. A tablebase holds only positions with a zero fifty-move counter,
 * so resolving one with captures available means playing them out - the probe
 * makes moves and takes them back. It does so on the caller's board because a
 * Position carries its whole repetition history and copying one per probe
 * would cost more than the lookup. The board is restored exactly.
 */
Value syzygy_probe_wdl(Position *pos, int ply);

/* What a root probe found. Both fields are the "not probable" sentinel
 * together: MOVE_NONE and VALUE_NONE. */
typedef struct {
    Move move;   /* preserves the result; matched against the generator's list */
    Value value; /* the true result at ply 0, fifty-move rule included */
    int dtz;     /* plies to the next zeroing move that keeps the result */
} SyzygyRoot;

/*
 * DTZ probe for the root, which answers two questions with one lookup.
 *
 * The move is what converts: WDL alone rates every winning move identically,
 * so a search free to choose among them can shuffle until the fifty-move rule
 * takes the win away. DTZ names one that provably makes progress.
 *
 * The value is why this is also the labelling path. Interior nodes only probe
 * at halfmoveClock == 0, so the children of a five-man root - clock 1 after
 * any piece move - are searched heuristically, and the root's score would come
 * back from the evaluation rather than from the tables. Unlike WDL, DTZ
 * accounts for a fifty-move counter already running, so this value is correct
 * at any clock.
 *
 * The move is matched against the generator's own list, never synthesised, so
 * nothing the generator did not produce is ever played.
 *
 * `dtz` is not used to play - the move already encodes the choice - and is
 * reported because it is the one number a differential test can compare
 * against an oracle to prove the DTZ TABLE was decoded correctly, rather than
 * merely that its sign came out right. The value derived from it saturates to
 * three outcomes and would hide a mapping or rounding bug entirely.
 */
SyzygyRoot syzygy_probe_root(Position *pos); /* mutates and restores; see above */

#endif /* SYZYGY_H */
