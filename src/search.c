/*
 * search.c - iterative deepening PVS with quiescence, run on a Lazy SMP thread pool.
 *
 * Most of this file is move ordering: alpha-beta only approaches its best-case tree size
 * when the best move comes first, and the pruning and reductions depend on that.
 */
#include "search.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bitboard.h"
#include "eval.h"
#include "movegen.h"
#include "nnue.h"
#include "syzygy.h"
#include "test/movepicktest.h"
#include "test/uncprobe.h"
#include "thread.h"
#include "timeman.h"
#include "tt.h"
#include "uci.h"

/* Shared by every thread: written before the pool is released, read-only while it runs. */
static Position RootPos;
static SearchLimits Limits;
static TimeManager Timer;

static atomic_bool Searching;
static atomic_bool StopFlag;
static atomic_bool Pondering;

/* When the clock started: at `go`, or at ponderhit for a ponder search. */
static atomic_llong ClockOrigin;

/* Max pieces for a TB probe, cached at search start. 0 when no tables are loaded, which
 * keeps bench identical across machines. */
static int TbLimit;

/* Suppresses `info` output during a synchronous (datagen) search. */
static bool Silent;

/* LMR amounts by [depth][move number]. Read-only during a search, so shared. */
static uint8_t Reductions[64][64];

typedef struct {
    Move move;
    Piece movedPiece; /* the board no longer shows it once the move is played */
    Value staticEval;

    /* The move a singular search at this ply must skip. */
    Move excludedMove;

    /* Squares the opponent attacks. The main history is keyed on this, so every read and
     * write for a move made at this ply must use this same bitboard. */
    Bitboard threats;

    /* Beta cutoffs at this ply since the grandparent was entered; the parent reduces harder
     * when its children keep cutting. Cleared two plies up. */
    int cutoffCnt;

    /* Double extensions on the line to this ply, capped so a line cannot extend forever. */
    int doubleExtensions;
} SearchStack;

#define CONT_SLOTS 3

/* Plies back each continuation-history slot looks. */
static const int ContPlies[3] = {1, 2, 4};

#define CORRHIST_SIZE       16384
#define CORRHIST_GRAIN      256
#define CORRHIST_LIMIT      (CORRHIST_GRAIN * 32)
#define CORRHIST_WEIGHT_MAX 256

typedef enum {
    PICK_TT,
    PICK_GENERATE_TACTICALS,
    PICK_GOOD_TACTICALS,
    PICK_KILLER_0,
    PICK_KILLER_1,
    PICK_COUNTER,
    PICK_GENERATE_QUIETS,
    PICK_QUIETS,
    PICK_BAD_TACTICALS,
    PICK_EVASIONS,
    PICK_DONE
} PickStage;

/* One node's staged move list. Kept in SearchThread per ply and per singular nesting, since a
 * verification search re-enters the same ply while its parent's list is still live. */
typedef struct {
    ScoredMove moves[MAX_MOVES];
    Move tt, excluded;
    Move refutations[3];
    Move returned[3];
    PickStage stage;
    int cur, end, start;
    int tacticalCount, quietCount, badBegin;
} MovePicker;

/*
 * Everything one searching thread owns. None of it may be shared (invariant 11): two
 * threads writing one history table average two searches into a table neither can trust.
 * Only the transposition table is shared, because its entries are verified on every hit.
 *
 * Heap-allocated when `Threads` is set, since the tables run to megabytes.
 */
typedef struct {
    /* Every table here must be reset by search_clear(). */
    Move killers[MAX_PLY][2];

    /* [side][from attacked][to attacked][from][to]. Escaping an attack and walking into one
     * are different moves and must not share an entry. */
    int16_t history[COLOR_NB][2][2][SQUARE_NB][SQUARE_NB];
    Move counterMoves[PIECE_NB][SQUARE_NB];

    /* [slot][previous piece][previous to][this piece][this to], keyed ContPlies back. */
    int16_t contHist[CONT_SLOTS][PIECE_NB][SQUARE_NB][PIECE_NB][SQUARE_NB];

    /* [moving piece][to][captured type]. */
    int16_t captureHist[PIECE_NB][SQUARE_NB][PIECE_TYPE_NB];

    /* Correction history, by [side to move] and one key each: the pawn structure, each
     * colour's non-pawn pieces, and the moves two and four plies up paired with the last. */
    int16_t pawnCorrHist[COLOR_NB][CORRHIST_SIZE];
    int16_t nonPawnCorrHist[COLOR_NB][COLOR_NB][CORRHIST_SIZE];
    int16_t contCorrHist[PIECE_NB][SQUARE_NB][PIECE_NB][SQUARE_NB];
    int16_t contCorrHist4[PIECE_NB][SQUARE_NB][PIECE_NB][SQUARE_NB];

    /* Triangular PV table: pvTable[ply] is the line from `ply` down. */
    Move pvTable[MAX_PLY][MAX_PLY];
    int pvLength[MAX_PLY];

    /* +2: every node clears its grandchildren's cutoff counters. */
    SearchStack stack[MAX_PLY + 2];

    /* Move lists live here rather than on the stack: a frame over a page makes Windows probe
     * the stack on every call, ~1% of the search. Scratch, so search_clear() skips them. */
    MovePicker pickers[MAX_PLY][2]; /* [ply][isExcluded] */
    ScoredMove qsMoves[MAX_PLY][MAX_MOVES];

    Position rootPos;

    /* NULL if the allocation failed: still correct, just much slower. */
    EvalState *es;

    /* Nominal depth of the current iteration; extensions are bounded relative to it. */
    Depth rootDepth;
    int selDepth;

    /* Owner-only counters, published to the atomics below every CHECK_INTERVAL nodes. */
    uint64_t nodeCount;
    uint64_t tbHits;
    atomic_ullong publishedNodes;
    atomic_ullong publishedTbHits;

    /* The last completed iteration, compared across the pool by best_thread(). */
    Move bestMove;
    Move ponderMove;
    Value rootScore;
    Depth completedDepth;

    /* Copied out when the iteration completes: an interrupted iteration overwrites pvTable,
     * and the thread whose move is played is often one that was interrupted. */
    Move rootPv[MAX_PLY];
    int rootPvLength;

    /* Nodes under each root move this search, [from * 64 + to] so it survives re-sorting.
     * Only thread 0's is read, for time management. */
    uint64_t rootEffort[SQUARE_NB * SQUARE_NB];

    /* Thread 0 owns the clock, the `info` lines and `bestmove`. */
    int id;

    ThreadHandle handle;
    bool started;

    /* Guarded by ThreadMutex. The pool is parked between searches rather than recreated. */
    bool go;
    bool exit;
    bool searching;
} SearchThread;

static SearchThread **Threads;
static int ThreadCount;

/* One mutex and condition variable for the whole pool; it is signalled once per `go`. */
static Mutex ThreadMutex;
static CondVar ThreadCv;
static bool PoolReady;

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int iclamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* Nodes between clock checks. Must be a power of two. */
#define CHECK_INTERVAL 2048

void search_limits_clear(SearchLimits *limits) {
    memset(limits, 0, sizeof(*limits));
    limits->startTime = time_ms();
}

/* round(ln(i) * 1024), with ln(0) taken as 0. Hard-coded because libm's log() can differ by
 * an ULP between platforms, which would change reductions and break bench determinism. */
/* clang-format off */
static const int LogFixed[64] = {
       0,    0,  710, 1125, 1420, 1648, 1835, 1993,
    2129, 2250, 2358, 2455, 2545, 2627, 2702, 2773,
    2839, 2901, 2960, 3015, 3068, 3118, 3165, 3211,
    3254, 3296, 3336, 3375, 3412, 3448, 3483, 3516,
    3549, 3580, 3611, 3641, 3670, 3698, 3725, 3751,
    3777, 3803, 3827, 3851, 3875, 3898, 3921, 3943,
    3964, 3985, 4006, 4026, 4046, 4066, 4085, 4104,
    4122, 4140, 4158, 4175, 4193, 4210, 4226, 4243,
};
/* clang-format on */

static void init_reductions(void);
static void thread_pool_set(int count);

void search_init(void) {
    board_cuckoo_init();
    atomic_store(&Searching, false);
    atomic_store(&StopFlag, false);
    atomic_store(&Pondering, false);
    atomic_store(&ClockOrigin, 0);
    init_reductions();
    board_set_startpos(&RootPos);

    mutex_init(&ThreadMutex);
    cond_init(&ThreadCv);
    PoolReady = true;

    /* No worker thread yet: pool_start() creates them when a search needs one. */
    thread_pool_set(1);
}

void search_clear(void) {
    tt_clear();

    for (int i = 0; i < ThreadCount; ++i) {
        SearchThread *const td = Threads[i];

        memset(td->killers, 0, sizeof(td->killers));
        memset(td->history, 0, sizeof(td->history));
        memset(td->counterMoves, 0, sizeof(td->counterMoves));
        memset(td->contHist, 0, sizeof(td->contHist));
        memset(td->captureHist, 0, sizeof(td->captureHist));
        memset(td->pawnCorrHist, 0, sizeof(td->pawnCorrHist));
        memset(td->nonPawnCorrHist, 0, sizeof(td->nonPawnCorrHist));
        memset(td->contCorrHist, 0, sizeof(td->contCorrHist));
        memset(td->contCorrHist4, 0, sizeof(td->contCorrHist4));
        memset(td->stack, 0, sizeof(td->stack));
    }

    /* Cached evaluations survive between searches but not between games. Workers drop
     * theirs at the top of thread_search(); the calling thread drops its own here. */
    eval_state_retire();
    eval_state_clear(eval_state());
}

bool search_running(void) { return atomic_load(&Searching); }
bool search_stopped(void) { return atomic_load(&StopFlag); }

/* Trails the truth by up to CHECK_INTERVAL nodes per thread, so it is for reports and
 * limits only, never a decision inside the tree. */
uint64_t search_nodes(void) {
    uint64_t total = 0;
    for (int i = 0; i < ThreadCount; ++i)
        total += atomic_load(&Threads[i]->publishedNodes);
    return total;
}

/* The same, with the caller's own live counter in place of its published one. */
static uint64_t nodes_including(const SearchThread *td) {
    uint64_t total = td->nodeCount;
    for (int i = 0; i < ThreadCount; ++i)
        if (Threads[i] != td)
            total += atomic_load(&Threads[i]->publishedNodes);
    return total;
}

static uint64_t tbhits_including(const SearchThread *td) {
    uint64_t total = td->tbHits;
    for (int i = 0; i < ThreadCount; ++i)
        if (Threads[i] != td)
            total += atomic_load(&Threads[i]->publishedTbHits);
    return total;
}

static int64_t elapsed_ms(void) { return time_ms() - atomic_load(&ClockOrigin); }

/* Node and time limits, checked mid-tree by thread 0 only; helpers just poll StopFlag.
 * Infinite and ponder searches ignore the clock because the GUI decides when they end. */
static void check_limits(SearchThread *td) {
    if (td->id != 0 || atomic_load(&StopFlag))
        return;

    if (Limits.nodes && nodes_including(td) >= Limits.nodes) {
        atomic_store(&StopFlag, true);
        return;
    }

    if (Limits.infinite || atomic_load(&Pondering))
        return;

    if (elapsed_ms() >= Timer.maximum)
        atomic_store(&StopFlag, true);
}

static inline void count_node(SearchThread *td) {
    if ((++td->nodeCount & (CHECK_INTERVAL - 1)) == 0) {
        atomic_store(&td->publishedNodes, td->nodeCount);
        atomic_store(&td->publishedTbHits, td->tbHits);
        check_limits(td);
    }
}

static inline void update_seldepth(SearchThread *td, int ply) {
    if (ply > td->selDepth)
        td->selDepth = ply;
}

/* Ordering bands, far enough apart that no score within one reaches the band above:
 * TT move > good captures and promotions > killers > counter > quiets > bad captures. */
#define SCORE_TT       (1 << 24)
#define SCORE_CAPTURE  1000000
#define SCORE_KILLER_1 900000
#define SCORE_KILLER_2 800000
#define SCORE_COUNTER  700000

#define SCORE_BAD_CAPTURE (-1000000)

/* Keeps the quiet band below SCORE_COUNTER, and fits an int16_t. */
#define HISTORY_MAX 16384

/* A constant, or a UCI spin option under `make TUNE_SEARCH=on`. */
#ifdef TUNE_SEARCH
#define TUNABLE(name, def) int name = (def)
#else
#define TUNABLE(name, def) enum { name = (def) }
#endif

/* Pruning margins are centipawns per ply of remaining depth unless noted. Thresholds stay
 * #defines: SPSA rounds both sides of a perturbation to the same integer. */
TUNABLE(RFP_MARGIN, 67);
#define RFP_DEPTH 7

#define LMP_DEPTH 8

#define FUTILITY_DEPTH 6

TUNABLE(FUTILITY_MARGIN, 60);

#define ASPIRATION_MIN_DEPTH 5

/* When a proven mate may end the search early. */
#define MATE_CONFIRM_ITERS  5
#define MATE_CONFIRM_MARGIN 8

/* Razoring: drop into quiescence when the eval is this far below alpha. */
TUNABLE(RAZOR_MARGIN, 319);
#define RAZOR_DEPTH 3

/* SEE pruning: linear in depth for captures, quadratic for quiets. */
#define SEE_CAPTURE_DEPTH 6
TUNABLE(SEE_CAPTURE_MARGIN, 87);
#define SEE_QUIET_DEPTH 8
TUNABLE(SEE_QUIET_MARGIN, 8);

/* ProbCut. The depth is a TUNABLE only as an off switch for ablations; exclude it from
 * SPSA fits. */
