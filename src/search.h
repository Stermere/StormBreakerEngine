/*
 * search.h - search driver and its threading contract.
 *
 * THREADING MODEL (get this right once and never revisit it):
 * `search_start` returns immediately, having handed the search to a worker
 * thread. The main thread goes straight back to reading stdin, which is the
 * only way `stop` and `ponderhit` can be honoured while thinking. An engine
 * that searches on the main thread cannot respond to `stop`, will overrun its
 * clock, and will lose games on time in every tournament it enters.
 *
 * The worker prints its own `bestmove` when it finishes, whether it stopped
 * naturally or was interrupted.
 */
#ifndef SEARCH_H
#define SEARCH_H

#include "board.h"
#include "move.h"
#include "types.h"

/* Everything the `go` command can ask for. Times are milliseconds. */
typedef struct {
    int64_t time[COLOR_NB]; /* wtime / btime */
    int64_t inc[COLOR_NB];  /* winc / binc */
    int movestogo;
    Depth depth;
    uint64_t nodes;
    int64_t movetime;
    int mate;
    bool infinite;
    bool ponder;

    /* `go searchmoves e2e4 d2d4` restricts the root to these moves. */
    Move searchmoves[MAX_MOVES];
    int searchmovesCount;

    /* Wall clock at which `go` was received, for time management and nps. */
    int64_t startTime;
} SearchLimits;

/* Zeroes `limits` and stamps startTime. Always use this rather than a bare
 * struct: a stale `depth` or `infinite` left over from a previous `go` makes
 * the engine hang or blunder. */
void search_limits_clear(SearchLimits *limits);

void search_init(void);

/* Drops all state that must not survive into a new game: transposition table,
 * history heuristics, killers. Called on `ucinewgame`. Failing to do this
 * makes games depend on which games preceded them, which quietly destroys the
 * reproducibility every SPRT result depends on. */
void search_clear(void);

/* Starts searching `pos` asynchronously and returns at once. */
void search_start(const Position *pos, const SearchLimits *limits);

/* Asks the worker to stop as soon as it can. Safe to call when idle. */
void search_stop(void);

/* Converts a pondering search into a normal one: the opponent played the move
 * we were pondering, so the clock is now running and time limits apply. */
void search_ponderhit(void);

/* Blocks until the worker has finished and printed its bestmove. */
void search_wait(void);

bool search_running(void);
uint64_t search_nodes(void);

/* True once the search has been asked to stop, or has exhausted its limits.
 * The search polls this in its inner loop. */
bool search_stopped(void);

#endif /* SEARCH_H */
