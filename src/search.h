/*
 * search.h - search driver and its threading contract.
 *
 * search_start() returns at once, having handed the search to a worker thread;
 * the main thread goes straight back to stdin, which is the only way `stop` and
 * `ponderhit` can be honoured while thinking. The worker prints its own
 * `bestmove`, whether it finished or was interrupted.
 */
#ifndef SEARCH_H
#define SEARCH_H

#include "board.h"
#include "move.h"
#include "types.h"

/* Everything `go` can ask for. Times are milliseconds. */
typedef struct {
    int64_t time[COLOR_NB];
    int64_t inc[COLOR_NB];
    int movestogo;
    Depth depth;
    uint64_t nodes;
    int64_t movetime;
    int mate;
    bool infinite;
    bool ponder;

    /* `go searchmoves e2e4 d2d4` restricts the root to these. */
    Move searchmoves[MAX_MOVES];
    int searchmovesCount;

    /* Wall clock at which `go` arrived, for time management and nps. */
    int64_t startTime;
} SearchLimits;

/* Zeroes `limits` and stamps startTime. Always use it rather than a bare struct: a
 * stale `depth` or `infinite` from a previous `go` makes the engine hang. */
void search_limits_clear(SearchLimits *limits);

void search_init(void);

/* Drops everything that must not survive into a new game - transposition table,
 * history, killers. Called on `ucinewgame`; skipping it makes a game depend on
 * which games preceded it, and takes reproducibility with it. */
void search_clear(void);

void search_start(const Position *pos, const SearchLimits *limits);

/* `score` is side-to-move relative, and it and `depth` describe the last iteration
 * that ran to COMPLETION - one cut short by a limit searched its moves under
 * windows the others never saw, so its opinion is not comparable. */
typedef struct {
    Move best;
    Value score;
    Depth depth;
    uint64_t nodes;
} SearchResult;

/*
 * Runs to completion ON THE CALLING THREAD, printing nothing, for tools/datagen.c;
 * the UCI layer must never call it. It drives the same file-scope state as
 * search_start, so the two may not overlap, and `limits` must carry a node or
 * depth cap or this never returns.
 */
void search_run_sync(const Position *pos, const SearchLimits *limits, SearchResult *out);

/* Asks the worker to stop as soon as it can. Safe to call when idle. */
void search_stop(void);

/* Turns a pondering search into a normal one: the opponent played the move we were
 * pondering, so the clock is running and time limits apply. */
void search_ponderhit(void);

/* Blocks until the worker has finished and printed its bestmove. */
void search_wait(void);

bool search_running(void);
uint64_t search_nodes(void);

/* Asked to stop, or out of limits. Polled in the inner loop. */
bool search_stopped(void);

/*
 * The live constants of unc_scale()'s margin scaling, for the probe that re-centres
 * them onto a new net. They cannot be read directly: outside a TUNE_SEARCH build
 * every TUNABLE is an enum private to search.c, and which pair is live depends on
 * the loaded net.
 */
#ifdef UNC_PROBE
typedef struct {
    int base;
    int slope;
    int cap;
    int grain;
    bool sigma;
} UncMapping;

void search_unc_mapping(UncMapping *out);
#endif

/* The pruning margins as spin options, in a `make TUNE_SEARCH=on` build only: a
 * released engine has no business letting a GUI move its search margins. */
#ifdef TUNE_SEARCH

int search_tunable_count(void);
void search_tunable_info(int i, const char **name, int *value, int *min, int *max);
bool search_tunable_set(const char *name, int value);
#endif

#endif