TUNABLE(PROBCUT_DEPTH, 5);
#define PROBCUT_REDUCTION 4
TUNABLE(PROBCUT_MARGIN, 112);

/* Quiescence delta pruning, about a minor piece. */
TUNABLE(DELTA_MARGIN, 421);

/* Singular extensions: minimum depth, and the margin in sixteenths of a cp per ply. */
#define SINGULAR_DEPTH 7
TUNABLE(SINGULAR_MARGIN, 33);

/* Double extensions: how far below the singular window, and the cap per line. */
TUNABLE(DEXT_MARGIN, 20);
TUNABLE(DEXT_MAX, 6);

/* Triple extensions, quiet table moves only: a forced capture is usually a recapture. */
TUNABLE(TEXT_MARGIN, 80);

/* Correction history weights, out of CORR_W_UNIT. Chosen, not fitted (E14); the non-pawn
 * key is half weight per colour so the pair matches the pawn table. */
#define CORR_W_UNIT 128
TUNABLE(CORR_W_PAWN, 134);
TUNABLE(CORR_W_NONPAWN, 64);
TUNABLE(CORR_W_CONT, 128);

/* Margin scaling by uncertainty, for a net without a sigma head: floor %, % per cp of
 * learned correction, and cap. The cap is SPSA-fitted (E22). */
TUNABLE(UNC_SCALE_BASE, 89);
TUNABLE(UNC_SCALE_SLOPE, 2);
TUNABLE(UNC_SCALE_MAX, 145);

/* The same for a net with a sigma head: floor %, and slope in sixteenths of a % per cp.
 * Re-centred for each net with `make unc-probe PROBE_ARGS=-ref` so it reproduces the
 * previous net's scale distribution; these are gen-6-P1's (E52). Overridable at build time
 * so a candidate net can be played without UCI options. */
#ifndef UNC_SIGMA_BASE_DEFAULT
#define UNC_SIGMA_BASE_DEFAULT 98
#endif
#ifndef UNC_SIGMA_SLOPE_DEFAULT
#define UNC_SIGMA_SLOPE_DEFAULT 5
#endif
TUNABLE(UNC_SIGMA_BASE, UNC_SIGMA_BASE_DEFAULT);
TUNABLE(UNC_SIGMA_SLOPE, UNC_SIGMA_SLOPE_DEFAULT);

/* Per-margin weight on the scale's deviation from 100%: 100 + (scale - 100) * W / UNIT.
 * Weighting the deviation, not the scale, keeps it independent of the margin itself. */
#define UNC_W_UNIT 16
TUNABLE(UNC_W_RFP, 17);
TUNABLE(UNC_W_FUTILITY, 16);
TUNABLE(UNC_W_RAZOR, 18);
TUNABLE(UNC_W_PROBCUT, 17);
TUNABLE(UNC_W_DELTA, 20);

/* Formula constants. Divisors have a minimum of 1 so a sweep cannot divide by zero, and
 * Reductions must be rebuilt when LMR_BASE or LMR_DIVISOR changes. */
TUNABLE(LMR_BASE, 13);
TUNABLE(LMR_DIVISOR, 23);

/* History's pull on a reduction; larger means less. */
TUNABLE(LMR_HIST_DIVISOR, 8493);
TUNABLE(LMR_CONT_DIVISOR, 7334);
TUNABLE(CAPHIST_DIVISOR, 5); /* capture history's weight in capture ordering */

/* Capture history's give-back on a capture's reduction: a full entry is two plies. */
TUNABLE(LMR_CAPHIST_DIVISOR, 8192);

/* Null-move reduction: base, depth divisor, and the eval-over-beta divisor and cap. */
TUNABLE(NMP_BASE, 5);
TUNABLE(NMP_DEPTH_DIVISOR, 5);
TUNABLE(NMP_EVAL_DIVISOR, 180);
TUNABLE(NMP_EVAL_MAX, 3);

/* Reduction given back at a node that was once on a PV. */
TUNABLE(TTPV_REDUCTION, 2);

/* History bonus is min(depth, cap)^2 * mul. STC never reaches the cap, so leave it out of
 * STC fits. */
TUNABLE(HIST_BONUS_MUL, 10);
TUNABLE(HIST_BONUS_DEPTH_MAX, 20);

/* The malus for moves that were tried and failed, on its own multiplier. */
TUNABLE(HIST_MALUS_MUL, 9);

/* Child cutoffs above which the next move is reduced a ply more. */
#define CUTOFF_CNT_THRESHOLD 3

/* Late move pruning: `moveCount >= base + depth * depth`. */
TUNABLE(LMP_BASE, 10);

/* History pruning: at depth 4 and below, skip a late quiet whose main plus continuation
 * history is below -HIST_PRUNE per ply. */
TUNABLE(HIST_PRUNE, 4096);

/* Half-width of the first aspiration window. */
TUNABLE(ASPIRATION_DELTA, 16);

/* Quiet ordering by piece safety: history units for escaping a lesser attacker (and the
 * same malus for walking into one), x2 for a rook, x3 for a queen. */
TUNABLE(THREAT_ORDER, 8192);

/* Extra LMR plies for a late quiet at an expected cut node. */
TUNABLE(LMR_CUTNODE, 1);

/* z-score LMR (E45). z is the child's corrected static eval against alpha in units of the
 * sigma head's predicted error, in sixteenths: (-v_child - alpha) * 16 / max(sigma, floor).
 * A late quiet with z >= ZLMR_LESS is reduced a ply less, and if it then fails high it is
 * re-searched one ply past normal depth at z >= ZDEEPER_Z, two at twice that. */
TUNABLE(ZLMR_LESS, 0);
TUNABLE(ZDEEPER_Z, 16);
TUNABLE(ZLMR_SIGMA_FLOOR, 16);

#ifdef TUNE_SEARCH

/* Name and sweep range for each TUNABLE. The ranges are deliberately wide: a sweep that
 * cannot leave the current value's neighbourhood can only confirm it. */
static const struct {
    const char *name;
    int *value;
    int min;
    int max;
} Tunables[] = {
    {"RfpMargin", &RFP_MARGIN, 20, 250},
    {"FutilityMargin", &FUTILITY_MARGIN, 10, 150},
    {"RazorMargin", &RAZOR_MARGIN, 80, 600},
    {"SeeCaptureMargin", &SEE_CAPTURE_MARGIN, 20, 300},
    {"SeeQuietMargin", &SEE_QUIET_MARGIN, 1, 120},
    {"DeltaMargin", &DELTA_MARGIN, 50, 600},
    {"ProbCutMargin", &PROBCUT_MARGIN, 30, 300},
    {"ProbCutDepth", &PROBCUT_DEPTH, 5, 99},
    {"SingularMargin", &SINGULAR_MARGIN, 4, 128},
    {"DextMargin", &DEXT_MARGIN, 0, 100},
    {"DextMax", &DEXT_MAX, 0, 16},
    {"TextMargin", &TEXT_MARGIN, 0, 400},
    {"CorrWPawn", &CORR_W_PAWN, 0, 256},
    {"CorrWNonPawn", &CORR_W_NONPAWN, 0, 256},
    {"CorrWCont", &CORR_W_CONT, 0, 256},
    {"UncScaleBase", &UNC_SCALE_BASE, 60, 120},
    {"UncScaleSlope", &UNC_SCALE_SLOPE, 0, 12},
    {"UncScaleMax", &UNC_SCALE_MAX, 100, 200},
    {"UncSigmaBase", &UNC_SIGMA_BASE, 40, 120},
    {"UncSigmaSlope", &UNC_SIGMA_SLOPE, 0, 64},
    {"UncWRfp", &UNC_W_RFP, 0, 64},
    {"UncWFutility", &UNC_W_FUTILITY, 0, 64},
    {"UncWRazor", &UNC_W_RAZOR, 0, 64},
    {"UncWProbCut", &UNC_W_PROBCUT, 0, 64},
    {"UncWDelta", &UNC_W_DELTA, 0, 64},
    {"LmrBase", &LMR_BASE, 4, 24},
    {"LmrDivisor", &LMR_DIVISOR, 12, 48},
    {"LmrHistDivisor", &LMR_HIST_DIVISOR, 2048, 32768},
    {"LmrContDivisor", &LMR_CONT_DIVISOR, 2048, 32768},
    {"CapHistDivisor", &CAPHIST_DIVISOR, 1, 32},
    {"LmrCapHistDivisor", &LMR_CAPHIST_DIVISOR, 1024, 32768},
    {"NmpBase", &NMP_BASE, 1, 6},
    {"NmpDepthDivisor", &NMP_DEPTH_DIVISOR, 1, 12},
    {"NmpEvalDivisor", &NMP_EVAL_DIVISOR, 50, 600},
    {"NmpEvalMax", &NMP_EVAL_MAX, 0, 5},
    {"HistBonusMul", &HIST_BONUS_MUL, 1, 24},
    {"HistBonusDepthMax", &HIST_BONUS_DEPTH_MAX, 4, 32},
    {"HistMalusMul", &HIST_MALUS_MUL, 1, 32},
    {"LmpBase", &LMP_BASE, 1, 24},
    {"HistPrune", &HIST_PRUNE, 512, 16384},
    {"TtPvReduction", &TTPV_REDUCTION, 0, 3},
    {"AspirationDelta", &ASPIRATION_DELTA, 4, 60},
    {"ThreatOrder", &THREAT_ORDER, 0, 32768},
    {"LmrCutNode", &LMR_CUTNODE, 0, 3},
    {"ZlmrLess", &ZLMR_LESS, -48, 48},
    {"ZdeeperZ", &ZDEEPER_Z, 0, 64},
    {"ZlmrSigmaFloor", &ZLMR_SIGMA_FLOOR, 1, 48},
};

int search_tunable_count(void) { return (int)(sizeof(Tunables) / sizeof(Tunables[0])); }

void search_tunable_info(int i, const char **name, int *value, int *min, int *max) {
    *name  = Tunables[i].name;
    *value = *Tunables[i].value;
    *min   = Tunables[i].min;
    *max   = Tunables[i].max;
}

bool search_tunable_set(const char *name, int value) {
    for (int i = 0; i < search_tunable_count(); ++i)
        if (strcmp(name, Tunables[i].name) == 0) {
            /* Clamped, not rejected: a rejected value would silently keep the old one. */
            *Tunables[i].value = iclamp(value, Tunables[i].min, Tunables[i].max);
            init_reductions();
            return true;
        }
    return false;
}
#endif

/* ln(depth) * ln(moveNumber) * LMR_BASE / LMR_DIVISOR, in exact integer arithmetic. */
static void init_reductions(void) {
    for (int d = 0; d < 64; ++d)
        for (int m = 0; m < 64; ++m)
            Reductions[d][m] = (uint8_t)((int64_t)LogFixed[d] * LogFixed[m] * LMR_BASE /
                                         (1024LL * 1024 * LMR_DIVISOR));
}

/* Captures and promotions. Castling is encoded king-takes-own-rook, so it must be excluded
 * explicitly. */
static inline bool is_tactical(const Position *pos, Move m) {
    switch (type_of_move(m)) {
    case MT_CASTLING: return false;
    case MT_EN_PASSANT:
    case MT_PROMOTION: return true;
    default: return piece_on(pos, to_sq(m)) != NO_PIECE;
    }
}

/* Static exchange evaluation: does `m` win at least `threshold` once both sides have
 * exchanged optimally on its square? Removing each attacker from `occupied` reveals the
 * sliders behind it. */
static bool see_ge(const Position *pos, Move m, Value threshold) {
    /* Castling, en passant and promotions are not modelled: they count as winning nothing. */
    if (type_of_move(m) != MT_NORMAL)
        return VALUE_ZERO >= threshold;

    const Square from = from_sq(m);
    const Square to   = to_sq(m);

    /* Balance after the first capture, from the mover's point of view. */
    int swap = PieceValues[type_of(piece_on(pos, to))] - threshold;
    if (swap < 0)
        return false;

    swap = PieceValues[type_of(piece_on(pos, from))] - swap;
    if (swap <= 0)
        return true;

    Bitboard occupied  = occupied_bb(pos) ^ square_bb(from) ^ square_bb(to);
    Bitboard attackers = board_attackers_to(pos, to, occupied);
    Color stm          = pos->sideToMove;
    int result         = 1;

    for (;;) {
        stm = (Color)(stm ^ 1);
        attackers &= occupied;

        const Bitboard mine = attackers & color_bb(pos, stm);
        if (!mine)
            break;

        result ^= 1;

        /* Least valuable attacker first. Each branch re-adds only the sliders that could have
         * been behind the piece it removes. */
        Bitboard bb;
        if ((bb = mine & pos->byType[PAWN])) {
            if ((swap = PieceValues[PAWN] - swap) < result)
                break;
            occupied ^= square_bb(lsb(bb));
            attackers |= bishop_attacks(to, occupied) & (pos->byType[BISHOP] | pos->byType[QUEEN]);
        } else if ((bb = mine & pos->byType[KNIGHT])) {
            if ((swap = PieceValues[KNIGHT] - swap) < result)
                break;
            occupied ^= square_bb(lsb(bb));
        } else if ((bb = mine & pos->byType[BISHOP])) {
            if ((swap = PieceValues[BISHOP] - swap) < result)
                break;
            occupied ^= square_bb(lsb(bb));
            attackers |= bishop_attacks(to, occupied) & (pos->byType[BISHOP] | pos->byType[QUEEN]);
        } else if ((bb = mine & pos->byType[ROOK])) {
            if ((swap = PieceValues[ROOK] - swap) < result)
                break;
            occupied ^= square_bb(lsb(bb));
            attackers |= rook_attacks(to, occupied) & (pos->byType[ROOK] | pos->byType[QUEEN]);
        } else if ((bb = mine & pos->byType[QUEEN])) {
            if ((swap = PieceValues[QUEEN] - swap) < result)
                break;
            occupied ^= square_bb(lsb(bb));
            attackers |=
                (bishop_attacks(to, occupied) & (pos->byType[BISHOP] | pos->byType[QUEEN])) |
                (rook_attacks(to, occupied) & (pos->byType[ROOK] | pos->byType[QUEEN]));
        } else {
            /* Only the king is left, and it cannot capture onto a defended square. */
            return (attackers & ~color_bb(pos, stm)) ? (result ^ 1) != 0 : result != 0;
        }
    }
    return result != 0;
}

