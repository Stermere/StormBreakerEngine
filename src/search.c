/*
 * search.c - search driver and worker thread.
 *
 * What is here is the foundation and nothing beyond it: iterative deepening,
 * negamax with alpha-beta, a quiescence search, and MVV-LVA capture ordering.
 * Ordering is not an optional extra - alpha-beta only approaches its
 * theoretical node count when the best move is tried first - so it ships with
 * the foundation rather than as a separate "improvement".
 *
 * Everything past that point (transposition table probes, killers, history,
 * null-move pruning, late move reductions, aspiration windows) is a
 * BEHAVIOURAL change and must not be committed without a passing SPRT. See
 * docs/TESTING.md; roughly half of the patches that look obviously good
 * measure neutral or worse.
 */
#include "search.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "bitboard.h"
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

/* When the clock the search is spending actually started running. Normally
 * the moment `go` arrived, but a ponder search moves it forward on ponderhit:
 * the time spent guessing was free. */
static atomic_llong ClockOrigin;

/* ------------------------------------------------------- worker-owned --- */

/* Touched only by the search thread. NodeCount is published into the atomic
 * `Nodes` periodically rather than per node, because an atomic increment in
 * the innermost loop of the search is a measurable cost for a counter nothing
 * reads at that granularity. */
static uint64_t NodeCount;

/* Triangular PV table. PvTable[ply] is the principal variation from `ply`
 * downwards; a child's line is copied up behind the move that produced it. */
static Move PvTable[MAX_PLY][MAX_PLY];
static int PvLength[MAX_PLY];

/* Nodes between clock checks. Fine enough that a search cannot overrun its
 * budget by more than a millisecond or two, coarse enough that the clock read
 * does not show up in a profile. Must be a power of two. */
#define CHECK_INTERVAL 2048

void search_limits_clear(SearchLimits *limits) {
    memset(limits, 0, sizeof(*limits));
    limits->startTime = time_ms();
}

