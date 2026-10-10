/*
 * search.h - the search and its thread pool.
 *
 * search_start() returns at once, so the UCI thread can keep reading `stop` and
 * `ponderhit`; thread 0 prints `bestmove`. The pool is Lazy SMP sharing only the TT, and
 * its threads are parked between searches.
 */
#ifndef SEARCH_H
#define SEARCH_H

#include <stddef.h>

#include "board.h"
#include "move.h"
#include "types.h"

/* Each thread needs ~8 MB; what cannot be allocated is reported and not used. */
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

    /* Whether the field was present: a zero clock means "move now", an absent one means
     * no deadline, and the zeroed struct cannot tell them apart by value. */
    bool timeGiven;
    bool movetimeGiven;

    Move searchmoves[MAX_MOVES];
    int searchmovesCount;

    /* When `go` arrived. */
    int64_t startTime;
} SearchLimits;

/* Zeroes `limits` and stamps startTime; never reuse a previous `go`'s struct. */
void search_limits_clear(SearchLimits *limits);

void search_init(void);

void search_exit(void);

/* The `Threads` option. Ends any search and clears the history. May make fewer threads
 * than asked; search_threads() says how many. */
void search_set_threads(int count);
int search_threads(void);

/* Per-thread memory, for reporting. */
size_t search_thread_bytes(void);

/* Resets everything that carries between searches (invariant 7). */
void search_clear(void);

void search_start(const Position *pos, const SearchLimits *limits);

/* From the last completed iteration; `score` is side-to-move relative. */
typedef struct {
    Move best;
    Value score;
    Depth depth;
    uint64_t nodes;
} SearchResult;

/* Single-threaded, silent and on the calling thread, so the result is reproducible; for
 * datagen, never the UCI layer. `limits` must cap nodes or depth. */
void search_run_sync(const Position *pos, const SearchLimits *limits, SearchResult *out);

/* Safe to call when idle. */
void search_stop(void);

/* The predicted move was played: the clock now runs. */
void search_ponderhit(void);

/* Returns once the search is finished and `bestmove` printed. */
void search_wait(void);

bool search_running(void);

/* Summed over threads; trails by up to CHECK_INTERVAL nodes each. */
uint64_t search_nodes(void);

bool search_stopped(void);

/* The live constants of unc_scale(), for the probe. */
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

/* The TUNABLEs as UCI spin options, in a `make TUNE_SEARCH=on` build. */
#ifdef TUNE_SEARCH

int search_tunable_count(void);
void search_tunable_info(int i, const char **name, int *value, int *min, int *max);
bool search_tunable_set(const char *name, int value);
#endif

#endif