/* Squares `c` attacks, with sliders stopped by every piece (no x-rays). */
static Bitboard attacked_by(const Position *pos, Color c) {
    const Bitboard occ   = occupied_bb(pos);
    const Bitboard pawns = pieces_bb(pos, c, PAWN);
    Bitboard att         = c == WHITE ? shift_north_east(pawns) | shift_north_west(pawns)
                                      : shift_south_east(pawns) | shift_south_west(pawns);

    for (Bitboard b = pieces_bb(pos, c, KNIGHT); b; b &= b - 1)
        att |= knight_attacks(lsb(b));
    for (Bitboard b = pieces2_bb(pos, c, BISHOP, QUEEN); b; b &= b - 1)
        att |= bishop_attacks(lsb(b), occ);
    for (Bitboard b = pieces2_bb(pos, c, ROOK, QUEEN); b; b &= b - 1)
        att |= rook_attacks(lsb(b), occ);

    return att | king_attacks(king_square(pos, c));
}

/* Squares `c` attacks with a pawn, with a minor or cheaper, with a rook or cheaper. */
typedef struct {
    Bitboard pawn, minor, rook;
} LesserThreats;

static LesserThreats lesser_threats(const Position *pos, Color c) {
    const Bitboard occ   = occupied_bb(pos);
    const Bitboard pawns = pieces_bb(pos, c, PAWN);
    LesserThreats t;

    t.pawn  = c == WHITE ? shift_north_east(pawns) | shift_north_west(pawns)
                         : shift_south_east(pawns) | shift_south_west(pawns);
    t.minor = t.pawn;
    for (Bitboard b = pieces_bb(pos, c, KNIGHT); b; b &= b - 1)
        t.minor |= knight_attacks(lsb(b));
    for (Bitboard b = pieces_bb(pos, c, BISHOP); b; b &= b - 1)
        t.minor |= bishop_attacks(lsb(b), occ);
    t.rook = t.minor;
    for (Bitboard b = pieces_bb(pos, c, ROOK); b; b &= b - 1)
        t.rook |= rook_attacks(lsb(b), occ);
    return t;
}

/* Squares where a `pt` is attacked by something cheaper. */
static inline Bitboard lesser_for(const LesserThreats *t, PieceType pt) {
    return pt == QUEEN                    ? t->rook
           : pt == ROOK                   ? t->minor
           : pt == KNIGHT || pt == BISHOP ? t->pawn
                                          : BB_EMPTY;
}

/* Every main-history access goes through here, so readers and writers bucket alike. */
static inline int16_t *main_hist(SearchThread *td, Color c, Bitboard threats, Move m) {
    const Square from = from_sq(m), to = to_sq(m);
    return &td->history[c][bb_test(threats, from)][bb_test(threats, to)][from][to];
}

/* The slice for the move `back` plies up, or NULL above the root or after a null move. */
static inline int16_t *cont_slice(SearchThread *td, int slot, int ply, int back) {
    if (ply < back)
        return NULL;

    const Move prev = td->stack[ply - back].move;
    if (!is_ok_move(prev))
        return NULL;

    return &td->contHist[slot][td->stack[ply - back].movedPiece][to_sq(prev)][0][0];
}

static inline int cont_index(Piece pc, Square to) { return (int)pc * SQUARE_NB + (int)to; }

static inline int cont_score(int16_t *const *slices, Piece pc, Square to) {
    const int idx = cont_index(pc, to);
    int total     = 0;

    for (int i = 0; i < CONT_SLOTS; ++i)
        if (slices[i])
            total += slices[i][idx];

    return total;
}

/* The captured piece type, or NO_PIECE_TYPE (castling included). */
static inline PieceType victim_of(const Position *pos, Move m) {
    switch (type_of_move(m)) {
    case MT_EN_PASSANT: return PAWN;
    case MT_CASTLING: return NO_PIECE_TYPE;
    default: return type_of(piece_on(pos, to_sq(m)));
    }
}

/* Captures by SEE band, MVV-LVA and capture history; quiets by killers, counter and history.
 * Returns the index of the best, first of equals - the move pick_move() would select. */
static int score_moves_context(SearchThread *td, const Position *pos, ScoredMove *list, int count,
                               Move ttMove, int ply, Move counter, Move killer0, Move killer1) {
    const Color us = pos->sideToMove;

    int16_t *slices[CONT_SLOTS];
    for (int i = 0; i < CONT_SLOTS; ++i)
        slices[i] = cont_slice(td, i, ply, ContPlies[i]);

    /* Built lazily: most lists scored here are quiescence captures. */
    LesserThreats lesser = {BB_EMPTY, BB_EMPTY, BB_EMPTY};
    bool lesserKnown     = false;

    for (int i = 0; i < count; ++i) {
        const Move m      = list[i].m;
        const MoveType mt = type_of_move(m);

        if (m == ttMove) {
            list[i].score = SCORE_TT;
            continue;
        }

        const Piece moved      = piece_on(pos, from_sq(m));
        const PieceType victim = victim_of(pos, m);
        int score              = 0;

        if (victim != NO_PIECE_TYPE) {
            const int mvvLva = PieceValues[victim] * 16 - PieceValues[type_of(moved)];

            /* Capture history only refines MVV-LVA. A losing capture goes below the quiets. */
            const int capHist = td->captureHist[moved][to_sq(m)][victim] / CAPHIST_DIVISOR;

            score =
                (see_ge(pos, m, VALUE_ZERO) ? SCORE_CAPTURE : SCORE_BAD_CAPTURE) + mvvLva + capHist;
        }

        if (mt == MT_PROMOTION)
            score += SCORE_CAPTURE + PieceValues[promotion_type(m)];

        /* Not `score == 0`: capture history can put a capture's score on zero. */
        if (victim == NO_PIECE_TYPE && mt != MT_PROMOTION) {
            if (m == killer0)
                score = SCORE_KILLER_1;
            else if (m == killer1)
                score = SCORE_KILLER_2;
            else if (m == counter)
                score = SCORE_COUNTER;
            else {
                score = *main_hist(td, us, td->stack[ply].threats, m) +
                        cont_score(slices, moved, to_sq(m));
                if (THREAT_ORDER) {
                    if (!lesserKnown) {
                        lesser      = lesser_threats(pos, (Color)(us ^ 1));
                        lesserKnown = true;
                    }
                    const PieceType pt = type_of(moved);
                    const Bitboard lt  = lesser_for(&lesser, pt);
                    const int scale    = pt == QUEEN ? 3 : pt == ROOK ? 2 : 1;
                    if (bb_test(lt, from_sq(m)))
                        score += bb_test(lt, to_sq(m)) ? 0 : THREAT_ORDER * scale;
                    else if (bb_test(lt, to_sq(m)))
                        score -= THREAT_ORDER * scale;
                }
            }
        }

        list[i].score = score;
    }

    int best = 0;
    for (int i = 1; i < count; ++i)
        if (list[i].score > list[best].score)
            best = i;

    return best;
}

static int score_moves(SearchThread *td, const Position *pos, ScoredMove *list, int count,
                       Move ttMove, int ply, Move counter) {
    return score_moves_context(td, pos, list, count, ttMove, ply, counter, td->killers[ply][0],
                               td->killers[ply][1]);
}

/* The counter-move registered against whatever was played to reach `ply`. */
static Move counter_move(const SearchThread *td, int ply) {
    const Move prev = td->stack[ply - 1].move;
    return is_ok_move(prev) ? td->counterMoves[td->stack[ply - 1].movedPiece][to_sq(prev)]
                            : MOVE_NONE;
}

static int history_bonus(Depth depth) {
    const Depth d = imin(depth, HIST_BONUS_DEPTH_MAX);
    return d * d * HIST_BONUS_MUL;
}

static int history_malus(Depth depth) {
    const Depth d = imin(depth, HIST_BONUS_DEPTH_MAX);
    return d * d * HIST_MALUS_MUL;
}

/* The entry decays toward zero in proportion to its size, so it tracks recent success
 * instead of saturating. */
static void history_update(int16_t *entry, int bonus) {
    const int b = bonus > HISTORY_MAX ? HISTORY_MAX : bonus < -HISTORY_MAX ? -HISTORY_MAX : bonus;
    *entry += (int16_t)(b - (int)*entry * (b < 0 ? -b : b) / HISTORY_MAX);
}

static void cont_hist_update(SearchThread *td, int ply, Piece pc, Square to, int bonus) {
    const int idx = cont_index(pc, to);

    for (int i = 0; i < CONT_SLOTS; ++i) {
        int16_t *const slice = cont_slice(td, i, ply, ContPlies[i]);
        if (slice)
            history_update(&slice[idx], bonus);
    }
}

/* Reads the victim off the board, so call it after the move is undone. */
static void capture_hist_update(SearchThread *td, const Position *pos, Move m, int bonus) {
    const Piece moved = piece_on(pos, from_sq(m));
    history_update(&td->captureHist[moved][to_sq(m)][victim_of(pos, m)], bonus);
}

/* Credits `best` in its own table and penalises everything tried before it, quiet or not. */
static void update_stats(SearchThread *td, const Position *pos, Move best, const Move *quiets,
                         int quietCount, const Move *captures, int captureCount, Depth depth,
                         int ply) {
    const Color us   = pos->sideToMove;
    const int bonus  = history_bonus(depth);
    const int malus  = history_malus(depth);
    const bool quiet = !is_tactical(pos, best);

    if (quiet) {
        if (td->killers[ply][0] != best) {
            td->killers[ply][1] = td->killers[ply][0];
            td->killers[ply][0] = best;
        }

        history_update(main_hist(td, us, td->stack[ply].threats, best), bonus);
        cont_hist_update(td, ply, piece_on(pos, from_sq(best)), to_sq(best), bonus);

        const Move prev = td->stack[ply - 1].move;
        if (is_ok_move(prev))
            td->counterMoves[td->stack[ply - 1].movedPiece][to_sq(prev)] = best;
    } else {
        capture_hist_update(td, pos, best, bonus);
    }

    for (int i = 0; i < quietCount; ++i) {
        if (quiets[i] == best)
            continue;
        history_update(main_hist(td, us, td->stack[ply].threats, quiets[i]), -malus);
        cont_hist_update(td, ply, piece_on(pos, from_sq(quiets[i])), to_sq(quiets[i]), -malus);
    }

    for (int i = 0; i < captureCount; ++i)
        if (captures[i] != best)
            capture_hist_update(td, pos, captures[i], -malus);
}

/* Whether `m` gives check, judged before it is played so the quiet prunes can exempt it.
 * The piece is moved in the occupancy and slider sets, so one test per slider type covers
 * direct and discovered checks. Only normal moves are judged; the rest report false. */
static bool gives_check(const Position *pos, Move m) {
    if (type_of_move(m) != MT_NORMAL)
        return false;

    const Color us       = pos->sideToMove;
    const Square ksq     = king_square(pos, (Color)(us ^ 1));
    const Square from    = from_sq(m);
    const Square to      = to_sq(m);
    const PieceType pt   = type_of(piece_on(pos, from));
    const Bitboard kingB = square_bb(ksq);

    if (pt == PAWN && (pawn_attacks(us, to) & kingB))
        return true;
    if (pt == KNIGHT && (knight_attacks(to) & kingB))
        return true;

    const Bitboard occ = (occupied_bb(pos) ^ square_bb(from)) | square_bb(to);
    Bitboard diag      = pieces2_bb(pos, us, BISHOP, QUEEN) & ~square_bb(from);
    Bitboard orth      = pieces2_bb(pos, us, ROOK, QUEEN) & ~square_bb(from);
    if (pt == BISHOP || pt == QUEEN)
        diag |= square_bb(to);
    if (pt == ROOK || pt == QUEEN)
        orth |= square_bb(to);

    return (bishop_attacks(ksq, occ) & diag) || (rook_attacks(ksq, occ) & orth);
}

/* Null move is only tried with this: king-and-pawn endings are full of zugzwang. */
static inline bool has_non_pawn_material(const Position *pos, Color c) {
    return (pieces2_bb(pos, c, KNIGHT, BISHOP) | pieces2_bb(pos, c, ROOK, QUEEN)) != BB_EMPTY;
}

/* Moves are selected one at a time, since most cutoffs come early. The first pick was
 * already found by score_moves(). */
static inline void pick_first(ScoredMove *list, int best) {
    if (best != 0) {
        const ScoredMove tmp = list[0];
        list[0]              = list[best];
        list[best]           = tmp;
    }
}

static void pick_move(ScoredMove *list, int count, int index) {
    /* Strictly greater, so the first of equal scores wins: generation order is part of the
     * tree. (An AVX2 scan measured 4% slower; most lists are short.) */
    int best      = index;
    int bestScore = list[index].score;
    for (int i = index + 1; i < count; ++i) {
        const int s = list[i].score;
        if (s > bestScore) {
            bestScore = s;
            best      = i;
        }
    }

    if (best != index) {
        const ScoredMove tmp = list[index];
        list[index]          = list[best];
        list[best]           = tmp;
    }
}