void search_init(void) {
    atomic_store(&Searching, false);
    atomic_store(&StopFlag, false);
    atomic_store(&Pondering, false);
    atomic_store(&Nodes, 0);
    atomic_store(&ClockOrigin, 0);
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

static int64_t elapsed_ms(void) { return time_ms() - atomic_load(&ClockOrigin); }

/*
 * Enforces the limits the search cannot express structurally.
 *
 * Depth is handled by the iteration loop; node and time limits have to be
 * noticed mid-tree. An infinite or pondering search ignores the clock
 * entirely - UCI gives the GUI sole authority over when those end.
 */
static void check_limits(void) {
    if (atomic_load(&StopFlag))
        return;

    if (Limits.nodes && NodeCount >= Limits.nodes) {
        atomic_store(&StopFlag, true);
        return;
    }

    if (Limits.infinite || atomic_load(&Pondering))
        return;

    if (elapsed_ms() >= Timer.maximum)
        atomic_store(&StopFlag, true);
}

static inline void count_node(void) {
    if ((++NodeCount & (CHECK_INTERVAL - 1)) == 0) {
        atomic_store(&Nodes, NodeCount);
        check_limits();
    }
}

/* ------------------------------------------------------- move ordering -- */

/* Well clear of any capture score, so a promotion or capture is always tried
 * before a quiet move. */
#define SCORE_CAPTURE 1000000

/*
 * MVV-LVA: try capturing the most valuable victim with the least valuable
 * attacker first. It is the cheapest ordering that works, and it is what makes
 * quiescence terminate quickly instead of exploring every recapture order.
 */
static void score_moves(const Position *pos, ScoredMove *list, int count) {
    for (int i = 0; i < count; ++i) {
        const Move m      = list[i].m;
        const MoveType mt = type_of_move(m);
        int score         = 0;

        const PieceType victim = mt == MT_EN_PASSANT ? PAWN : type_of(piece_on(pos, to_sq(m)));

        if (victim != NO_PIECE_TYPE) {
            const PieceType attacker = type_of(piece_on(pos, from_sq(m)));
            score = SCORE_CAPTURE + PieceValues[victim] * 16 - PieceValues[attacker];
        }

        if (mt == MT_PROMOTION)
            score += SCORE_CAPTURE + PieceValues[promotion_type(m)];

        list[i].score = score;
    }
}

/* Selection sort, one move at a time. A beta cutoff usually lands within the
 * first few moves, so sorting the whole list up front is mostly wasted work. */
static void pick_move(ScoredMove *list, int count, int index) {
    int best = index;
    for (int i = index + 1; i < count; ++i)
        if (list[i].score > list[best].score)
            best = i;

    if (best != index) {
        const ScoredMove tmp = list[index];
        list[index]          = list[best];
        list[best]           = tmp;
    }
}

static void update_pv(int ply, Move m) {
    const int childLength = PvLength[ply + 1];

    PvTable[ply][0] = m;
    memcpy(&PvTable[ply][1], PvTable[ply + 1], (size_t)childLength * sizeof(Move));
    PvLength[ply] = childLength + 1;
}

/* ----------------------------------------------------------- quiescence -- */

/*
 * Search only the forcing moves until the position is quiet.
 *
 * Without this the engine hangs pieces at every depth: a search that stops
 * counting material in the middle of an exchange believes whatever the last
 * capture left on the board. It is not an optimisation, it is the difference
 * between an engine that plays chess and one that does not.
 */
static Value qsearch(Position *pos, Value alpha, Value beta, int ply) {
    count_node();

    if (search_stopped())
        return VALUE_ZERO;

    if (ply >= MAX_PLY - 1)
        return eval_evaluate(pos);

    const bool inCheck = board_checkers(pos) != BB_EMPTY;
    Value best         = -VALUE_INFINITE;

    if (!inCheck) {
        /* Stand pat: the side to move is never obliged to capture, so the
         * static evaluation is a lower bound on what it can achieve. In check
         * there is no such option - every reply must be searched. */
        best = eval_evaluate(pos);
        if (best >= beta)
            return best;
        if (best > alpha)
            alpha = best;
    }

    ScoredMove moves[MAX_MOVES];
    const int count = movegen_generate(pos, inCheck ? GEN_EVASIONS : GEN_CAPTURES, moves);
    score_moves(pos, moves, count);

    int legal = 0;
    for (int i = 0; i < count; ++i) {
        pick_move(moves, count, i);
        const Move m = moves[i].m;

        if (!movegen_is_legal(pos, m))
            continue;
        ++legal;

        board_do_move(pos, m);
        const Value v = -qsearch(pos, -beta, -alpha, ply + 1);
        board_undo_move(pos, m);

        if (search_stopped())
            return VALUE_ZERO;

        if (v > best) {
            best = v;
            if (v > alpha) {
                alpha = v;
                if (v >= beta)
                    break;
            }
        }
    }

    /* Evasions are the complete move list, so no legal reply here really is
     * checkmate - worth detecting, since missing it makes the engine walk into
     * mate believing it has stand-pat equality. */
    if (inCheck && legal == 0)
        return mated_in(ply);

    return best;
}

/* --------------------------------------------------------------- search -- */

static Value negamax(Position *pos, Depth depth, Value alpha, Value beta, int ply) {
    /* Cleared here rather than at each use, so that a child which never
     * extends the PV (a quiescence node) leaves a length of zero behind. */
    PvLength[ply] = 0;

    if (depth <= 0)
        return qsearch(pos, alpha, beta, ply);

    count_node();

    if (search_stopped())
        return VALUE_ZERO;

    /* The root is handled by search_root, so this is never ply 0 - which is
     * what lets the draw and mate-distance tests below run unconditionally. */
    assert(ply > 0);

    if (board_is_draw(pos, ply))
        return VALUE_DRAW;

    if (ply >= MAX_PLY - 1)
        return eval_evaluate(pos);

    /*
     * Mate distance pruning. Once a mate is known at this ply, no line can
     * beat it by mating later, and none can be worse than being mated right
     * now - so the window narrows to that range. It costs two comparisons and
     * stops the search wandering through longer mates once a short one exists.
     */
    if (alpha < mated_in(ply))
        alpha = mated_in(ply);
    if (beta > mate_in(ply + 1))
        beta = mate_in(ply + 1);
    if (alpha >= beta)
        return alpha;

    const bool inCheck = board_checkers(pos) != BB_EMPTY;

    ScoredMove moves[MAX_MOVES];
    const int count = movegen_generate(pos, inCheck ? GEN_EVASIONS : GEN_ALL, moves);
    score_moves(pos, moves, count);

    Value best = -VALUE_INFINITE;
    int legal  = 0;

    for (int i = 0; i < count; ++i) {
        pick_move(moves, count, i);
        const Move m = moves[i].m;

        if (!movegen_is_legal(pos, m))
            continue;
        ++legal;

        board_do_move(pos, m);
        const Value v = -negamax(pos, depth - 1, -beta, -alpha, ply + 1);
        board_undo_move(pos, m);

        if (search_stopped())
            return VALUE_ZERO;

        if (v > best) {
            best = v;
            if (v > alpha) {
                alpha = v;
                update_pv(ply, m);
                if (v >= beta)
                    break; /* fail high: the opponent would avoid this line */
            }
        }
    }

    /* No legal move at all: mate if the king is attacked, stalemate if not.
     * Scoring mate by ply is what makes the engine prefer the faster one. */
    if (legal == 0)
        return inCheck ? mated_in(ply) : VALUE_DRAW;

    return best;
}

/* ------------------------------------------------------------- the root -- */

/* Legal root moves, restricted to `go searchmoves ...` when the GUI asked. */
static int collect_root_moves(const Position *pos, ScoredMove *out) {
    ScoredMove all[MAX_MOVES];
    const int count = movegen_generate(pos, board_checkers(pos) ? GEN_EVASIONS : GEN_ALL, all);
    int n           = 0;

    for (int i = 0; i < count; ++i) {
        const Move m = all[i].m;

        if (!movegen_is_legal(pos, m))
            continue;

        if (Limits.searchmovesCount) {
            bool wanted = false;
            for (int j = 0; j < Limits.searchmovesCount && !wanted; ++j)
                wanted = Limits.searchmoves[j] == m;
            if (!wanted)
                continue;
        }

        out[n].m     = m;
        out[n].score = 0;
        ++n;
    }
    return n;
}

/* Insertion sort by descending score. Stable, so equal-scoring moves keep
 * their generation order and the node count stays reproducible. */
static void sort_root_moves(ScoredMove *roots, int count) {
    for (int i = 1; i < count; ++i) {
        const ScoredMove key = roots[i];
        int j                = i - 1;
        while (j >= 0 && roots[j].score < key.score) {
            roots[j + 1] = roots[j];
            --j;
        }
        roots[j + 1] = key;
    }
}

/*
 * One iteration of the root search.
 *
 * Written out rather than folded into negamax because the root is the only
 * place that has to honour `searchmoves`, keep its move list alive between
 * iterations so it can be reordered, and score every move rather than cutting
 * off. Returns VALUE_NONE if the iteration was interrupted, in which case its
 * partial result must be discarded.
 */
static Value search_root(Position *pos, ScoredMove *roots, int count, Depth depth, Move *bestMove) {
    Value alpha = -VALUE_INFINITE;

    PvLength[0] = 0;

    for (int i = 0; i < count; ++i) {
        const Move m = roots[i].m;

        board_do_move(pos, m);
        const Value v = -negamax(pos, depth - 1, -VALUE_INFINITE, -alpha, 1);
        board_undo_move(pos, m);

        if (search_stopped())
            return VALUE_NONE;

        roots[i].score = (int)v;

        if (i == 0 || v > alpha) {
            alpha     = v;
            *bestMove = m;
            update_pv(0, m);
        }
    }

    return alpha;
}

/* Plies-to-mate converted to the signed move count UCI wants. */
static int mate_in_moves(Value v) {
    return v > 0 ? (VALUE_MATE - v + 1) / 2 : -((VALUE_MATE + v + 1) / 2);
}

static void print_iteration(Depth depth, Value value, int64_t elapsed) {
    char buf[8];

    printf("info depth %d ", depth);

    if (is_mate_score(value))
        printf("score mate %d ", mate_in_moves(value));
    else
        printf("score cp %d ", value);

    /* Clamped so a sub-millisecond iteration cannot divide by zero. */
    const int64_t ms = elapsed > 0 ? elapsed : 1;
    printf("nodes %llu nps %llu time %lld pv", (unsigned long long)NodeCount,
           (unsigned long long)((NodeCount * 1000ULL) / (uint64_t)ms), (long long)elapsed);

    for (int i = 0; i < PvLength[0]; ++i)
        printf(" %s", move_to_str(PvTable[0][i], buf));

    printf("\n");
    fflush(stdout);
}

static Move iterative_deepening(Position *pos, Move *ponderMove) {
    *ponderMove = MOVE_NONE;

    timeman_init(&Timer, &Limits, pos->sideToMove, pos->gamePly);
    tt_new_search();

    ScoredMove roots[MAX_MOVES];
    const int rootCount = collect_root_moves(pos, roots);

    /* Checkmate, stalemate, or a `searchmoves` list with nothing legal in it.
     * MOVE_NONE prints as `bestmove 0000`, which is what GUIs expect. */
    if (rootCount == 0)
        return MOVE_NONE;

    Move best = roots[0].m;

    const Depth maxDepth = Limits.depth > 0 && Limits.depth < MAX_PLY ? Limits.depth : MAX_PLY - 1;

    for (Depth depth = 1; depth <= maxDepth; ++depth) {
        Move iterationBest = MOVE_NONE;
        const Value value  = search_root(pos, roots, rootCount, depth, &iterationBest);

        /* An interrupted iteration searched some moves under a window the
         * others never saw, so its ordering is meaningless. Keep the last
         * completed iteration's move instead. */
        if (search_stopped())
            break;

        best = iterationBest;
        if (PvLength[0] > 1)
            *ponderMove = PvTable[0][1];

        print_iteration(depth, value, elapsed_ms());
        sort_root_moves(roots, rootCount);

        if (Limits.mate && is_mate_score(value) && value > 0 && mate_in_moves(value) <= Limits.mate)
            break;

        /*
         * Do not begin an iteration there is no realistic chance of finishing.
         * Each one costs several times the last, so starting one at 90% of the
         * budget just burns the remainder and throws the result away.
         */
        if (!Limits.infinite && !atomic_load(&Pondering) && elapsed_ms() >= Timer.optimum)
            break;
    }

    atomic_store(&Nodes, NodeCount);
    return best;
}

/* ------------------------------------------------------------ lifecycle -- */

static void worker_entry(void *arg) {
    (void)arg;

    NodeCount = 0;
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
    atomic_store(&ClockOrigin, limits->startTime);
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
     * one and the clock is now ours. Time spent pondering was free, so the
     * allocation computed at `go` runs from this moment rather than from then.
     * Only the origin moves - the budget itself was derived from the clock the
     * GUI reported, which has not changed. */
    atomic_store(&ClockOrigin, time_ms());
    atomic_store(&Pondering, false);
}

void search_wait(void) {
    if (WorkerStarted) {
        thread_join(Worker);
        WorkerStarted = false;
    }
}
