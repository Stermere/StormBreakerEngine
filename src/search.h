/*
 * search.h - search driver and its threading contract.
 *
 * search_start() returns at once, having handed the search to a pool of worker
 * threads; the main thread goes straight back to stdin, which is the only way
 * `stop` and `ponderhit` can be honoured while thinking. Worker 0 prints the
 * `bestmove`, whether the search finished or was interrupted.
 *
 * The pool is Lazy SMP: every thread searches the whole tree from the same root
 * and they share only the transposition table, so the parallelism is in what
 * each one is made to look at rather than in any splitting of the work. The
 * threads are created when `Threads` is set and parked between searches - at 8 MB
 * of tables apiece, starting them per `go` would cost more than the move itself.
 */
#ifndef SEARCH_H
#define SEARCH_H

#include <stddef.h>

#include "board.h"
#include "move.h"
#include "types.h"

/*
 * The ceiling on `Threads`. High enough that no real machine reaches it, and it is a
 * bound rather than a promise: each thread needs its own history tables and its own
 * accumulator stack, a little over 8 MB together, so asking for a thousand of them asks
 * for eight gigabytes. What cannot be allocated is reported and not used.
 */
#define SEARCH_MAX_THREADS 1024

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

/* Joins the pool and releases it. Only main() needs this, and only so a leak checker
 * sees a clean exit; the process would drop it all anyway. */
void search_exit(void);

/*
 * Resizes the thread pool - the `Threads` option, and nothing else may call it while a
 * search runs. Ends any search in progress, since the blocks it resizes are the ones
 * that search is using, and clears the history with them: half a pool's worth of
 * ordering statistics carried into a differently sized pool describes neither.
 *
 * Fewer threads than asked for is a possible outcome, and search_threads() is what
 * actually got made.
 */
void search_set_threads(int count);
int search_threads(void);

/* One thread's footprint, for the option handler to report before it multiplies. */
size_t search_thread_bytes(void);

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
 * the UCI layer must never call it. It drives the same root state as search_start,
 * so the two may not overlap, and `limits` must carry a node or depth cap or this
 * never returns.
 *
 * Always single-threaded, whatever `Threads` says. A caller here wants a search that
 * is a function of its position and its limits and of nothing else, and a parallel
 * search is not one: its threads reach the shared table in whatever order the
 * scheduler hands them, so the same position would label differently twice running.
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

/* Every thread's nodes, summed. Each publishes its own count periodically rather than
 * per node, so this trails by at most a couple of thousand nodes a thread - it is a
 * report and a limit, never an input to anything inside the tree. */
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