static void picker_init(MovePicker *mp, SearchThread *td, const Position *pos, Move tt,
                        Move excluded, int ply, Move counter) {
    mp->tt             = tt;
    mp->excluded       = excluded;
    mp->refutations[0] = td->killers[ply][0];
    mp->refutations[1] = td->killers[ply][1];
    mp->refutations[2] = counter;
    mp->returned[0] = mp->returned[1] = mp->returned[2] = MOVE_NONE;
    mp->cur = mp->end = mp->start = mp->badBegin = 0;
    mp->tacticalCount = mp->quietCount = -1;
    mp->stage                          = PICK_TT;

    /* In check, evasions are generated eagerly and the TT move is only scored among them:
     * being pseudo-legal does not make it an evasion. */
    if (board_checkers(pos)) {
        mp->stage = PICK_EVASIONS;
        mp->end   = movegen_generate(pos, GEN_EVASIONS, mp->moves);
        pick_first(mp->moves, score_moves(td, pos, mp->moves, mp->end, tt, ply, counter));
    }
}

static bool picker_duplicate(const MovePicker *mp, Move m) {
    return m == mp->tt || m == mp->excluded || m == mp->returned[0] || m == mp->returned[1] ||
           m == mp->returned[2];
}

static Move picker_next(MovePicker *mp, SearchThread *td, const Position *pos, int ply) {
    for (;;) {
        switch (mp->stage) {
        case PICK_TT:
            mp->stage = PICK_GENERATE_TACTICALS;
            /* Validated at the probe; legality is checked in the move loop. */
            if (mp->tt != MOVE_NONE && mp->tt != mp->excluded)
                return mp->tt;
            break;

        case PICK_GENERATE_TACTICALS:
            mp->tacticalCount = movegen_generate(pos, GEN_TACTICALS, mp->moves);
            mp->cur = mp->start = 0;
            mp->end             = mp->tacticalCount;
            pick_first(mp->moves, score_moves(td, pos, mp->moves, mp->end, mp->tt, ply, MOVE_NONE));
#ifndef NDEBUG
            /* The stages assume no tactical scores inside the quiet band. */
            for (int i = 0; i < mp->end; ++i)
                assert(mp->moves[i].score > SCORE_KILLER_1 ||
                       mp->moves[i].score < -HISTORY_MAX * (1 + CONT_SLOTS + 2));
#endif
            mp->stage = PICK_GOOD_TACTICALS;
            break;

        case PICK_GOOD_TACTICALS:
            while (mp->cur < mp->end) {
                if (mp->cur > mp->start)
                    pick_move(mp->moves, mp->end, mp->cur);
                if (mp->moves[mp->cur].score < SCORE_KILLER_1)
                    break;
                const Move m = mp->moves[mp->cur++].m;
                if (!picker_duplicate(mp, m))
                    return m;
            }
            /* The rest are bad captures, left in place for PICK_BAD_TACTICALS. */
            mp->badBegin = mp->cur;
            mp->stage    = PICK_KILLER_0;
            break;

        case PICK_KILLER_0:
        case PICK_KILLER_1:
        case PICK_COUNTER: {
            const int slot = mp->stage - PICK_KILLER_0;
            const Move m   = mp->refutations[slot];
            ++mp->stage;
            if (m != MOVE_NONE && !picker_duplicate(mp, m) && movegen_is_pseudo_legal(pos, m) &&
                !is_tactical(pos, m)) {
                mp->returned[slot] = m;
                return m;
            }
            break;
        }

        case PICK_GENERATE_QUIETS:
            mp->quietCount =
                movegen_generate(pos, GEN_NON_TACTICALS, mp->moves + mp->tacticalCount);
            assert(mp->tacticalCount + mp->quietCount <= MAX_MOVES);
            mp->cur = mp->start = mp->tacticalCount;
            mp->end             = mp->tacticalCount + mp->quietCount;
            /* No refutations: they were snapshotted and tried, and killers written since by a
             * singular search must not become a second refutation stage. */
            pick_first(mp->moves + mp->start,
                       score_moves_context(td, pos, mp->moves + mp->start, mp->quietCount,
                                           MOVE_NONE, ply, MOVE_NONE, MOVE_NONE, MOVE_NONE));
            mp->stage = PICK_QUIETS;
            break;

        case PICK_QUIETS:
        case PICK_BAD_TACTICALS:
        case PICK_EVASIONS:
            while (mp->cur < mp->end) {
                if (mp->cur > mp->start)
                    pick_move(mp->moves, mp->end, mp->cur);
                const Move m = mp->moves[mp->cur++].m;
                if (mp->stage == PICK_EVASIONS ? m != mp->excluded : !picker_duplicate(mp, m))
                    return m;
            }
            if (mp->stage == PICK_QUIETS) {
                mp->cur = mp->start = mp->badBegin;
                mp->end             = mp->tacticalCount;
                mp->stage           = PICK_BAD_TACTICALS;
            } else {
                mp->stage = PICK_DONE;
            }
            break;

        case PICK_DONE: return MOVE_NONE;
        }
    }
}

/* Bridges for `movepick selftest`, on a private SearchThread so the pool is untouched. */
bool search_test_picker(const Position *pos, PickerTestMoves hints, int limit,
                        PickerTestResult *out) {
    memset(out, 0, sizeof(*out));
    SearchThread *td = calloc(1, sizeof(*td));
    if (!td)
        return false;
    td->killers[1][0] = hints.killer0;
    td->killers[1][1] = hints.killer1;
    if (!movegen_is_pseudo_legal(pos, hints.tt))
        hints.tt = MOVE_NONE;
    MovePicker mp;
    picker_init(&mp, td, pos, hints.tt, hints.excluded, 1, hints.counter);
    bool ok = true;
    while (out->count < limit) {
        const Move m = picker_next(&mp, td, pos, 1);
        if (m == MOVE_NONE)
            break;
        if (out->count == MAX_MOVES) {
            ok = false;
            break;
        }
        ScoredMove *entry = &out->moves[out->count++];
        entry->m          = m;
        score_moves(td, pos, entry, 1, hints.tt, 1, hints.counter);
    }
    out->tacticalCount = mp.tacticalCount;
    out->quietCount    = mp.quietCount;
    free(td);
    return ok;
}

static uint64_t picker_perft(SearchThread *td, Position *pos, int depth) {
    if (depth == 0)
        return 1;
    MovePicker mp;
    picker_init(&mp, td, pos, MOVE_NONE, MOVE_NONE, 1, MOVE_NONE);
    uint64_t nodes = 0;
    for (Move m; (m = picker_next(&mp, td, pos, 1)) != MOVE_NONE;) {
        if (!movegen_is_legal(pos, m))
            continue;
        board_do_move(pos, m);
        nodes += picker_perft(td, pos, depth - 1);
        board_undo_move(pos, m);
    }
    return nodes;
}

uint64_t search_test_picker_perft(Position *pos, int depth) {
    SearchThread *td = calloc(1, sizeof(*td));
    if (!td || depth < 0) {
        free(td);
        return 0;
    }
    const uint64_t nodes = picker_perft(td, pos, depth);
    free(td);
    return nodes;
}

int search_test_picker_contracts(void) {
    SearchThread *td = calloc(1, sizeof(*td));
    Position *pos    = calloc(1, sizeof(*pos));
    if (!td || !pos) {
        free(td);
        free(pos);
        return 1;
    }
    int failures = 0;
    board_set_startpos(pos);
    const Move tt       = make_move(SQ_E2, SQ_E4);
    const Move k0       = make_move(SQ_D2, SQ_D4);
    const Move k1       = make_move(SQ_G1, SQ_F3);
    const Move counter  = make_move(SQ_B1, SQ_C3);
    const Move deferred = make_move(SQ_A2, SQ_A4);
    td->killers[1][0]   = k0;
    td->killers[1][1]   = k1;
    MovePicker parent, child;
    picker_init(&parent, td, pos, tt, MOVE_NONE, 1, counter);
    failures += picker_next(&parent, td, pos, 1) != tt;

    /* A nested same-ply picker (singular verification) must not disturb the parent's. */
    td->killers[1][0] = deferred;
    td->killers[1][1] = MOVE_NONE;
    picker_init(&child, td, pos, tt, tt, 1, deferred);
    int childCount = 0;
    for (Move m; (m = picker_next(&child, td, pos, 1)) != MOVE_NONE;) {
        failures += m == tt;
        if (++childCount > MAX_MOVES) {
            ++failures;
            break;
        }
    }
    failures += childCount != 19;
    failures += picker_next(&parent, td, pos, 1) != k0;
    failures += picker_next(&parent, td, pos, 1) != k1;
    failures += picker_next(&parent, td, pos, 1) != counter;
    failures += parent.quietCount != -1;
    /* History is read once, when quiet scoring is reached. */
    td->history[WHITE][0][0][SQ_A2][SQ_A4] = HISTORY_MAX;
    failures += picker_next(&parent, td, pos, 1) != deferred;
    MovePicker frozen = parent;
    for (int from = 0; from < SQUARE_NB; ++from)
        for (int to = 0; to < SQUARE_NB; ++to)
            td->history[WHITE][0][0][from][to] = (int16_t)(to * 73 - from * 31);
    for (int i = 0; i < MAX_MOVES; ++i) {
        const Move a = picker_next(&parent, td, pos, 1);
        const Move b = picker_next(&frozen, td, pos, 1);
        failures += a != b;
        if (a == MOVE_NONE)
            break;
        if (i == MAX_MOVES - 1)
            ++failures;
    }

    /* Promotions stay in the tactical band at both capture-history limits. */
    if (!board_set_fen(pos, "1r2k3/P7/8/8/8/8/8/4K3 w - - 0 1")) {
        ++failures;
    } else {
        ScoredMove list[MAX_MOVES];
        const int n = movegen_generate(pos, GEN_TACTICALS, list);
        failures += n != 8;
        for (int sign = -1; sign <= 1; sign += 2) {
            td->captureHist[W_PAWN][SQ_B8][ROOK] = (int16_t)(sign * HISTORY_MAX);
            score_moves(td, pos, list, n, MOVE_NONE, 1, MOVE_NONE);
            for (int i = 0; i < n; ++i)
                failures += list[i].score <= SCORE_KILLER_1;
        }
    }
    const int maxQuiet = HISTORY_MAX * (1 + CONT_SLOTS + 2);
    failures += maxQuiet >= SCORE_COUNTER;

    /* Each kind of ordering evidence is read at its own stage, not at picker_init(). */
    if (!board_set_fen(pos, "4k3/8/8/8/8/1p3p2/8/2N1K1N1 w - - 0 1")) {
        ++failures;
    } else {
        td->killers[1][0] = td->killers[1][1] = MOVE_NONE;
        const Move king                       = make_move(SQ_E1, SQ_E2);
        const Move capture                    = make_move(SQ_G1, SQ_F3);
        picker_init(&parent, td, pos, king, MOVE_NONE, 1, MOVE_NONE);
        failures += picker_next(&parent, td, pos, 1) != king;
        td->captureHist[W_KNIGHT][SQ_F3][PAWN] = HISTORY_MAX;
        failures += picker_next(&parent, td, pos, 1) != capture;
    }
    {
        /* Continuation history is keyed on the move that led here. */
        board_set_startpos(pos);
        memset(td->history, 0, sizeof(td->history));
        td->stack[0].move       = make_move(SQ_E7, SQ_E5);
        td->stack[0].movedPiece = B_PAWN;
        picker_init(&parent, td, pos, tt, MOVE_NONE, 1, MOVE_NONE);
        failures += picker_next(&parent, td, pos, 1) != tt;

        int16_t *const entry = &td->contHist[0][B_PAWN][SQ_E5][W_PAWN][SQ_A4];
        *entry               = HISTORY_MAX;
        failures += picker_next(&parent, td, pos, 1) != deferred;
        *entry = 0;
    }
    free(pos);
    free(td);
    return failures;
}

static void update_pv(SearchThread *td, int ply, Move m) {
    const int childLength = td->pvLength[ply + 1];

    td->pvTable[ply][0] = m;
    memcpy(&td->pvTable[ply][1], td->pvTable[ply + 1], (size_t)childLength * sizeof(Move));
    td->pvLength[ply] = childLength + 1;
}

/* Correction history: how far search results have run from the static eval, by key. */
static inline int16_t *corr_entry(SearchThread *td, const Position *pos) {
    return &td->pawnCorrHist[pos->sideToMove][pos->pawnKey & (CORRHIST_SIZE - 1)];
}

static inline int16_t *non_pawn_corr_entry(SearchThread *td, const Position *pos, Color c) {
    return &td->nonPawnCorrHist[pos->sideToMove][c][pos->nonPawnKey[c] & (CORRHIST_SIZE - 1)];
}

/* The two-move entry, or NULL where either move is missing or null. */
static inline int16_t *cont_corr_entry(SearchThread *td, int ply) {
    if (ply < 2)
        return NULL;

    const Move m1 = td->stack[ply - 1].move;
    const Move m2 = td->stack[ply - 2].move;
    if (!is_ok_move(m1) || !is_ok_move(m2))
        return NULL;

    return &td->contCorrHist[td->stack[ply - 2].movedPiece][to_sq(m2)]
                            [td->stack[ply - 1].movedPiece][to_sq(m1)];
}

static inline int16_t *cont4_corr_entry(SearchThread *td, int ply) {
    if (ply < 4)
        return NULL;

    const Move m1 = td->stack[ply - 1].move;
    const Move m4 = td->stack[ply - 4].move;
    if (!is_ok_move(m1) || !is_ok_move(m4))
        return NULL;

    return &td->contCorrHist4[td->stack[ply - 4].movedPiece][to_sq(m4)]
                             [td->stack[ply - 1].movedPiece][to_sq(m1)];
}

