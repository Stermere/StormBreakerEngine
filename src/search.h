/*
 * search.h - search driver and its threading contract.
 *
 * THREADING MODEL:
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

/* What a search produced, for a caller that is not a GUI. `score` is
 * side-to-move relative, the same convention eval_evaluate() uses, and both it
 * and `depth` describe the last iteration that ran to COMPLETION - an
 * iteration cut short by the node limit searched its moves under windows the
 * others never saw, so its opinion is not comparable. */
typedef struct {
    Move best;
    Value score; /* VALUE_NONE if no iteration completed */
    Depth depth;
    uint64_t nodes;
} SearchResult;

/*
 * Runs a search to completion ON THE CALLING THREAD, printing nothing.
 *
 * This exists for tools/datagen.c, which wants millions of labelled positions
 * and no `info` lines. The UCI layer must never call it: invariant 4 says the
 * protocol stays answerable while thinking, and this does not return until the
 * search is done.
 *
 * It drives the same file-scope state search_start does, so the two may not
 * overlap - one search at a time, per process. `limits` must carry a node or
 * depth cap; with neither, and `infinite` set, this never returns.
 */
void search_run_sync(const Position *pos, const SearchLimits *limits, SearchResult *out);

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

#ifdef DATAGEN
/* --------------------------------------------------- data-generation hook -- */
/*
 * Compiled in only by `make datagen`, which defines DATAGEN. The shipped
 * engine gets neither the hook nor the branch that tests it.
 *
 * docs/NNUE.md wants training positions sampled from INSIDE the search tree,
 * because the leaves of an alpha-beta tree are the distribution the evaluation
 * is actually called on and a game line is not. Nothing outside the search can
 * see those nodes, so the sampler has to be invited in - but the innermost
 * loop of the engine is not the place to pay for a feature no game ever uses,
 * hence the guard.
 *
 * The visitor runs at a node's EXIT, with every move already unmade and `pos`
 * restored to that node's position. That is the one moment where the position
 * and the move that cut off are both in hand.
 *
 * It must not mutate `pos` or touch any search state. A visitor that does
 * changes the labels, and datagen's whole contract is that a label is a
 * function of the position alone.
 */
typedef enum {
    NODE_FAIL_LOW, /* nothing reached alpha; `best` is a hint, not a label */
    NODE_EXACT,    /* a move raised alpha without failing high: the PV move */
    NODE_FAIL_HIGH /* `best` is the move that caused the beta cutoff */
} SearchNodeKind;

typedef struct {
    Depth depth; /* remaining depth; quiescence never visits, so this is >= 1 */
    int ply;
    bool inCheck;
    Move best;
    SearchNodeKind kind;
} SearchNodeInfo;

typedef void (*SearchNodeVisitor)(const Position *pos, const SearchNodeInfo *info, void *ctx);

/* Installs the visitor, or removes it when `fn` is NULL. Not thread-safe:
 * set it before the search starts. */
void search_set_node_visitor(SearchNodeVisitor fn, void *ctx);
#endif /* DATAGEN */

#ifdef TUNE_SEARCH
/*
 * The search's tunable pruning margins, exposed so uci.c can advertise them as
 * spin options. Present only in a `make TUNE_SEARCH=on` build: a released engine has
 * no business letting a GUI move its search margins, and the options would be
 * one more thing for a test client to set wrong.
 */
int search_tunable_count(void);
void search_tunable_info(int i, const char **name, int *value, int *min, int *max);
bool search_tunable_set(const char *name, int value);
#endif

#endif /* SEARCH_H */
