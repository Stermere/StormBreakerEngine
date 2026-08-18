/*
 * search.c - search driver and worker thread.
 *
 * The threading, lifecycle and stop handling here are COMPLETE and correct.
 * The actual tree search is TODO: `iterative_deepening` is where the engine
 * gets written.
 */
#include "search.h"

#include <stdatomic.h>
#include <string.h>

#include "eval.h"
#include "movegen.h"
#include "thread.h"
#include "timeman.h"
#include "tt.h"
#include "uci.h"

/* Root state, owned by the worker while a search is running. The main thread
 * only writes these before the worker starts, and only reads them after it
 * has been joined, so no lock is needed. */
static Position RootPos;
static SearchLimits Limits;
static TimeManager Timer;

static ThreadHandle Worker;
static bool WorkerStarted; /* main thread only */

/* Cross-thread flags. Relaxed ordering would be enough for a stop flag, but
 * the default sequential consistency costs nothing at the rate we poll it. */
static atomic_bool Searching;
static atomic_bool StopFlag;
static atomic_bool Pondering;

static atomic_ullong Nodes;

void search_limits_clear(SearchLimits *limits) {
    memset(limits, 0, sizeof(*limits));
    limits->startTime = time_ms();
}

void search_init(void) {
    atomic_store(&Searching, false);
    atomic_store(&StopFlag, false);
    atomic_store(&Pondering, false);
    atomic_store(&Nodes, 0);
    board_set_startpos(&RootPos);
}

void search_clear(void) {
    tt_clear();
    /* TODO(engine): also clear history heuristics, killer moves, counter-move
     * tables and any correction history once they exist. Anything that carries
     * information between searches must be reset here. */
}

bool search_running(void) { return atomic_load(&Searching); }
bool search_stopped(void) { return atomic_load(&StopFlag); }
uint64_t search_nodes(void) { return atomic_load(&Nodes); }

/*
 * TODO(engine): the search itself.
 *
 * Build it in this order, SPRT-testing every step (docs/TESTING.md):
 *   1. negamax + alpha-beta                 - the foundation
 *   2. iterative deepening + PV collection  - lets you use partial results
 *   3. quiescence search                    - removes the horizon effect;
 *                                             without it the engine hangs
 *                                             pieces at every depth
 *   4. transposition table probe/store      - see tt.h
 *   5. move ordering: TT move, captures by MVV-LVA/SEE, killers, history
 *      (ordering matters more than almost anything else: alpha-beta only
 *       reaches its theoretical node count with near-perfect ordering)
 *   6. null-move pruning, late move reductions, futility pruning, aspiration
 *      windows
 *
 * Contract this function must honour:
 *   - poll search_stopped() regularly (every ~1024 nodes) and unwind promptly
 *   - increment Nodes for every node visited: bench and nps depend on it
 *   - print `info depth ... score cp ... nodes ... nps ... pv ...` per
 *     iteration, because that is what GUIs display and what you debug from
 *   - return the best move found; the caller emits `bestmove`
 */
static Move iterative_deepening(Position *pos, Move *ponderMove) {
    *ponderMove = MOVE_NONE;

    timeman_init(&Timer, &Limits, pos->sideToMove, pos->gamePly);
    tt_new_search();

    /* No move generation yet, so there is no legal move to return. Reporting
     * MOVE_NONE makes the engine visibly "not playing" rather than illegally
     * playing something wrong - see uci_print_bestmove. */
    return MOVE_NONE;
}

static void worker_entry(void *arg) {
    (void)arg;

    atomic_store(&Nodes, 0);

    Move ponderMove = MOVE_NONE;
    Move best       = iterative_deepening(&RootPos, &ponderMove);

    /*
     * UCI forbids sending `bestmove` during a ponder or an infinite search:
     * the GUI owns that decision and will send `stop` or `ponderhit` first.
     * Replying early desynchronises the GUI and shows up as spurious losses.
     */
    while (!atomic_load(&StopFlag) && (atomic_load(&Pondering) || Limits.infinite))
        thread_sleep_ms(1);

    atomic_store(&Searching, false);
    uci_print_bestmove(best, ponderMove);
}

void search_start(const Position *pos, const SearchLimits *limits) {
    search_wait(); /* never run two searches at once */

    RootPos = *pos;
    Limits  = *limits;

    atomic_store(&StopFlag, false);
    atomic_store(&Pondering, limits->ponder);
    atomic_store(&Searching, true);

    WorkerStarted = thread_create(&Worker, worker_entry, NULL);
    if (!WorkerStarted) {
        /* Thread creation failed. Searching inline still produces a legal game
         * - it just cannot be interrupted - which beats not moving at all. */
        worker_entry(NULL);
    }
}

void search_stop(void) { atomic_store(&StopFlag, true); }

void search_ponderhit(void) {
    /* The opponent played our predicted move: the ponder search becomes a real
     * one and the clock is now ours. */
    atomic_store(&Pondering, false);

    /* TODO(engine): re-run timeman_init here. Time spent pondering was free,
     * so the allocation should be computed from this moment. */
}

void search_wait(void) {
    if (WorkerStarted) {
        thread_join(Worker);
        WorkerStarted = false;
    }
}