/* Feeds pruning, reductions and `improving` only. Reported scores are never corrected, and
 * the TT stores the raw eval so a later probe re-corrects with newer data. */
static Value corrected_eval(SearchThread *td, const Position *pos, Value raw, int ply) {
    if (raw == VALUE_NONE)
        return VALUE_NONE;

    const int16_t *const cc = cont_corr_entry(td, ply);
    const int16_t *const c4 = cont4_corr_entry(td, ply);
    const int nonPawn = *non_pawn_corr_entry(td, pos, WHITE) + *non_pawn_corr_entry(td, pos, BLACK);

    /* Clamped below TB and mate scores: a correction must not fake a proven result. */
    const int v = raw + (CORR_W_PAWN * *corr_entry(td, pos) / CORR_W_UNIT) / CORRHIST_GRAIN +
                  (CORR_W_NONPAWN * nonPawn / CORR_W_UNIT) / CORRHIST_GRAIN +
                  (cc ? (CORR_W_CONT * *cc / CORR_W_UNIT) / CORRHIST_GRAIN : 0) +
                  (c4 ? (CORR_W_CONT * *c4 / CORR_W_UNIT) / CORRHIST_GRAIN : 0);
    return (Value)iclamp(v, VALUE_TB_LOSS_IN_MAX_PLY + 1, VALUE_TB_WIN_IN_MAX_PLY - 1);
}

/* Exponential moving average, weighted by depth. */
static void corrhist_fold(int16_t *e, int diff, int weight) {
    const int updated = (*e * (CORRHIST_WEIGHT_MAX - weight) + diff * weight) / CORRHIST_WEIGHT_MAX;
    *e                = (int16_t)iclamp(updated, -CORRHIST_LIMIT, CORRHIST_LIMIT);
}

/* Every key learns the same residual, conditioned on a different part of the position. */
static void corrhist_update(SearchThread *td, const Position *pos, Value searched, Value staticEval,
                            Depth depth, int ply) {
    const int weight = imin(depth + 1, 16);
    const int diff   = (searched - staticEval) * CORRHIST_GRAIN;

    corrhist_fold(corr_entry(td, pos), diff, weight);
    corrhist_fold(non_pawn_corr_entry(td, pos, WHITE), diff, weight);
    corrhist_fold(non_pawn_corr_entry(td, pos, BLACK), diff, weight);

    int16_t *const cc = cont_corr_entry(td, ply);
    if (cc)
        corrhist_fold(cc, diff, weight);

    int16_t *const c4 = cont4_corr_entry(td, ply);
    if (c4)
        corrhist_fold(c4, diff, weight);
}

/* Clamped at zero: a negative scale would invert the margin. */
static inline int unc_apply(int scale, int weight) {
    const int scaled = 100 + (scale - 100) * weight / UNC_W_UNIT;
    return scaled < 0 ? 0 : scaled;
}

/* This node's margin scale in percent, from the net's sigma head, or else from |pawn
 * correction|. A cold correction entry lands on the floor, just under 100. */
static inline int unc_scale(SearchThread *td, const Position *pos) {
#ifdef EVAL_NNUE
    if (nnue_has_uncertainty()) {
        const int sigma = nnue_uncertainty(td->es, pos);
        const int scale = imin(UNC_SIGMA_BASE + sigma * UNC_SIGMA_SLOPE / 16, UNC_SCALE_MAX);
        return unc_probe(sigma, scale);
    }
#endif
    const int c     = *corr_entry(td, pos);
    const int ac    = (c < 0 ? -c : c) / CORRHIST_GRAIN;
    const int scale = imin(UNC_SCALE_BASE + ac * UNC_SCALE_SLOPE, UNC_SCALE_MAX);
    return unc_probe(ac, scale);
}

/* The scale costs a second inference and most nodes never reach a margin, so it is computed
 * on first use. UNC_PENDING is "not yet computed"; 100 is neutral, for nodes in check. */
#define UNC_PENDING (-1)

/* Eager under UNC_PROBE: which nodes reach a margin depends on the mapping being measured,
 * so a deferred sample would be biased by it. */
static inline int unc_start(bool neutral, SearchThread *td, const Position *pos) {
    if (neutral)
        return 100;
#ifdef UNC_PROBE
    return unc_scale(td, pos);
#else
    (void)td;
    (void)pos;
    return UNC_PENDING;
#endif
}

static inline int unc_get(int *cache, SearchThread *td, const Position *pos) {
    if (*cache == UNC_PENDING)
        *cache = unc_scale(td, pos);
    return *cache;
}

#ifdef UNC_PROBE

/* Pairs this node's signal with the search's result for it, for the probe. */
static void unc_probe_node(SearchThread *td, const Position *pos, Value searched, Value staticEval,
                           Bound bound, Depth depth) {
    if (staticEval == VALUE_NONE)
        return;

    int signal = 0;
#ifdef EVAL_NNUE
    if (nnue_has_uncertainty())
        signal = nnue_uncertainty(td->es, pos);
    else
#endif
    {
        const int c = *corr_entry(td, pos);
        signal      = (c < 0 ? -c : c) / CORRHIST_GRAIN;
    }

    unc_probe_residual(signal, searched - staticEval, bound == BOUND_EXACT,
                       is_decisive_score(searched), depth);
}

/* The constants unc_scale() is using; keep the branch in step with it. */
void search_unc_mapping(UncMapping *out) {
#ifdef EVAL_NNUE
    if (nnue_has_uncertainty()) {
        out->base  = UNC_SIGMA_BASE;
        out->slope = UNC_SIGMA_SLOPE;
        out->cap   = UNC_SCALE_MAX;
        out->grain = 16;
        out->sigma = true;
        return;
    }
#endif
    out->base  = UNC_SCALE_BASE;
    out->slope = UNC_SCALE_SLOPE;
    out->cap   = UNC_SCALE_MAX;
    out->grain = 1;
    out->sigma = false;
}
#else
static inline void unc_probe_node(SearchThread *td, const Position *pos, Value searched,
                                  Value staticEval, Bound bound, Depth depth) {
    (void)td;
    (void)pos;
    (void)searched;
    (void)staticEval;
    (void)bound;
    (void)depth;
}
#endif

/* Captures only (every evasion in check) until the position is quiet, so material is never
 * counted in the middle of an exchange. */
static Value qsearch(SearchThread *td, Position *pos, Value alpha, Value beta, int ply) {
    count_node(td);
    update_seldepth(td, ply);

    if (search_stopped())
        return VALUE_ZERO;

    if (ply >= MAX_PLY - 1)
        return corrected_eval(td, pos, eval_evaluate(td->es, pos), ply);

    const bool pvNode = beta - alpha > 1;
    const Key key     = pos->key;

    /* Stored at depth 0, so a quiescence entry never satisfies a main-search probe. */
    TTEntry tte;
    const bool ttHit    = tt_probe(key, &tte);
    const Value ttValue = ttHit ? tt_value_from_tt(tt_entry_value(&tte), ply) : VALUE_NONE;
    Move ttMove         = ttHit ? tt_entry_move(&tte) : MOVE_NONE;

    /* A 16-bit key match is not proof of identity (invariant 6). */
    if (ttMove != MOVE_NONE && !movegen_is_pseudo_legal(pos, ttMove))
        ttMove = MOVE_NONE;

    /* Preserved, never set here: seeding it from pvNode would mark ~18% of the tree as PV. */
    const bool ttPv = ttHit && tt_entry_is_pv(&tte);

    if (!pvNode && ttValue != VALUE_NONE &&
        (tt_entry_bound(&tte) & (ttValue >= beta ? BOUND_LOWER : BOUND_UPPER)))
        return ttValue;

    const bool inCheck = board_checkers(pos) != BB_EMPTY;
    Value best         = -VALUE_INFINITE;
    Value staticEval   = VALUE_NONE;
    Value rawEval      = VALUE_NONE;
    int uncScale       = 100;

    if (!inCheck) {
        /* Stand pat: capturing is optional, so the static eval is a lower bound. */
        rawEval    = ttHit && tt_entry_eval(&tte) != VALUE_NONE ? tt_entry_eval(&tte)
                                                                : eval_evaluate(td->es, pos);
        staticEval = corrected_eval(td, pos, rawEval, ply);
        uncScale   = unc_start(false, td, pos);
        best       = staticEval;

        /* A TT bound beyond the static eval is a better stand-pat. Not for proven scores, and
         * delta pruning still uses staticEval. */
        if (ttValue != VALUE_NONE && !is_decisive_score(ttValue) &&
            (tt_entry_bound(&tte) & (ttValue > best ? BOUND_LOWER : BOUND_UPPER)))
            best = ttValue;

        if (best >= beta) {
            tt_store(key, MOVE_NONE, best, rawEval, 0, BOUND_LOWER, ttPv, ply);
            return best;
        }
        if (best > alpha)
            alpha = best;
    }

    /* Quiet evasions are scored by the main history, which is keyed on threats. */
    if (inCheck)
        td->stack[ply].threats = attacked_by(pos, (Color)(pos->sideToMove ^ 1));

    ScoredMove *const moves = td->qsMoves[ply];
    const int count         = movegen_generate(pos, inCheck ? GEN_EVASIONS : GEN_CAPTURES, moves);

    pick_first(moves, score_moves(td, pos, moves, count, ttMove, ply, MOVE_NONE));

    Move bestMove = MOVE_NONE;
    int legal     = 0;

    for (int i = 0; i < count; ++i) {
        if (i > 0)
            pick_move(moves, count, i);
        const Move m = moves[i].m;

        /* Losing captures are the only negative scores and moves come out best first, so
         * the rest of the list can go. */
        if (!inCheck && moves[i].score < 0)
            break;

        /* Delta pruning: even winning the victim plus a margin does not reach alpha. Testing
         * the bare material first keeps most captures from needing the uncertainty scale. */
        if (!inCheck && type_of_move(m) != MT_PROMOTION && !is_mate_score(alpha)) {
            const Value bare = staticEval + PieceValues[victim_of(pos, m)];

            if (bare <= alpha &&
                bare + DELTA_MARGIN * unc_apply(unc_get(&uncScale, td, pos), UNC_W_DELTA) / 100 <=
                    alpha)
                continue;
        }

        if (!movegen_is_legal(pos, m))
            continue;
        ++legal;

        /* Kept current so nodes below read the move that led there, not a stale one. */
        td->stack[ply].move       = m;
        td->stack[ply].movedPiece = piece_on(pos, from_sq(m));
        td->stack[ply].staticEval = staticEval;

        board_do_move(pos, m);
        tt_prefetch(pos->key);
        eval_state_push(td->es, pos, m);
        const Value v = -qsearch(td, pos, -beta, -alpha, ply + 1);
        eval_state_pop(td->es);
        board_undo_move(pos, m);

        if (search_stopped())
            return VALUE_ZERO;

        if (v > best) {
            best     = v;
            bestMove = m;
            if (v > alpha) {
                alpha = v;
                if (v >= beta)
                    break;
            }
        }
    }

    /* Evasions are the complete move list, so no legal reply is mate. */
    if (inCheck && legal == 0)
        return mated_in(ply);

    /* Never exact: quiescence does not search every move. */
    tt_store(key, bestMove, best, rawEval, 0, best >= beta ? BOUND_LOWER : BOUND_UPPER, ttPv, ply);

    return best;
}

/* `cutNode`: the parent expects this node to fail high. A hint for reductions only. */
static Value negamax(SearchThread *td, Position *pos, Depth depth, Value alpha, Value beta, int ply,
                     bool cutNode) {
    td->pvLength[ply] = 0;

    if (depth <= 0)
        return qsearch(td, pos, alpha, beta, ply);

    count_node(td);
    update_seldepth(td, ply);

    if (search_stopped())
        return VALUE_ZERO;

    /* Read and cleared: a singular search sets it just before re-entering this ply, and no
     * later visit may inherit it. */
    const Move excluded         = td->stack[ply].excludedMove;
    td->stack[ply].excludedMove = MOVE_NONE;
    const bool isExcluded       = excluded != MOVE_NONE;

    td->stack[ply + 2].cutoffCnt = 0;

    /* Inherited on entry for children reached outside the move loop (null move, ProbCut). */
    td->stack[ply].doubleExtensions = td->stack[ply - 1].doubleExtensions;

    /* search_root() handles ply 0, so the draw and mate-distance tests need no root guard. */
    assert(ply > 0);

    if (board_is_draw(pos, ply))
        return VALUE_DRAW;

    if (ply >= MAX_PLY - 1)
        return corrected_eval(td, pos, eval_evaluate(td->es, pos), ply);

    /* Decided before mate distance pruning, which can narrow a PV node's window to null. */
    const bool pvNode = beta - alpha > 1;

    /* Mate distance pruning. */
    if (alpha < mated_in(ply))
        alpha = mated_in(ply);
    if (beta > mate_in(ply + 1))
        beta = mate_in(ply + 1);
    if (alpha >= beta)
        return alpha;

    /* Upcoming repetition: if a reversible move returns to a position on this line, the side
     * to move can claim a draw. The scan stops at the nearest null move. */
    if (alpha < VALUE_DRAW && ply >= 4) {
        int limit = ply - 1;
        for (int j = 1; j <= ply - 1; ++j)
            if (td->stack[ply - j].move == MOVE_NULL) {
                limit = j - 1;
                break;
            }
        if (board_upcoming_repetition(pos, ply, limit)) {
            alpha = VALUE_DRAW;
            if (alpha >= beta)
                return alpha;
        }
    }

    const Key key = pos->key;

    TTEntry tte;
    const bool ttHit    = tt_probe(key, &tte);
    const Value ttValue = ttHit ? tt_value_from_tt(tt_entry_value(&tte), ply) : VALUE_NONE;
    Move ttMove         = ttHit ? tt_entry_move(&tte) : MOVE_NONE;

    if (ttMove != MOVE_NONE && !movegen_is_pseudo_legal(pos, ttMove))
        ttMove = MOVE_NONE;

    /* Was this position ever on a PV? Seeded only by real PV nodes and spread only through
     * the TT, so it cannot run away. */
    const bool ttPv = pvNode || (ttHit && tt_entry_is_pv(&tte));

    /* TT cutoff. Not at PV nodes (it would truncate the PV), near the fifty-move limit, or in
     * a singular verification, whose entry was proved with the excluded move available. */
    if (!isExcluded && !pvNode && ttValue != VALUE_NONE && tt_entry_depth(&tte) >= depth &&
        pos->halfmoveClock < 90 &&
        (tt_entry_bound(&tte) & (ttValue >= beta ? BOUND_LOWER : BOUND_UPPER)))
        return ttValue;

    /* Set by a TB hit at a PV node; neutral everywhere else. */
    Value tbFloor   = -VALUE_INFINITE;
    Value tbCeiling = VALUE_INFINITE;

    /* Syzygy WDL probe, only right after a capture or pawn move: WDL assumes a fresh
     * fifty-move counter. Returned where it settles the node, and stored deeper so neighbours
     * transpose into it. At a PV node an unsettled win or loss becomes a floor or ceiling and
     * the search goes on, so it can still find the mate the tables cannot. */
    if (TbLimit != 0 && !isExcluded && pos->halfmoveClock == 0 && pos->castling == NO_CASTLING &&
        popcount(occupied_bb(pos)) <= TbLimit) {
        const Value tbValue = syzygy_probe_wdl(pos, ply);
        if (tbValue != VALUE_NONE) {
            ++td->tbHits;
            const Bound tbBound = tbValue > VALUE_DRAW   ? BOUND_LOWER
                                  : tbValue < VALUE_DRAW ? BOUND_UPPER
                                                         : BOUND_EXACT;

            if (!pvNode || tbBound == BOUND_EXACT ||
                (tbBound == BOUND_LOWER ? tbValue >= beta : tbValue <= alpha)) {
                tt_store(key, MOVE_NONE, tbValue, VALUE_NONE, (Depth)imin(depth + 6, MAX_PLY - 1),
                         tbBound, ttPv, ply);
                return tbValue;
            }

            /* Not stored: it would shadow the entry the search below writes. */
            if (tbBound == BOUND_LOWER) {
                tbFloor = tbValue;
                alpha   = (Value)imax(alpha, tbValue);
            } else {
                tbCeiling = tbValue;
            }
        }
    }

    const Color us     = pos->sideToMove;
    const bool inCheck = board_checkers(pos) != BB_EMPTY;

    /* Internal iterative reduction: without a TT move the ordering is poor, so search a ply
     * shallower and leave a TT move for the revisit. */
    if (depth >= 4 && ttMove == MOVE_NONE && !inCheck)
        --depth;

    /* Not computed in check, where nothing reads it. The TT stores rawEval. */
    const Value rawEval =
        inCheck ? VALUE_NONE
                : (ttHit && tt_entry_eval(&tte) != VALUE_NONE ? tt_entry_eval(&tte)
                                                              : eval_evaluate(td->es, pos));

    const Value staticEval = corrected_eval(td, pos, rawEval, ply);
    int uncScale           = unc_start(inCheck, td, pos);

    td->stack[ply].staticEval = staticEval;

    /* Against the grandparent, the last node with the same side to move. */
    const bool improving = !inCheck && ply >= 2 && td->stack[ply - 2].staticEval != VALUE_NONE &&
                           staticEval > td->stack[ply - 2].staticEval;

    /* Reverse futility pruning. The bare comparison comes first, before the margin's scale
     * costs an inference. Returns halfway to beta: the margin shows a fail high, not its
     * size. */
    if (!pvNode && !inCheck && depth <= RFP_DEPTH && !is_mate_score(beta) && staticEval >= beta &&
        staticEval - RFP_MARGIN * (depth - improving) *
                         unc_apply(unc_get(&uncScale, td, pos), UNC_W_RFP) / 100 >=
            beta)
        return (Value)((staticEval + beta) / 2);

    /* Razoring: far below alpha, let quiescence decide whether a tactic saves the node. */
    if (!pvNode && !inCheck && depth <= RAZOR_DEPTH && !is_mate_score(alpha) &&
        staticEval < alpha &&
        staticEval +
                RAZOR_MARGIN * depth * unc_apply(unc_get(&uncScale, td, pos), UNC_W_RAZOR) / 100 <
            alpha) {
        const Value v = qsearch(td, pos, alpha - 1, alpha, ply);
        if (v < alpha)
            return v;
    }

    /* Null-move pruning, never twice in a row. */
    if (!pvNode && !inCheck && !isExcluded && depth >= 3 && staticEval >= beta &&
        td->stack[ply - 1].move != MOVE_NULL && has_non_pawn_material(pos, us)) {
        const Depth r = NMP_BASE + depth / NMP_DEPTH_DIVISOR +
                        imin((staticEval - beta) / NMP_EVAL_DIVISOR, NMP_EVAL_MAX);

        td->stack[ply].move       = MOVE_NULL;
        td->stack[ply].movedPiece = NO_PIECE;

        board_do_null_move(pos);
        tt_prefetch(pos->key);
        eval_state_push_null(td->es, pos);
        const Value v = -negamax(td, pos, depth - r, -beta, -beta + 1, ply + 1, !cutNode);
        eval_state_pop(td->es);
        board_undo_null_move(pos);

        if (search_stopped())
            return VALUE_ZERO;

        /* A mate found by passing is not a mate. */
        if (v >= beta)
            return v >= VALUE_MATE_IN_MAX_PLY ? beta : v;
    }

    /* ProbCut: a capture that beats a raised beta in quiescence and then in a reduced search
     * fails the node high. Skipped in a singular verification (it ignores `excluded`), when
     * the TT shows the bound unreached, and when staticEval already beats it (the SEE
     * threshold would admit every capture). The margin-free gates come first so the scale is
     * only read when needed. */
    const bool probCutGate =
        !pvNode && !inCheck && !isExcluded && depth >= PROBCUT_DEPTH && !is_mate_score(beta);
    const Value probCutBeta =
        probCutGate
            ? beta + PROBCUT_MARGIN * unc_apply(unc_get(&uncScale, td, pos), UNC_W_PROBCUT) / 100
            : VALUE_NONE;

    if (probCutGate && !is_mate_score(probCutBeta) && staticEval < probCutBeta &&
        !(ttValue != VALUE_NONE && tt_entry_depth(&tte) >= depth - 3 && ttValue < probCutBeta)) {
        /* Borrows this node's picker list, unused until picker_init(). ProbCut never runs
         * under an exclusion, so the slot is [ply][0]. */
        ScoredMove *const pcMoves = td->pickers[ply][0].moves;
        const int pcCount         = movegen_generate(pos, GEN_CAPTURES, pcMoves);

        pick_first(pcMoves, score_moves(td, pos, pcMoves, pcCount, ttMove, ply, MOVE_NONE));

        for (int i = 0; i < pcCount; ++i) {
            if (i > 0)
                pick_move(pcMoves, pcCount, i);
            const Move m = pcMoves[i].m;

            if (!see_ge(pos, m, probCutBeta - staticEval))
                continue;

            if (!movegen_is_legal(pos, m))
                continue;

            td->stack[ply].move       = m;
            td->stack[ply].movedPiece = piece_on(pos, from_sq(m));

            board_do_move(pos, m);
            tt_prefetch(pos->key);
            eval_state_push(td->es, pos, m);

            Value v = -qsearch(td, pos, -probCutBeta, -probCutBeta + 1, ply + 1);
            if (v >= probCutBeta)
                v = -negamax(td, pos, depth - PROBCUT_REDUCTION, -probCutBeta, -probCutBeta + 1,
                             ply + 1, !cutNode);

            eval_state_pop(td->es);
            board_undo_move(pos, m);

            if (search_stopped())
                return VALUE_ZERO;

            if (v >= probCutBeta) {
                /* Stored at the depth actually searched. */
                tt_store(key, m, v, rawEval, depth - PROBCUT_REDUCTION + 1, BOUND_LOWER, ttPv, ply);
                return v;
            }
        }
    }

    td->stack[ply].threats = attacked_by(pos, (Color)(us ^ 1));

    MovePicker *const picker = &td->pickers[ply][isExcluded];
    picker_init(picker, td, pos, ttMove, excluded, ply, counter_move(td, ply));

    /* Moves tried so far, penalised if another move cuts. */
    Move quiets[64];
    int quietCount = 0;
    Move captures[32];
    int captureCount = 0;

    int16_t *slices[CONT_SLOTS];
    for (int i = 0; i < CONT_SLOTS; ++i)
        slices[i] = cont_slice(td, i, ply, ContPlies[i]);

    /* From the TB floor, so a proven win survives a loop in which nothing beats it. */
    Value best    = tbFloor;
    Move bestMove = MOVE_NONE;
    int moveCount = 0;

    /* Not `bestMove != MOVE_NONE`: a node that failed low still has a best move, and its
     * entry must be an upper bound, not exact. */
    bool raisedAlpha = false;

    for (Move m; (m = picker_next(picker, td, pos, ply)) != MOVE_NONE;) {
        if (!movegen_is_legal(pos, m))
            continue;
        ++moveCount;

        const bool tactical = is_tactical(pos, m);
        const Piece moved   = piece_on(pos, from_sq(m));

        const int contScore = tactical ? 0 : cont_score(slices, moved, to_sq(m));

        /* Quiet pruning, once the node has found something better than being mated. */
        if (!pvNode && !inCheck && best > VALUE_MATED_IN_MAX_PLY && !tactical) {
            /* Late move pruning, halved when not improving. Quiet checks are exempt here and
             * from futility: the ordering cannot see their value (WAC.001's mate in two is a
             * quiet check at a futile node). */
            if (depth <= LMP_DEPTH &&
                moveCount >=
                    (improving ? LMP_BASE + depth * depth : (LMP_BASE + depth * depth) / 2) &&
                !gives_check(pos, m))
                continue;

            /* Futility pruning. */
            if (depth <= FUTILITY_DEPTH && staticEval <= alpha &&
                staticEval + FUTILITY_MARGIN * depth *
                                 unc_apply(unc_get(&uncScale, td, pos), UNC_W_FUTILITY) / 100 <=
                    alpha &&
                !gives_check(pos, m))
                continue;

            /* History pruning. */
            if (depth <= 4 &&
                *main_hist(td, us, td->stack[ply].threats, m) + contScore < -HIST_PRUNE * depth &&
                !gives_check(pos, m))
                continue;
        }

        /* SEE pruning, the one prune that also applies to captures. */
        if (!pvNode && !inCheck && best > VALUE_MATED_IN_MAX_PLY) {
            const Depth seeDepth = tactical ? SEE_CAPTURE_DEPTH : SEE_QUIET_DEPTH;

            if (depth <= seeDepth) {
                const Value seeMargin =
                    tactical ? -SEE_CAPTURE_MARGIN * depth : -SEE_QUIET_MARGIN * depth * depth;

                if (!see_ge(pos, m, seeMargin))
                    continue;
            }
        }

        if (tactical && captureCount < (int)(sizeof(captures) / sizeof(captures[0]))) {
            captures[captureCount++] = m;
        }

        Depth extension = 0;

        /* Singular extension: search every other move just below the TT score. All fail low:
         * extend. One beats beta: multi-cut. Otherwise the TT move is one good move of several
         * and loses depth. The verification re-enters this ply with `excludedMove` set. */
        if (!isExcluded && depth >= SINGULAR_DEPTH && m == ttMove && ttValue != VALUE_NONE &&
            !is_mate_score(ttValue) && (tt_entry_bound(&tte) & BOUND_LOWER) &&
            tt_entry_depth(&tte) >= depth - 3) {
            const Value singularBeta  = ttValue - SINGULAR_MARGIN * depth / 16;
            const Depth singularDepth = (depth - 1) / 2;

            /* The verification writes this ply's PV entry. */
            const int savedPvLength = td->pvLength[ply];

            td->stack[ply].excludedMove = m;
            const Value v =
                negamax(td, pos, singularDepth, singularBeta - 1, singularBeta, ply, cutNode);
            td->stack[ply].excludedMove = MOVE_NONE;

            td->pvLength[ply] = savedPvLength;

            if (search_stopped())
                return VALUE_ZERO;

            /* Double and triple extensions are off the PV only, and capped per line. */
            if (v < singularBeta) {
                extension = !pvNode && v < singularBeta - DEXT_MARGIN &&
                                    td->stack[ply - 1].doubleExtensions < DEXT_MAX
                                ? 2
                                : 1;
                if (extension == 2 && !tactical && v < singularBeta - TEXT_MARGIN)
                    extension = 3;
            } else if (singularBeta >= beta && !pvNode)
                return singularBeta;
            else if (ttValue >= beta)
                extension = -2;
            else if (cutNode)
                extension = -1;
        }

        /* Read before the move is made, while the victim is still on the board. */
        const int capHistScore = tactical ? td->captureHist[moved][to_sq(m)][victim_of(pos, m)] : 0;

        td->stack[ply].move       = m;
        td->stack[ply].movedPiece = moved;

        board_do_move(pos, m);
        /* Prefetch before the accumulator update, everywhere a move is made: the update hides
         * the miss (+5.9% nps). */
        tt_prefetch(pos->key);
        eval_state_push(td->es, pos, m);

        const bool givesCheck = board_checkers(pos) != BB_EMPTY;

        /* No check extension: singular extensions find the forced checks (E46). */
        const Depth childDepth = depth - 1 + extension;

        td->stack[ply].doubleExtensions = td->stack[ply - 1].doubleExtensions + (extension >= 2);
        Value v                         = VALUE_NONE;

        /* Late move reductions. Checks and tactical moves are exempt. */
        Depth r   = 0;
        int moveZ = INT32_MIN; /* this quiet's z, where one was computed */
        if (depth >= 3 && moveCount > 2 && !tactical && !inCheck && !givesCheck) {
            r = Reductions[imin(depth, 63)][imin(moveCount, 63)];

            if (pvNode)
                --r;
            else if (ttPv)
                r -= TTPV_REDUCTION;

            if (!improving)
                ++r;

            if (td->stack[ply + 1].cutoffCnt > CUTOFF_CNT_THRESHOLD)
                ++r;

            if (cutNode)
                r += LMR_CUTNODE;

            r -= *main_hist(td, us, td->stack[ply].threats, m) / LMR_HIST_DIVISOR;
            r -= contScore / LMR_CONT_DIVISOR;

#ifdef EVAL_NNUE
            /* The child's eval is cached by the accumulator stack, so this costs a lookup, not
             * an inference. */
            if (nnue_has_uncertainty() && !is_mate_score(alpha)) {
                const Value cv = corrected_eval(td, pos, eval_evaluate(td->es, pos), ply + 1);
                const int sig  = imax(nnue_uncertainty(td->es, pos), ZLMR_SIGMA_FLOOR);
                moveZ          = (-cv - alpha) * 16 / sig;
                if (moveZ >= ZLMR_LESS)
                    --r;
            }
#endif

            if (r < 0)
                r = 0;
            else if (r > childDepth - 1)
                r = childDepth - 1;
        }
        /* Late captures: half the quiet reduction, given back by capture history. */
        else if (depth >= 3 && moveCount > 1 + pvNode && tactical &&
                 type_of_move(m) != MT_PROMOTION && !inCheck && !givesCheck) {
            r = Reductions[imin(depth, 63)][imin(moveCount, 63)] / 2;
            r -= capHistScore / LMR_CAPHIST_DIVISOR;

            if (r < 0)
                r = 0;
            else if (r > childDepth - 1)
                r = childDepth - 1;
        }

        /* PVS. A reduced child is expected to fail high, hence cutNode = true. */
        if (r > 0) {
            v = -negamax(td, pos, childDepth - r, -alpha - 1, -alpha, ply + 1, true);
            /* Re-search; deeper for a move z called promising (see ZDEEPER_Z), bounded by
             * ply < 2 * rootDepth. */
            if (v > alpha) {
                const int deeper =
                    moveZ != INT32_MIN && moveZ >= ZDEEPER_Z && ply < 2 * td->rootDepth
                        ? 1 + (moveZ >= 2 * ZDEEPER_Z)
                        : 0;
                v = -negamax(td, pos, childDepth + deeper, -alpha - 1, -alpha, ply + 1, !cutNode);
            }
        } else if (!pvNode || moveCount > 1) {
            v = -negamax(td, pos, childDepth, -alpha - 1, -alpha, ply + 1, !cutNode);
        }

        if (pvNode && (moveCount == 1 || (v > alpha && v < beta)))
            v = -negamax(td, pos, childDepth, -beta, -alpha, ply + 1, false);

        eval_state_pop(td->es);
        board_undo_move(pos, m);

        if (search_stopped())
            return VALUE_ZERO;

        /* Only moves actually searched; pruned ones were never given the chance to fail. */
        if (!tactical && quietCount < (int)(sizeof(quiets) / sizeof(quiets[0])))
            quiets[quietCount++] = m;

        if (v > best) {
            best     = v;
            bestMove = m;
            if (v > alpha) {
                alpha       = v;
                raisedAlpha = true;
                update_pv(td, ply, m);
                if (v >= beta) {
                    ++td->stack[ply].cutoffCnt;
                    update_stats(td, pos, m, quiets, quietCount, captures, captureCount, depth,
                                 ply);
                    break;
                }
            }
        }
    }

    /* Mate or stalemate - unless a move was excluded, in which case it was the only one, and
     * returning alpha lets the caller extend it. */
    if (moveCount == 0) {
        if (isExcluded)
            return alpha;

        best = inCheck ? mated_in(ply) : VALUE_DRAW;
        tt_store(key, MOVE_NONE, best, rawEval, depth, BOUND_EXACT, ttPv, ply);
        return best;
    }

    /* A proven loss caps the score, before it is stored. The ceiling is only set at PV nodes,
     * and every early return between the probe and here is !pvNode-gated. */
    if (tbCeiling != VALUE_INFINITE)
        best = (Value)imin(best, tbCeiling);

    /* Never from a singular verification: its move list was missing a move. */
    if (!isExcluded) {
        const Bound bound = best >= beta ? BOUND_LOWER : raisedAlpha ? BOUND_EXACT : BOUND_UPPER;
        tt_store(key, bestMove, best, rawEval, depth, bound, ttPv, ply);

        unc_probe_node(td, pos, best, staticEval, bound, depth);

        /* Learn the eval's error only where it means something: not in check, not from a
         * decisive score (a TB score would saturate the entry at once), not when a capture
         * explains the gap, and from a bound only in the direction it bounds. */
        if (!inCheck && !is_decisive_score(best) &&
            (bestMove == MOVE_NONE || !is_tactical(pos, bestMove)) &&
            !(bound == BOUND_LOWER && best <= staticEval) &&
            !(bound == BOUND_UPPER && best >= staticEval))
            corrhist_update(td, pos, best, staticEval, depth, ply);
    }

    return best;
}

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

static inline int root_effort_key(Move m) { return (int)from_sq(m) * SQUARE_NB + (int)to_sq(m); }

/* Stable, so ties keep generation order and the node count stays reproducible. */
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

/* One root iteration over a move list kept between iterations. Returns VALUE_NONE if
 * interrupted. */
static Value search_root(SearchThread *td, Position *pos, ScoredMove *roots, int count, Depth depth,
                         Value alpha, Value beta, Move *bestMove) {
    Value best = -VALUE_INFINITE;

    td->pvLength[0]        = 0;
    td->stack[2].cutoffCnt = 0;

    for (int i = 0; i < count; ++i) {
        const Move m = roots[i].m;

        td->stack[0].move       = m;
        td->stack[0].movedPiece = piece_on(pos, from_sq(m));
        td->stack[0].staticEval = VALUE_NONE;

        board_do_move(pos, m);
        tt_prefetch(pos->key);
        eval_state_push(td->es, pos, m);

        const uint64_t nodesBefore = td->nodeCount;

        Value v;
        if (i == 0) {
            v = -negamax(td, pos, depth - 1, -beta, -alpha, 1, false);
        } else {
            v = -negamax(td, pos, depth - 1, -alpha - 1, -alpha, 1, true);
            if (v > alpha && v < beta && !search_stopped())
                v = -negamax(td, pos, depth - 1, -beta, -alpha, 1, false);
        }

        eval_state_pop(td->es);
        board_undo_move(pos, m);

        td->rootEffort[root_effort_key(m)] += td->nodeCount - nodesBefore;

        if (search_stopped())
            return VALUE_NONE;

        roots[i].score = (int)v;

        if (v > best) {
            best      = v;
            *bestMove = m;
            update_pv(td, 0, m);

            if (v > alpha)
                alpha = v;

            /* Failed high on the aspiration window: the caller widens and retries. */
            if (v >= beta)
                break;
        }
    }

    return best;
}

static int mate_in_moves(Value v) {
    return v > 0 ? (VALUE_MATE - v + 1) / 2 : -((VALUE_MATE + v + 1) / 2);
}

/* TB wins are reported as 200 pawns minus the ply distance, Stockfish's convention, rather
 * than the raw internal value just below the mate band. */
enum { TB_REPORT_CP = 20000 };

static int tb_in_cp(Value v) {
    const int ply = VALUE_TB_WIN - (v > 0 ? v : -v);
    return v > 0 ? TB_REPORT_CP - ply : ply - TB_REPORT_CP;
}

/* Built in full and written with one fputs: stdout is unbuffered and the UCI thread may
 * print `readyok` at any moment, which would otherwise land mid-line. */
static void print_iteration(const SearchThread *td, Depth depth, Value value, int64_t elapsed,
                            bool chess960) {
    char line[256 + MAX_PLY * 6];
    char buf[8];
    size_t n = 0;

    n += (size_t)snprintf(line + n, sizeof(line) - n, "info depth %d seldepth %d ", depth,
                          td->selDepth);

    if (is_mate_score(value))
        n += (size_t)snprintf(line + n, sizeof(line) - n, "score mate %d ", mate_in_moves(value));
    else if (is_decisive_score(value))
        n += (size_t)snprintf(line + n, sizeof(line) - n, "score cp %d ", tb_in_cp(value));
    else
        n += (size_t)snprintf(line + n, sizeof(line) - n, "score cp %d ", value);

    const uint64_t nodes = nodes_including(td);
    const int64_t ms     = elapsed > 0 ? elapsed : 1;
    n += (size_t)snprintf(line + n, sizeof(line) - n, "nodes %llu nps %llu time %lld hashfull %d",
                          (unsigned long long)nodes,
                          (unsigned long long)((nodes * 1000ULL) / (uint64_t)ms),
                          (long long)elapsed, tt_hashfull());
    if (TbLimit != 0)
        n += (size_t)snprintf(line + n, sizeof(line) - n, " tbhits %llu",
                              (unsigned long long)tbhits_including(td));

    n += (size_t)snprintf(line + n, sizeof(line) - n, " pv");

    for (int i = 0; i < td->rootPvLength; ++i)
        n += (size_t)snprintf(line + n, sizeof(line) - n, " %s",
                              move_to_str(td->rootPv[i], chess960, buf));

    assert(n < sizeof(line) - 1);
    line[n++] = '\n';
    line[n]   = '\0';

    fputs(line, stdout);
    fflush(stdout);
}

/* Which iterations each helper skips, so the pool spreads over several depths instead of
 * searching in lockstep. Stockfish's old tables; the pattern repeats every 20 helpers. */
/* clang-format off */
static const int SkipSize[]  = {1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4};
static const int SkipPhase[] = {0, 1, 0, 1, 2, 3, 0, 1, 2, 3, 4, 5, 0, 1, 2, 3, 4, 5, 6, 7};
/* clang-format on */

#define SKIP_PATTERNS ((int)(sizeof(SkipSize) / sizeof(SkipSize[0])))

/* One thread's iterative deepening. Helpers run the same loop without time management and
 * with a skip pattern. */
static void thread_search(SearchThread *td) {
    Position *const pos = &td->rootPos;
    const bool isMain   = td->id == 0;

    /* Pooled threads claim this in thread_entry(); a synchronous search (datagen) or the
     * inline fallback in search_start() arrives here without one. */
    if (!td->es)
        td->es = eval_state();

    /* An accumulator keyed on the same position would otherwise be reused, even after
     * `setoption name EvalFile` swapped the net. */
    eval_state_clear(td->es);

    td->bestMove       = MOVE_NONE;
    td->ponderMove     = MOVE_NONE;
    td->rootScore      = VALUE_NONE;
    td->completedDepth = 0;
    td->rootPvLength   = 0;
    memset(td->rootEffort, 0, sizeof(td->rootEffort));

    if (isMain)
        timeman_init(&Timer, &Limits, pos->sideToMove, pos->gamePly);

    ScoredMove roots[MAX_MOVES];
    int rootCount = collect_root_moves(pos, roots);

    /* Mate, stalemate, or no legal `searchmoves`: `bestmove 0000`. */
    if (rootCount == 0)
        return;

    Value tbRootValue = VALUE_NONE;

    /* Syzygy at the root. DTZ picks a move that makes progress (WDL alone can shuffle until
     * the fifty-move rule takes the win), and the root list is cut to it so the PV and time
     * management still work. The score is reported too, since interior nodes only probe
     * right after a capture or pawn move. */
    if (TbLimit != 0 && Limits.searchmovesCount == 0) {
        const SyzygyRoot tb = syzygy_probe_root(pos);
        if (tb.value != VALUE_NONE) {
            ++td->tbHits;
            tbRootValue = tb.value;

            if (isMain && !Silent)
                printf("info string syzygy: %s at the root (dtz %d)\n",
                       tb.value > VALUE_DRAW   ? "win"
                       : tb.value < VALUE_DRAW ? "loss"
                       : tb.dtz != 0           ? "draw by the fifty-move rule"
                                               : "draw",
                       tb.dtz);
        }
        if (tb.move != MOVE_NONE && rootCount > 1) {
            for (int i = 0; i < rootCount; ++i) {
                if (roots[i].m == tb.move) {
                    roots[0]  = roots[i];
                    rootCount = 1;
                    break;
                }
            }
        }
    }

    /* A legal move from the start, in case `stop` arrives before the first iteration ends. */
    td->bestMove = roots[0].m;

    const Depth maxDepth = Limits.depth > 0 && Limits.depth < MAX_PLY ? Limits.depth : MAX_PLY - 1;

    Value prevScore = VALUE_NONE;

    /* Completed iterations in a row that agreed on the best move, for time management. */
    int stability = 0;
    Move prevBest = MOVE_NONE;

    /* Completed iterations in a row with the same mate score. */
    int mateStreak      = 0;
    Value prevMateScore = VALUE_NONE;

    /* Skips are offset by the game ply so successive searches vary each helper's schedule. */
    const int pattern = isMain ? -1 : (td->id - 1) % SKIP_PATTERNS;

    for (Depth depth = 1; depth <= maxDepth; ++depth) {
        if (pattern >= 0 &&
            ((depth + pos->gamePly + SkipPhase[pattern]) / SkipSize[pattern]) % 2 != 0)
            continue;

        td->rootDepth      = depth;
        td->selDepth       = 0;
        Move iterationBest = MOVE_NONE;

        Value alpha = -VALUE_INFINITE;
        Value beta  = VALUE_INFINITE;
        Value delta = ASPIRATION_DELTA;

        /* Aspiration windows, widened geometrically on each fail. */
        if (depth >= ASPIRATION_MIN_DEPTH && prevScore != VALUE_NONE && !is_mate_score(prevScore)) {
            alpha = prevScore - delta > -VALUE_INFINITE ? prevScore - delta : -VALUE_INFINITE;
            beta  = prevScore + delta < VALUE_INFINITE ? prevScore + delta : VALUE_INFINITE;
        }

        Value value;
        for (;;) {
            value = search_root(td, pos, roots, rootCount, depth, alpha, beta, &iterationBest);

            if (search_stopped())
                break;

            if (value <= alpha) {
                beta  = (alpha + beta) / 2;
                alpha = value - delta > -VALUE_INFINITE ? value - delta : -VALUE_INFINITE;
            } else if (value >= beta) {
                beta = value + delta < VALUE_INFINITE ? value + delta : VALUE_INFINITE;
            } else {
                break;
            }

            delta += delta / 3;
        }

        /* An interrupted iteration is discarded; the last completed one stands. */
        if (search_stopped())
            break;

        stability = (prevBest != MOVE_NONE && iterationBest == prevBest) ? stability + 1 : 0;
        prevBest  = iterationBest;

        mateStreak = (prevMateScore != VALUE_NONE && value == prevMateScore) ? mateStreak + 1 : 0;
        prevMateScore = value >= VALUE_MATE_IN_MAX_PLY ? value : VALUE_NONE;

        /* Report the root TB result unless the search found a mate, which also gives the
         * distance. prevScore keeps the tree's value to centre the next window on. */
        const Value reported =
            tbRootValue != VALUE_NONE && !is_mate_score(value) ? tbRootValue : value;

        prevScore          = value;
        td->bestMove       = iterationBest;
        td->rootScore      = reported;
        td->completedDepth = depth;

        td->rootPvLength = td->pvLength[0];
        memcpy(td->rootPv, td->pvTable[0], (size_t)td->rootPvLength * sizeof(Move));

        /* Cleared when the PV is one move long: a stale ponder move may not follow `best`. */
        td->ponderMove = td->rootPvLength > 1 ? td->rootPv[1] : MOVE_NONE;

        if (isMain && !Silent)
            print_iteration(td, depth, reported, elapsed_ms(), pos->chess960);
        sort_root_moves(roots, rootCount);

        if (!isMain)
            continue;

        if (Limits.mate && is_mate_score(value) && value > 0 && mate_in_moves(value) <= Limits.mate)
            break;

        /* Stop on a mate that has held for several iterations at a depth well past its
         * distance; the first sighting is often not the shortest. Only with a clock: fixed
         * depth, nodes and bench always do their full amount of work. */
        if (timeman_has_clock(&Timer) && !atomic_load(&Pondering) &&
            value >= VALUE_MATE_IN_MAX_PLY &&
            depth >= (Depth)(VALUE_MATE - value) + MATE_CONFIRM_MARGIN &&
            mateStreak >= MATE_CONFIRM_ITERS)
            break;

        /* Soft time limit, scaled by best-move stability and by the best move's share of
         * the nodes. */
        if (!Limits.infinite && !atomic_load(&Pondering)) {
            const uint64_t effort = td->rootEffort[root_effort_key(iterationBest)];
            const int permille    = td->nodeCount ? (int)(effort * 1000 / td->nodeCount) : 1000;

            if (elapsed_ms() >= timeman_optimum(&Timer, stability, permille))
                break;
        }
    }

    atomic_store(&td->publishedNodes, td->nodeCount);
    atomic_store(&td->publishedTbHits, td->tbHits);
}

/* The thread whose move is played: deepest completed iteration, then best score. */
static SearchThread *best_thread(void) {
    SearchThread *best = Threads[0];

    for (int i = 1; i < ThreadCount; ++i) {
        SearchThread *const td = Threads[i];

        if (td->completedDepth == 0 || td->bestMove == MOVE_NONE)
            continue;

        if (best->completedDepth == 0 || best->bestMove == MOVE_NONE ||
            td->completedDepth > best->completedDepth ||
            (td->completedDepth == best->completedDepth && td->rootScore > best->rootScore))
            best = td;
    }

    return best;
}

/* Per-search state; anything left from the previous search would leak into this one. */
static void thread_prepare(SearchThread *td) {
    td->rootPos   = RootPos;
    td->nodeCount = 0;
    td->tbHits    = 0;
    td->selDepth  = 0;

    atomic_store(&td->publishedNodes, 0);
    atomic_store(&td->publishedTbHits, 0);

    memset(td->stack, 0, sizeof(td->stack));
}

/* Thread 0, after its iterations: hold, stop the helpers, wait, and report. The order
 * matters - setting StopFlag early would also end the hold. */
static void finish_search(void) {
    /* UCI forbids `bestmove` during ponder or infinite until the GUI sends stop/ponderhit. */
    while (!atomic_load(&StopFlag) && (atomic_load(&Pondering) || Limits.infinite))
        thread_sleep_ms(1);

    atomic_store(&StopFlag, true);

    mutex_lock(&ThreadMutex);
    for (;;) {
        bool anyRunning = false;
        for (int i = 1; i < ThreadCount; ++i)
            anyRunning = anyRunning || Threads[i]->searching;

        if (!anyRunning)
            break;

        cond_wait(&ThreadCv, &ThreadMutex);
    }
    mutex_unlock(&ThreadMutex);

    const SearchThread *const best = best_thread();

    /* Re-report when a helper's result wins, so the last PV starts with the move played. */
    if (best->id != 0 && best->completedDepth > 0 && !Silent)
        print_iteration(best, best->completedDepth, best->rootScore, elapsed_ms(),
                        RootPos.chess960);

    atomic_store(&Searching, false);
    uci_print_bestmove(best->bestMove, best->ponderMove);
}

/* A pooled thread: park, search, park again, until `Threads` changes or the engine exits. */
static void thread_entry(void *arg) {
    SearchThread *const td = (SearchThread *)arg;

    thread_bind(td->id, ThreadCount);

    td->es = eval_state();
    if (!td->es)
        printf("info string thread %d: no accumulator stack; its evaluation will be slow\n",
               td->id);

    for (;;) {
        mutex_lock(&ThreadMutex);
        while (!td->go && !td->exit)
            cond_wait(&ThreadCv, &ThreadMutex);

        if (td->exit) {
            mutex_unlock(&ThreadMutex);
            break;
        }

        td->go = false;
        mutex_unlock(&ThreadMutex);

        thread_search(td);

        if (td->id == 0)
            finish_search();

        /* Cleared here but set by the starter: if a thread set it on waking, a waiter could
         * see an idle pool that has not begun (bench would read zero nodes). */
        mutex_lock(&ThreadMutex);
        td->searching = false;
        cond_broadcast(&ThreadCv);
        mutex_unlock(&ThreadMutex);
    }

    eval_state_free();
}

/* Returns once no thread is searching, and so after `bestmove` has been printed. */
static void pool_wait(void) {
    if (!PoolReady)
        return;

    mutex_lock(&ThreadMutex);
    for (;;) {
        bool anyRunning = false;
        for (int i = 0; i < ThreadCount; ++i)
            anyRunning = anyRunning || Threads[i]->searching;

        if (!anyRunning)
            break;

        cond_wait(&ThreadCv, &ThreadMutex);
    }
    mutex_unlock(&ThreadMutex);
}

static void pool_destroy(void) {
    if (ThreadCount == 0)
        return;

    mutex_lock(&ThreadMutex);
    for (int i = 0; i < ThreadCount; ++i)
        Threads[i]->exit = true;
    cond_broadcast(&ThreadCv);
    mutex_unlock(&ThreadMutex);

    for (int i = 0; i < ThreadCount; ++i) {
        if (Threads[i]->started)
            thread_join(Threads[i]->handle);
        free(Threads[i]);
    }

    free(Threads);
    Threads     = NULL;
    ThreadCount = 0;
}

/* Allocates the per-thread blocks but starts no threads. A failed allocation leaves a
 * smaller pool rather than none. */
static void thread_pool_set(int count) {
    pool_destroy();

    if (count < 1)
        count = 1;
    if (count > SEARCH_MAX_THREADS)
        count = SEARCH_MAX_THREADS;

    free(Threads);
    Threads = (SearchThread **)calloc((size_t)count, sizeof(SearchThread *));
    if (!Threads) {
        printf("info string could not allocate the thread table; searching with 1 thread\n");
        return;
    }

    for (int i = 0; i < count; ++i) {
        SearchThread *const td = (SearchThread *)calloc(1, sizeof(SearchThread));
        if (!td) {
            printf("info string could not allocate thread %d (%zu MB each); using %d\n", i,
                   sizeof(SearchThread) / (1024 * 1024), i);
            break;
        }

        td->id      = i;
        Threads[i]  = td;
        ThreadCount = i + 1;
    }
}

/* Starts any threads not yet running; returns whether thread 0 is. Kept separate from
 * allocation so a process that only searches synchronously (datagen, which forks) never
 * creates a thread. */
static bool pool_start(void) {
    for (int i = 0; i < ThreadCount; ++i) {
        SearchThread *const td = Threads[i];

        if (!td->started)
            td->started = thread_create(&td->handle, thread_entry, td);

        if (td->started)
            continue;

        /* Shrink to what started. Thread 0's block is kept regardless: inline and synchronous
         * searches run in it. */
        const int kept = i > 0 ? i : 1;
        printf("info string could not start thread %d; using %d\n", i, kept);

        for (int j = kept; j < ThreadCount; ++j) {
            free(Threads[j]);
            Threads[j] = NULL;
        }
        ThreadCount = kept;
        break;
    }

    return ThreadCount > 0 && Threads[0]->started;
}

int search_threads(void) { return ThreadCount; }

void search_set_threads(int count) {
    search_stop();
    search_wait();

    if (count == ThreadCount)
        return;

    thread_pool_set(count);

    /* Started now, not on the first move's clock. */
    pool_start();
    search_clear();
}

size_t search_thread_bytes(void) { return sizeof(SearchThread) + eval_state_bytes(); }

void search_exit(void) {
    search_stop();
    search_wait();
    pool_destroy();

    if (PoolReady) {
        mutex_destroy(&ThreadMutex);
        cond_destroy(&ThreadCv);
        PoolReady = false;
    }
}

/* Everything that must be set before any thread runs. */
static void search_setup(const Position *pos, const SearchLimits *limits, bool silent) {
    RootPos = *pos;
    Limits  = *limits;
    Silent  = silent;

    TbLimit = syzygy_max_pieces();

    atomic_store(&StopFlag, false);
    atomic_store(&ClockOrigin, limits->startTime);
    atomic_store(&Searching, true);

    /* Before any thread runs, or the helpers' first entries are stamped one search stale. */
    tt_new_search();

    for (int i = 0; i < ThreadCount; ++i)
        thread_prepare(Threads[i]);
}

void search_start(const Position *pos, const SearchLimits *limits) {
    /* Stop before waiting: a `go infinite` search would otherwise never finish. */
    search_stop();
    search_wait();

    if (ThreadCount == 0) {
        printf("info string no search thread could be allocated; cannot search\n");
        atomic_store(&Searching, false);
        uci_print_bestmove(MOVE_NONE, MOVE_NONE);
        return;
    }

    search_setup(pos, limits, false);
    atomic_store(&Pondering, limits->ponder);

    const bool released = pool_start();

    mutex_lock(&ThreadMutex);
    if (released)
        for (int i = 0; i < ThreadCount; ++i)
            if (Threads[i]->started) {
                Threads[i]->go        = true;
                Threads[i]->searching = true;
            }
    cond_broadcast(&ThreadCv);
    mutex_unlock(&ThreadMutex);

    if (!released) {
        /* No thread could start: search inline on the UCI thread, uninterruptibly. The
         * ponder/infinite hold is dropped, since only this thread could end it. */
        Limits.infinite = false;
        atomic_store(&Pondering, false);

        thread_search(Threads[0]);
        atomic_store(&StopFlag, true);
        atomic_store(&Searching, false);
        uci_print_bestmove(Threads[0]->bestMove, Threads[0]->ponderMove);
    }
}

void search_run_sync(const Position *pos, const SearchLimits *limits, SearchResult *out) {
    search_stop();
    search_wait();

    if (ThreadCount == 0) {
        printf("info string no search thread could be allocated; cannot search\n");
        out->best  = MOVE_NONE;
        out->score = VALUE_NONE;
        out->depth = 0;
        out->nodes = 0;
        return;
    }

    search_setup(pos, limits, true);

    /* Nobody could send the `stop` a synchronous ponder search would wait for. */
    atomic_store(&Pondering, false);

    /* One thread on the caller's stack, whatever `Threads` says: a parallel search is not
     * reproducible, and datagen labels must be. */
    SearchThread *const td = Threads[0];
    thread_search(td);

    Silent = false;
    atomic_store(&Searching, false);

    out->best  = td->bestMove;
    out->score = td->rootScore;
    out->depth = td->completedDepth;
    out->nodes = td->nodeCount;
}

void search_stop(void) { atomic_store(&StopFlag, true); }

void search_ponderhit(void) {
    /* A stray ponderhit during a timed search must not restart its clock. */
    if (!atomic_load(&Pondering))
        return;

    atomic_store(&ClockOrigin, time_ms());
    atomic_store(&Pondering, false);
}

void search_wait(void) { pool_wait(); }
