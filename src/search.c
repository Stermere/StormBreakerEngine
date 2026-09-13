/*
 * search.c - search driver and worker thread: iterative deepening over a principal
 * variation search, with a quiescence search at the leaves.
 *
 * Almost everything here exists to make alpha-beta see the best move first. That is not
 * a detail - alpha-beta only approaches its theoretical node count under good ordering,
 * and the gap between good and bad is a factor of ten in tree size. With the ordering
 * good enough to rely on, null-move pruning and late move reductions pay for themselves.
 *
 * Anything added from here is a BEHAVIOURAL change and must not be committed without a
 * passing SPRT: roughly half the patches that look obviously good measure neutral or
 * worse.
 */
#include "search.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bitboard.h"
#include "eval.h"
#include "history.h"
#include "movegen.h"
#include "nnue.h"
#include "syzygy.h"
#include "test/movepicktest.h"
#include "test/uncprobe.h"
#include "thread.h"
#include "timeman.h"
#include "tt.h"
#include "uci.h"

/* Root state, shared by every searching thread: the position they all start from and
 * the limits they all answer to. Written before the threads are released and read-only
 * while they run, so no lock is needed. */
static Position RootPos;
static SearchLimits Limits;
static TimeManager Timer;

/* Relaxed ordering would be enough for a stop flag, but sequential consistency costs
 * nothing at the rate these are polled. */
static atomic_bool Searching;
static atomic_bool StopFlag;
static atomic_bool Pondering;

/* When the clock the search is spending actually started running: normally the moment
 * `go` arrived, but a ponder search moves it forward on ponderhit, since the time spent
 * guessing was free. */
static atomic_llong ClockOrigin;

/* The piece-count gate, cached at search start so the per-node test is one comparison
 * against a local. TbLimit is 0 whenever no tablebases are loaded, which short-circuits
 * every probe and keeps bench identical across machines. */
static int TbLimit;

/* Suppresses the `info` lines, for the duration of a synchronous search only: datagen
 * runs millions of them and wants its own stdout. */
static bool Silent;

/* Late move reduction amounts by [depth][move number], built once by init_reductions().
 * Read-only once a search starts, so one copy serves every thread. */
static uint8_t Reductions[64][64];

/* What is being searched at each ply. The child needs the move that led to it - for its
 * counter-move, and to refuse a second null move in a row - and the piece that made it,
 * which the board no longer says once the move has been played. */
typedef struct {
    Move move;
    Piece movedPiece;
    Value staticEval;

    /* Set only while a singular search runs at this ply: the move that search must
     * pretend does not exist. */
    Move excludedMove;
} SearchStack;

#define CONT_SLOTS 3

/* How far back each slot looks. Sized independently of CONT_SLOTS so lowering the slot
 * count is a one-flag change; only the first CONT_SLOTS entries are read. */
static const int ContPlies[3] = {1, 2, 4};

#define CORRHIST_SIZE       16384
#define CORRHIST_GRAIN      256
#define CORRHIST_LIMIT      (CORRHIST_GRAIN * 32)
#define CORRHIST_WEIGHT_MAX 256

#ifdef MOVE_PICKER_PROFILE
typedef struct {
    uint64_t mainNodes, mainPicked, mainLegal, mainSearched;
    uint64_t genCalls[5], generated[5], scored[5];
    uint64_t orderingSee, pruningSee, cutoffs[7];
} MoveProfile;
#define MP_ADD(td, field, amount) ((td)->moveProfile.field += (uint64_t)(amount))
#else
#define MP_ADD(td, field, amount) ((void)0)
#endif

/*
 * Everything one searching thread owns.
 *
 * Lazy SMP is N threads searching the SAME tree with no work splitting. They diverge
 * because they reach the shared transposition table in different orders and because the
 * helpers skip iterations on their own schedule, and the entire gain is that a line one
 * of them refutes cheaply is a line none of the others has to search again.
 *
 * Nothing below may be shared, and that is a stronger statement than "it would race".
 * Two threads writing one history entry are two different searches averaging their
 * opinions into a table neither can then trust, and the ordering that results is worse
 * than either thread's alone. The transposition table is shared precisely because it is
 * the one structure whose entries are self-describing enough to survive being written
 * by somebody else: every hit is verified against the position and every move that
 * comes back out of it is validated before it is played.
 *
 * The tables dominate the size: 6 MB of continuation history and 1 MB of pawn history,
 * with the accumulator stack in nnue.c beside them. Hence heap blocks claimed when
 * `Threads` is set, rather than anything a `go` has to allocate.
 */
typedef struct {
    /*
     * The ordering heuristics, declared here rather than beside their use because
     * search_clear() has to reset every one - anything carrying information from one
     * search into the next makes a result depend on what was searched before it.
     *
     * Killers are quiet moves that cut at this ply elsewhere in the tree, since siblings
     * tend to share refutations; history is how well a quiet move has been doing lately,
     * untied to a ply; counterMoves is the quiet reply that most recently refuted this
     * exact move.
     */
    Move killers[MAX_PLY][2];
    int16_t history[COLOR_NB][SQUARE_NB][SQUARE_NB];
    PawnHistory pawnHistory;
    Move counterMoves[PIECE_NB][SQUARE_NB];

    /*
     * Continuation history, [slot][previous piece][previous to][this piece][this to], 2 MB
     * a slot. Where counterMoves remembers a single best reply, this scores EVERY reply
     * against the same context, which is what lets ordering understand plans rather than
     * one-move refutations.
     *
     * Three slots, keyed one, two and four plies back: the direct reply, the same side's
     * own previous move (so "knight to d2 then f1" scores as a unit), and the slower
     * manoeuvres a two-ply window reads as noise.
     */
    int16_t contHist[CONT_SLOTS][PIECE_NB][SQUARE_NB][PIECE_NB][SQUARE_NB];

    /* Capture history, [moving piece][to][captured type]. MVV-LVA says what a capture
     * takes and SEE what it wins, and neither can separate two equal-looking exchanges;
     * recent success can, which in a sharp middlegame is most of them. */
    int16_t captureHist[PIECE_NB][SQUARE_NB][PIECE_TYPE_NB];

    /* Correction history, [side to move][pawn key]. Three further keys were tried - minor
     * pieces, non-pawn material, and the move that led to the node - at two weightings,
     * and both measured slightly negative over 2076 games (E16). */
    int16_t pawnCorrHist[COLOR_NB][CORRHIST_SIZE];

    /* Triangular PV table: pvTable[ply] is the principal variation from `ply` downwards,
     * and a child's line is copied up behind the move that produced it. */
    Move pvTable[MAX_PLY][MAX_PLY];
    int pvLength[MAX_PLY];

    SearchStack stack[MAX_PLY];

    /* This thread's own board. Every thread starts from the same position and then plays
     * its own moves on it, so the copy is not an optimisation. */
    Position rootPos;

    /* This thread's accumulator stack and refresh cache, claimed once in thread_entry().
     * Held here rather than looked up per node - see EvalState in eval.h. NULL is legal
     * and means the allocation failed: correct, and several times slower. */
    EvalState *es;

    /* Nominal depth of the iteration currently running. Extensions are bounded relative
     * to it, so a line that can be extended indefinitely cannot grow the tree without
     * limit. */
    Depth rootDepth;

    /* Deepest ply any line reached this iteration, quiescence included. It is how a GUI
     * tells a search genuinely looking deep along forcing lines from one reporting a big
     * nominal depth after heavy reductions. */
    int selDepth;

    /* Touched only by the owning thread, and published into the atomics below
     * periodically rather than per node: an atomic increment in the innermost loop is a
     * measurable cost for a counter nothing reads at that granularity. */
    uint64_t nodeCount;
    uint64_t tbHits;

#ifdef MOVE_PICKER_PROFILE
    MoveProfile moveProfile;
#endif

    /* What any other thread is allowed to read, so `info nodes`, `info tbhits` and a node
     * limit can see the whole search rather than one thread's share of it. */
    atomic_ullong publishedNodes;
    atomic_ullong publishedTbHits;

    /* The last completed iteration's result. best_thread() compares these across the
     * pool, and the winner's move is the one played. */
    Move bestMove;
    Move ponderMove;
    Value rootScore;
    Depth completedDepth;

    /* The line that produced it, copied out of pvTable when the iteration completed.
     * The copy is what makes it reportable: pvTable is live storage, so an iteration
     * that was interrupted has already half-overwritten the line the finished one
     * left there - and the thread whose move gets played is very often a thread that
     * was interrupted. */
    Move rootPv[MAX_PLY];
    int rootPvLength;

    /* Thread 0 owns the clock, the `info` lines and the `bestmove`; every other thread
     * exists to disturb the shared table in a useful direction and nothing else. */
    int id;

    ThreadHandle handle;
    bool started;

    /* Guarded by ThreadMutex, and the reason it exists: the pool is parked between
     * searches rather than created per `go`, because starting 128 threads and faulting in
     * a gigabyte of fresh history tables costs more than a whole move at blitz. */
    bool go;
    bool exit;
    bool searching;
} SearchThread;

static SearchThread **Threads;
static int ThreadCount;

/* One mutex and one condition variable for the whole pool. The events are a search
 * starting and a thread finishing one - once per `go`, never per node - so waking every
 * thread with a broadcast is the cheap thing to do rather than the expensive one. */
static Mutex ThreadMutex;
static CondVar ThreadCv;
static bool PoolReady;

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int iclamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* Nodes between clock checks: fine enough that a search cannot overrun by more than a
 * millisecond or two, coarse enough that the clock read does not show up in a profile.
 * Must be a power of two. */
#define CHECK_INTERVAL 2048

void search_limits_clear(SearchLimits *limits) {
    memset(limits, 0, sizeof(*limits));
    limits->startTime = time_ms();
}

/* round(ln(i) * 1024) for i in 0..63, with ln(0) folded to 0. Hard-coded rather than
 * computed with libm: log() may differ by an ULP between platforms, and one ULP either
 * side of an integer boundary changes a reduction, which changes the tree and breaks the
 * cross-machine bench determinism OpenBench requires. */
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
    atomic_store(&Searching, false);
    atomic_store(&StopFlag, false);
    atomic_store(&Pondering, false);
    atomic_store(&ClockOrigin, 0);
    init_reductions();
    board_set_startpos(&RootPos);

    mutex_init(&ThreadMutex);
    cond_init(&ThreadCv);
    PoolReady = true;

    /* One block, and no thread yet - pool_start() makes them, at the first search that
     * needs one. A single-threaded search then takes exactly the path it always did. */
    thread_pool_set(1);
}

void search_clear(void) {
    tt_clear();

    for (int i = 0; i < ThreadCount; ++i) {
        SearchThread *const td = Threads[i];

        memset(td->killers, 0, sizeof(td->killers));
        memset(td->history, 0, sizeof(td->history));
        memset(&td->pawnHistory, 0, sizeof(td->pawnHistory));
        memset(td->counterMoves, 0, sizeof(td->counterMoves));
        memset(td->contHist, 0, sizeof(td->contHist));
        memset(td->captureHist, 0, sizeof(td->captureHist));
        memset(td->pawnCorrHist, 0, sizeof(td->pawnCorrHist));
        memset(td->stack, 0, sizeof(td->stack));
#ifdef MOVE_PICKER_PROFILE
        memset(&td->moveProfile, 0, sizeof(td->moveProfile));
#endif
    }

    /* The calling thread's, which is the one that has been evaluating outside a search.
     * Every worker clears its own at the top of thread_search(). */
    eval_state_clear(eval_state());
}

bool search_running(void) { return atomic_load(&Searching); }
bool search_stopped(void) { return atomic_load(&StopFlag); }

/*
 * What the whole pool has searched. Every thread publishes its own count every
 * CHECK_INTERVAL nodes, so this trails the truth by at most that much per thread -
 * which is why it is a report and a limit and never a decision inside the tree.
 *
 * With one thread it is exactly that thread's counter at its last publication, which is
 * what the single-threaded engine always reported.
 */
uint64_t search_nodes(void) {
    uint64_t total = 0;
    for (int i = 0; i < ThreadCount; ++i)
        total += atomic_load(&Threads[i]->publishedNodes);
    return total;
}

/* The same sum with the caller's own live counters folded in, for the numbers it prints
 * about a search it is in the middle of. */
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

/*
 * Enforces the limits the search cannot express structurally. Depth is handled by the
 * iteration loop; node and time limits have to be noticed mid-tree, and an infinite or
 * pondering search ignores the clock entirely because UCI gives the GUI sole authority
 * over when those end.
 *
 * Thread 0 alone runs this. A helper reading the same clock would reach the same answer
 * a moment later and set the same flag, and the node total it would have to sum is
 * O(threads) - so the helpers poll StopFlag, which is the one thing they need to know.
 */
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

/*
 * Ordering bands, far enough apart that nothing scored within one can be promoted past
 * the band above it:
 *
 *   TT move > winning captures and promotions > killers > counter-move
 *           > quiets by history > losing captures
 *
 * The transposition table move was actually best last time this position was searched,
 * usually to a greater depth, so nothing generated locally should displace it.
 */
#define SCORE_TT       (1 << 24)
#define SCORE_CAPTURE  1000000
#define SCORE_KILLER_1 900000
#define SCORE_KILLER_2 800000
#define SCORE_COUNTER  700000

/* Below every quiet move, since history is bounded by HISTORY_MAX. */
#define SCORE_BAD_CAPTURE (-1000000)

/* Chosen so the whole quiet band stays well below SCORE_COUNTER and the value fits an
 * int16_t with room for the update arithmetic. */
#define HISTORY_MAX 16384

#ifdef TUNE_SEARCH
#define TUNABLE(name, def) int name = (def)
#else
#define TUNABLE(name, def) enum { name = (def) }
#endif

/*
 * Centipawns per ply of remaining depth that the search is willing to assume a position
 * cannot swing by. Every one of these trades safety for speed, is wrong sometimes, and
 * arrived behind its own SPRT rather than on the argument that it looks reasonable.
 *
 * They are also all wrong TOGETHER when the evaluation changes underneath them: each was
 * fitted against the classical model's scale and noise, and the network shares neither.
 * Hence TUNABLE rather than #define - in a normal build it folds to an enum constant and
 * the released engine is unchanged, while `make TUNE_SEARCH=on` makes each a spin option
 * so a sweep costs a `setoption` instead of a rebuild.
 */
TUNABLE(RFP_MARGIN, 67);
#define RFP_DEPTH 7

#define LMP_DEPTH 8

#define FUTILITY_DEPTH 6

/* Much smaller than the "value of a quiet move" intuition suggests: at a null-window node
 * alpha tracks the evaluation, so staticEval sits close to alpha far more often than
 * not, and 100/ply measured at 0.008% of the bench tree. The useful range is narrow. */
TUNABLE(FUTILITY_MARGIN, 58);

/* The depth below which the previous score is too unreliable to aim a window at. A
 * #define rather than a TUNABLE because SPSA perturbs continuously and then rounds, so
 * both sides of a gradient estimate land on the same integer and the measurement is
 * noise; thresholds want a sweep of their own, not a sweep seat. */
#define ASPIRATION_MIN_DEPTH 5

/* How far below alpha the static evaluation must sit before the node drops straight into
 * quiescence. Much larger than futility's because the claim is bigger - this discards
 * the whole node - and it is verified rather than assumed: the qsearch actually runs,
 * and only its result can prune. */
TUNABLE(RAZOR_MARGIN, 309);
#define RAZOR_DEPTH 3

/* A move that loses material outright is worth searching only if there is depth left to
 * show what it wins back. Captures scale linearly and quiets quadratically, because a
 * quiet move that hangs a piece has no compensation to demonstrate in the first place. */
#define SEE_CAPTURE_DEPTH 6
TUNABLE(SEE_CAPTURE_MARGIN, 86);
#define SEE_QUIET_DEPTH 8
TUNABLE(SEE_QUIET_MARGIN, 12);

/*
 * ProbCut, the mirror of razoring at the other end of the window: a capture that still
 * beats a raised beta after a search several plies shallow is strong evidence the node
 * fails high. Only captures are tried, and that is the claim rather than a shortcut -
 * the bet is that material alone carries the node past the raised bound.
 *
 * The depth floor is a TUNABLE only so an ablation has a switch: set it above any depth
 * the search reaches and ProbCut is off with no rebuild. It is a threshold, not a sweep
 * seat - pass it to `make tune ARGS="--exclude ..."`.
 */
TUNABLE(PROBCUT_DEPTH, 5);
#define PROBCUT_REDUCTION 4
TUNABLE(PROBCUT_MARGIN, 109);

/* Standing pat is always available, so a capture that cannot bring the evaluation within
 * a minor piece of alpha even after winning its victim outright will not raise it. The
 * margin covers what the rest of the sequence might swing, hence a piece rather than a
 * pawn. */
TUNABLE(DELTA_MARGIN, 389);

/* The shallowest depth worth a singular test, and how far below the stored score the
 * verification window sits (in sixteenths of a centipawn per ply). The floor keeps it
 * affordable, and the margin has to be wide enough that a merely best move does not read
 * as singular and narrow enough that a forced line still does. */
#define SINGULAR_DEPTH 7
TUNABLE(SINGULAR_MARGIN, 39);

#define CORR_W_UNIT 128
/* How much the correction is believed, out of CORR_W_UNIT. A TUNABLE because how far to
 * trust a learned evaluation bias is the kind of question a sweep answers better than a
 * person; this was chosen rather than fitted. See E14. */
TUNABLE(CORR_W_PAWN, 132);

/* Uncertainty scaling of the margins above: floor percentage, percent per centipawn of
 * learned correction, and the cap. The cap is SPSA-fitted (E22, E22a); the floor and
 * slope are centred values, and cannot be swept against a net with an uncertainty head
 * because unc_scale() returns on the sigma branch before reading them. */
TUNABLE(UNC_SCALE_BASE, 89);
TUNABLE(UNC_SCALE_SLOPE, 2);
TUNABLE(UNC_SCALE_MAX, 144);

/* The same mapping for a net carrying the trained uncertainty head, whose signal is
 * predicted |eval error| rather than learned bias - a larger number with its own
 * distribution, hence its own floor and slope (sixteenths of a percent per cp). Centred
 * on sigma over the d12 bench tree and then fitted by E22; E22a's re-sweep moved both one
 * unit and stopped, so the mapping is converged. */
TUNABLE(UNC_SIGMA_BASE, 73);
TUNABLE(UNC_SIGMA_SLOPE, 13);

/*
 * How much of the mapping each margin actually wants. unc_scale() returns ONE number, and
 * every consumer multiplying by it asserts that RFP, razoring, ProbCut, delta and SEE all
 * want the same conditioning - which nothing ever measured, and which NNUE.md 5c found
 * two to three times too flat over half the tree.
 *
 * So each site weights the mapping DEVIATION: scale_c = 100 + (scale - 100) * W / UNIT.
 * Weighting the deviation rather than the scale keeps this orthogonal to the margin
 * beside it - a weight on the scale itself would just be a second spelling of the margin,
 * and a sweep holding both seats would converge on nothing.
 */
#define UNC_W_UNIT 16
TUNABLE(UNC_W_RFP, 16);
TUNABLE(UNC_W_FUTILITY, 16);
TUNABLE(UNC_W_RAZOR, 16);
TUNABLE(UNC_W_PROBCUT, 16);
TUNABLE(UNC_W_DELTA, 16);

/* Zero because these two never consulted the mapping at all. A SEE threshold is the same
 * kind of claim as a futility margin, so the exclusion was an omission rather than a
 * decision, and a seat that starts at zero costs nothing to leave there. */
TUNABLE(UNC_W_SEE_CAPTURE, 0);
TUNABLE(UNC_W_SEE_QUIET, 0);

/* Zero for that reason and one more: this margin sets a verification WINDOW rather than a
 * pruning threshold, so a wider one extends more rather than prunes less. Whether
 * uncertainty should buy extensions is a genuine question, and not the one the other
 * seats ask. */
TUNABLE(UNC_W_SINGULAR, 0);

/*
 * The second tier: constants that shape a formula rather than sit in a comparison.
 * Wrapping them costs the shipped engine nothing, which the unchanged bench node count is
 * the proof of. Every DIVISOR has a minimum of 1 rather than 0, and that is load-bearing:
 * a sweep may walk a parameter to its bound, and zero here is a division by zero in the
 * hot path.
 *
 * Reductions[d][m] is built from LMR_BASE and LMR_DIVISOR once, so search_tunable_set()
 * rebuilds the table.
 */
TUNABLE(LMR_BASE, 14);
TUNABLE(LMR_DIVISOR, 22);

/* How much history is allowed to pull a reduction back. Larger means less influence,
 * which is why these are divisors and not multipliers. */
TUNABLE(LMR_HIST_DIVISOR, 7714);
TUNABLE(LMR_CONT_DIVISOR, 6845);
TUNABLE(CAPHIST_DIVISOR, 5);

/* Null-move reduction: base, how fast it grows with depth, and how much of the margin
 * above beta may buy extra reduction before it is capped. */
TUNABLE(NMP_BASE, 5);
TUNABLE(NMP_DEPTH_DIVISOR, 5);
TUNABLE(NMP_EVAL_DIVISOR, 187);
TUNABLE(NMP_EVAL_MAX, 3);

/* How much less a node that was on a principal variation, but is not one in this tree,
 * gets reduced. Zero disables the exemption. */
TUNABLE(TTPV_REDUCTION, 2);

/* History bonus curve: the multiplier on depth-squared, and the depth past which extra
 * confidence stops being real. The cap cannot bind unless the search reaches past it, so
 * at STC - where this engine reaches depth 12-13 - a sweep random-walks it inside a flat
 * region; exclude it from the fit there rather than give it room to wander. */
TUNABLE(HIST_BONUS_MUL, 8);
TUNABLE(HIST_BONUS_DEPTH_MAX, 20);

/* The same curve for the moves that FAILED, on its own multiplier. "This move caused a
 * cutoff" and "this move was tried and did not" are not claims of equal strength - the
 * second is levelled at up to sixty-three moves at once - so nothing says the answer to
 * one should size the other. The depth cap stays shared, because it is a statement about
 * the search and is equally true of evidence pointing either way. */
TUNABLE(HIST_MALUS_MUL, 9);

/* Structure-specific evidence supplements the global move history in both ordering
 * and LMR. 128 is one full share; zero disables reads AND learning for an exact A/B. */
TUNABLE(PAWN_HIST_WEIGHT, 128);

/* Retained together at 25/25 after an inconclusive positive test (E32/E33).
 * Rescue credit belongs only to the winning pawn context; reduced-only failures
 * receive a smaller pawn malus. Zero weights disable the respective adjustments. */
TUNABLE(PAWN_RESCUE_WEIGHT, 25);
TUNABLE(PAWN_RESCUE_FLOOR, 32);
TUNABLE(PAWN_EVIDENCE_WEIGHT, 25);

/* Late move pruning: the constant in `moveCount >= base + depth * depth`. */
TUNABLE(LMP_BASE, 11);

/* Half-width of the first aspiration window. */
TUNABLE(ASPIRATION_DELTA, 20);

#ifdef TUNE_SEARCH

/* The sweep's view of the margins above: a name to set them by and the range a sweep may
 * explore. Written by hand rather than generated from a macro list because a list that
 * generated both could not carry the comments above, and the ranges are deliberately
 * wider than anything that looks plausible now - a sweep that cannot leave the
 * neighbourhood of the current value can only confirm it. */
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
    {"SeeQuietMargin", &SEE_QUIET_MARGIN, 5, 120},
    {"DeltaMargin", &DELTA_MARGIN, 50, 600},
    {"ProbCutMargin", &PROBCUT_MARGIN, 30, 300},
    {"ProbCutDepth", &PROBCUT_DEPTH, 5, 99},
    {"SingularMargin", &SINGULAR_MARGIN, 4, 128},
    {"CorrWPawn", &CORR_W_PAWN, 0, 256},
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
    {"UncWSeeCapture", &UNC_W_SEE_CAPTURE, 0, 64},
    {"UncWSeeQuiet", &UNC_W_SEE_QUIET, 0, 64},
    {"UncWSingular", &UNC_W_SINGULAR, 0, 64},
    {"LmrBase", &LMR_BASE, 4, 24},
    {"LmrDivisor", &LMR_DIVISOR, 12, 48},
    {"LmrHistDivisor", &LMR_HIST_DIVISOR, 2048, 32768},
    {"LmrContDivisor", &LMR_CONT_DIVISOR, 2048, 32768},
    {"CapHistDivisor", &CAPHIST_DIVISOR, 1, 32},
    {"NmpBase", &NMP_BASE, 1, 6},
    {"NmpDepthDivisor", &NMP_DEPTH_DIVISOR, 1, 12},
    {"NmpEvalDivisor", &NMP_EVAL_DIVISOR, 50, 600},
    {"NmpEvalMax", &NMP_EVAL_MAX, 0, 5},
    {"HistBonusMul", &HIST_BONUS_MUL, 1, 24},
    {"HistBonusDepthMax", &HIST_BONUS_DEPTH_MAX, 4, 32},
    {"HistMalusMul", &HIST_MALUS_MUL, 1, 32},
    {"PawnHistWeight", &PAWN_HIST_WEIGHT, 0, 256},
    {"PawnRescueWeight", &PAWN_RESCUE_WEIGHT, 0, 100},
    {"PawnRescueFloor", &PAWN_RESCUE_FLOOR, 1, 256},
    {"PawnEvidenceWeight", &PAWN_EVIDENCE_WEIGHT, 0, 100},
    {"LmpBase", &LMP_BASE, 1, 24},
    {"TtPvReduction", &TTPV_REDUCTION, 0, 3},
    {"AspirationDelta", &ASPIRATION_DELTA, 4, 60},
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
            /* Clamped rather than rejected: a sweep that walks outside the range should
             * stay at the edge and keep playing, not have one engine silently keep the
             * previous value for the rest of a match while the other moved. */
            *Tunables[i].value = iclamp(value, Tunables[i].min, Tunables[i].max);

            /* Reductions[][] is precomputed from LMR_BASE and LMR_DIVISOR, so setting
             * either without rebuilding would leave the sweep measuring the previous curve
             * and conclude the parameter does nothing. */
            init_reductions();
            return true;
        }
    return false;
}
#endif

/* Reduction ~ log(depth) * log(moveNumber) / 2.4 in exact integer arithmetic. Both
 * logarithms matter: scaling with move number is the whole idea, and scaling with depth
 * is what stops the reduction being reckless near the leaves, where there is no depth
 * left to absorb a mistake. */
static void init_reductions(void) {
    for (int d = 0; d < 64; ++d)
        for (int m = 0; m < 64; ++m)
            Reductions[d][m] = (uint8_t)((int64_t)LogFixed[d] * LogFixed[m] * LMR_BASE /
                                         (1024LL * 1024 * LMR_DIVISOR));
}

/* Captures and promotions - the moves that change material, and so must never be reduced
 * or written to the quiet history. Castling needs the explicit exclusion: it is encoded
 * king-captures-own-rook, so a naive look at the destination finds a friendly rook and
 * calls it a capture. */
static inline bool is_tactical(const Position *pos, Move m) {
    switch (type_of_move(m)) {
    case MT_CASTLING: return false;
    case MT_EN_PASSANT:
    case MT_PROMOTION: return true;
    default: return piece_on(pos, to_sq(m)) != NO_PIECE;
    }
}

/*
 * Is the material won by playing `m` at least `threshold`, once both sides have exchanged
 * optimally on the target square? MVV-LVA answers what a capture takes; this answers what
 * it WINS, which is the useful question - QxP looks excellent to MVV-LVA and is a
 * disaster if the pawn is defended.
 *
 * The standard swap-off: play the least valuable attacker each time, tracking the
 * balance, and stop as soon as the side to move would rather stand pat. Removing each
 * attacker from `occupied` is what reveals the sliders x-raying through it.
 */
static bool see_ge(const Position *pos, Move m, Value threshold) {
    /* Castling captures nothing, and en passant and promotions change material in ways the
     * swap loop does not model. Decline to judge them. */
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

        /* Capture with the least valuable attacker available. Each branch reveals only the
         * sliders that could have been behind the piece just removed, which is why the
         * x-ray refresh differs per piece type. */
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
            /* Only the king is left, and capturing into a defended square is illegal - so if
             * the other side still attacks it, the side to move cannot continue and loses
             * the exchange by default. */
            return (attackers & ~color_bb(pos, stm)) ? (result ^ 1) != 0 : result != 0;
        }
    }
    return result != 0;
}

/* The continuation-history slice for the move played `back` plies above `ply`, or NULL
 * when there is no such move - the top of the tree, or a null move, which is nobody's
 * plan and must not have continuations attributed to it. */
static inline int16_t *cont_slice(SearchThread *td, int slot, int ply, int back) {
    if (ply < back)
        return NULL;

    const Move prev = td->stack[ply - back].move;
    if (!is_ok_move(prev))
        return NULL;

    return &td->contHist[slot][td->stack[ply - back].movedPiece][to_sq(prev)][0][0];
}

static inline int cont_index(Piece pc, Square to) { return (int)pc * SQUARE_NB + (int)to; }

/* What every continuation slot together thinks of playing `pc` to `to` here. */
static inline int cont_score(int16_t *const *slices, Piece pc, Square to) {
    const int idx = cont_index(pc, to);
    int total     = 0;

    for (int i = 0; i < CONT_SLOTS; ++i)
        if (slices[i])
            total += slices[i][idx];

    return total;
}

/* The victim a move takes, or NO_PIECE_TYPE. Castling is encoded king-captures-own-rook,
 * so it needs the explicit exclusion. */
static inline PieceType victim_of(const Position *pos, Move m) {
    switch (type_of_move(m)) {
    case MT_EN_PASSANT: return PAWN;
    case MT_CASTLING: return NO_PIECE_TYPE;
    default: return type_of(piece_on(pos, to_sq(m)));
    }
}

static inline int pawn_history_score(SearchThread *td, Key key, Piece pc, Square to) {
    if (PAWN_HIST_WEIGHT == 0)
        return 0;
    return history_pawn_score(history_pawn_entry(&td->pawnHistory, key, pc, to), PAWN_HIST_WEIGHT);
}

/* MVV-LVA for the tactical moves - most valuable victim, least valuable attacker - with
 * SEE deciding which band a capture lands in and capture history separating the ones SEE
 * calls equal. Quiet moves fall through to the heuristic tables. */
/*
 * Returns the index of the highest-scoring move, taking the FIRST of equals - which is
 * exactly what pick_move() would have selected, generation order being the tie-break the
 * tree depends on. Handing it back costs one comparison per move on a loop that is
 * already touching every score, and saves the longest scan there is: the first selection
 * in the loop below, over the whole list.
 */
static int score_moves_context(SearchThread *td, const Position *pos, ScoredMove *list, int count,
                               Move ttMove, int ply, Move counter, Move killer0, Move killer1) {
    const Color us = pos->sideToMove;

    /* Context for continuation history: the moves that led to this node. */
    int16_t *slices[CONT_SLOTS];
    for (int i = 0; i < CONT_SLOTS; ++i)
        slices[i] = cont_slice(td, i, ply, ContPlies[i]);

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
            MP_ADD(td, orderingSee, 1);
            const int mvvLva = PieceValues[victim] * 16 - PieceValues[type_of(moved)];

            /* Divided down so it refines the MVV-LVA order rather than overturning it:
             * history is evidence about a capture, not a replacement for knowing what it
             * takes. A capture that loses material once the recaptures are played out is
             * worse than almost any quiet move, so it goes below them. */
            const int capHist = td->captureHist[moved][to_sq(m)][victim] / CAPHIST_DIVISOR;

            score =
                (see_ge(pos, m, VALUE_ZERO) ? SCORE_CAPTURE : SCORE_BAD_CAPTURE) + mvvLva + capHist;
        }

        if (mt == MT_PROMOTION)
            score += SCORE_CAPTURE + PieceValues[promotion_type(m)];

        /* Nothing tactical matched, so the move is quiet. Tested explicitly rather than by
         * `score == 0`: capture history can push a capture's score anywhere inside its
         * band, including onto zero. */
        if (victim == NO_PIECE_TYPE && mt != MT_PROMOTION) {
            if (m == killer0)
                score = SCORE_KILLER_1;
            else if (m == killer1)
                score = SCORE_KILLER_2;
            else if (m == counter)
                score = SCORE_COUNTER;
            else
                score = td->history[us][from_sq(m)][to_sq(m)] +
                        cont_score(slices, moved, to_sq(m)) +
                        pawn_history_score(td, pos->pawnKey, moved, to_sq(m));
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

/* A cutoff found deep in the tree is much stronger evidence than one next to the leaves,
 * so the bonus grows with depth - but is capped, because beyond a point the extra
 * confidence is not real and one deep cutoff should not saturate an entry alone. */
static int history_bonus(Depth depth) {
    const Depth d = imin(depth, HIST_BONUS_DEPTH_MAX);
    return d * d * HIST_BONUS_MUL;
}

/* What everything tried before the cutoff is charged: the same shape on its own
 * multiplier. */
static int history_malus(Depth depth) {
    const Depth d = imin(depth, HIST_BONUS_DEPTH_MAX);
    return d * d * HIST_MALUS_MUL;
}

/* Applies `bonus`, decaying the entry toward zero in proportion to how large it already
 * is. That gravity term is the whole design: a plain running total saturates and ends up
 * describing the opening rather than the position on the board, where scaling the decay
 * by the current value makes the entry an exponential moving average of recent success. */
static void history_update(int16_t *entry, int bonus) {
    const int b = bonus > HISTORY_MAX ? HISTORY_MAX : bonus < -HISTORY_MAX ? -HISTORY_MAX : bonus;
    *entry += (int16_t)(b - (int)*entry * (b < 0 ? -b : b) / HISTORY_MAX);
}

/* Credit or blame `pc -> to` in every continuation slot that exists here. */
static void cont_hist_update(SearchThread *td, int ply, Piece pc, Square to, int bonus) {
    const int idx = cont_index(pc, to);

    for (int i = 0; i < CONT_SLOTS; ++i) {
        int16_t *const slice = cont_slice(td, i, ply, ContPlies[i]);
        if (slice)
            history_update(&slice[idx], bonus);
    }
}

/* Keyed on what the capture takes, so the victim has to be read off the board - which
 * means this must run AFTER the move was undone. */
static void capture_hist_update(SearchThread *td, const Position *pos, Move m, int bonus) {
    const Piece moved = piece_on(pos, from_sq(m));
    history_update(&td->captureHist[moved][to_sq(m)][victim_of(pos, m)], bonus);
}

/*
 * Records that `best` caused a cutoff and that everything tried before it did not.
 * Knowing what fails is worth as much as knowing what works: without the penalty, a move
 * tried early that always fails keeps its position forever, because nothing pushes it
 * down.
 *
 * Both lists are penalised whichever kind of move cut, because both were tried and both
 * failed; only the winner's own table is credited, since a capture teaches the quiet
 * heuristics nothing.
 */
static void update_stats(SearchThread *td, const Position *pos, Move best, const Move *quiets,
                         const int *quietPawnMaluses, int quietCount, const Move *captures,
                         int captureCount, Depth depth, int ply, int pawnExtraCredit) {
    const Color us   = pos->sideToMove;
    const int bonus  = history_bonus(depth);
    const int malus  = history_malus(depth);
    const bool quiet = !is_tactical(pos, best);

    if (quiet) {
        if (td->killers[ply][0] != best) {
            td->killers[ply][1] = td->killers[ply][0];
            td->killers[ply][0] = best;
        }

        history_update(&td->history[us][from_sq(best)][to_sq(best)], bonus);
        cont_hist_update(td, ply, piece_on(pos, from_sq(best)), to_sq(best), bonus);
        if (PAWN_HIST_WEIGHT != 0)
            history_update(history_pawn_entry(&td->pawnHistory, pos->pawnKey,
                                              piece_on(pos, from_sq(best)), to_sq(best)),
                           bonus + pawnExtraCredit);

        const Move prev = td->stack[ply - 1].move;
        if (is_ok_move(prev))
            td->counterMoves[td->stack[ply - 1].movedPiece][to_sq(prev)] = best;
    } else {
        capture_hist_update(td, pos, best, bonus);
    }

    for (int i = 0; i < quietCount; ++i) {
        if (quiets[i] == best)
            continue;
        history_update(&td->history[us][from_sq(quiets[i])][to_sq(quiets[i])], -malus);
        cont_hist_update(td, ply, piece_on(pos, from_sq(quiets[i])), to_sq(quiets[i]), -malus);
        if (PAWN_HIST_WEIGHT != 0)
            history_update(history_pawn_entry(&td->pawnHistory, pos->pawnKey,
                                              piece_on(pos, from_sq(quiets[i])), to_sq(quiets[i])),
                           -(PAWN_EVIDENCE_WEIGHT != 0 ? quietPawnMaluses[i] : malus));
    }

    for (int i = 0; i < captureCount; ++i)
        if (captures[i] != best)
            capture_hist_update(td, pos, captures[i], -malus);
}

/* Anything but kings and pawns - the test that decides whether null-move pruning is
 * safe. */
static inline bool has_non_pawn_material(const Position *pos, Color c) {
    return (pieces2_bb(pos, c, KNIGHT, BISHOP) | pieces2_bb(pos, c, ROOK, QUEEN)) != BB_EMPTY;
}

/* Selection sort, one move at a time: a beta cutoff usually lands within the first few
 * moves, so sorting the whole list up front is mostly wasted work. */
/* The first selection, which score_moves() already worked out. */
static inline void pick_first(ScoredMove *list, int best) {
    if (best != 0) {
        const ScoredMove tmp = list[0];
        list[0]              = list[best];
        list[best]           = tmp;
    }
}

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

/* Invocation-local, NOT indexed by ply: singular verification re-enters at the
 * same ply while its parent's list is still alive. No allocation or buffer clear
 * on the playing path. The two disjoint generators together fit MAX_MOVES. */
typedef struct {
    ScoredMove moves[MAX_MOVES];
    Move tt, excluded;
    Move refutations[3];
    Move returned[3];
    PickStage stage;
    int cur, end, start;
    int tacticalCount, quietCount, badBegin;
} MovePicker;

static void picker_init(MovePicker *mp, SearchThread *td, const Position *pos, Move tt,
                        Move excluded, int ply, Move counter) {
    MP_ADD(td, mainNodes, 1);
    mp->tt             = tt;
    mp->excluded       = excluded;
    mp->refutations[0] = td->killers[ply][0];
    mp->refutations[1] = td->killers[ply][1];
    mp->refutations[2] = counter;
    mp->returned[0] = mp->returned[1] = mp->returned[2] = MOVE_NONE;
    mp->cur = mp->end = mp->start = mp->badBegin = 0;
    mp->tacticalCount = mp->quietCount = -1;
    mp->stage                          = PICK_TT;

    /* Keep the old, eager evasion ordering. A pseudo-legal TT move alone does
     * not prove membership of GEN_EVASIONS. */
    if (board_checkers(pos)) {
        mp->stage = PICK_EVASIONS;
        mp->end   = movegen_generate(pos, GEN_EVASIONS, mp->moves);
        MP_ADD(td, genCalls[2], 1);
        MP_ADD(td, generated[2], mp->end);
        MP_ADD(td, scored[2], mp->end);
        pick_first(mp->moves, score_moves(td, pos, mp->moves, mp->end, tt, ply, counter));
    }
#ifdef MOVE_PICKER_EAGER
    else {
        /* Matched-behaviour timing control: generation is eager, but scoring
         * still happens at exactly the same stages as in the lazy build. */
        mp->tacticalCount = movegen_generate(pos, GEN_TACTICALS, mp->moves);
        mp->quietCount    = movegen_generate(pos, GEN_NON_TACTICALS, mp->moves + mp->tacticalCount);
        MP_ADD(td, genCalls[0], 1);
        MP_ADD(td, generated[0], mp->tacticalCount);
        MP_ADD(td, genCalls[1], 1);
        MP_ADD(td, generated[1], mp->quietCount);
        assert(mp->tacticalCount + mp->quietCount <= MAX_MOVES);
    }
#endif
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
            /* Search validated the TT encoding when probing. Legality stays in
             * the common loop, before moveCount or board mutation. */
            if (mp->tt != MOVE_NONE && mp->tt != mp->excluded)
                return mp->tt;
            break;

        case PICK_GENERATE_TACTICALS:
            if (mp->tacticalCount < 0) {
                mp->tacticalCount = movegen_generate(pos, GEN_TACTICALS, mp->moves);
                MP_ADD(td, genCalls[0], 1);
                MP_ADD(td, generated[0], mp->tacticalCount);
            }
            MP_ADD(td, scored[0], mp->tacticalCount);
            mp->cur = mp->start = 0;
            mp->end             = mp->tacticalCount;
            pick_first(mp->moves, score_moves(td, pos, mp->moves, mp->end, mp->tt, ply, MOVE_NONE));
#ifndef NDEBUG
            /* A future SEE/promotion change must not silently put a tactical
             * between quiets while this picker assumes two separated bands. */
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
            /* The best remaining tactical has already been selected. Preserve
             * it, and its scores, across quiet subtrees. */
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
            if (mp->quietCount < 0) {
                mp->quietCount =
                    movegen_generate(pos, GEN_NON_TACTICALS, mp->moves + mp->tacticalCount);
                MP_ADD(td, genCalls[1], 1);
                MP_ADD(td, generated[1], mp->quietCount);
            }
            MP_ADD(td, scored[1], mp->quietCount);
            assert(mp->tacticalCount + mp->quietCount <= MAX_MOVES);
            mp->cur = mp->start = mp->tacticalCount;
            mp->end             = mp->tacticalCount + mp->quietCount;
            /* Refutations were snapshotted and tried already. Reading NEW
             * killers here after singular verification would create a second
             * refutation stage inside what is meant to be the history band. */
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

/* Acceptance bridges use isolated state, so the UCI selftest cannot contaminate
 * the pool's histories. No playing-path branch or allocation is needed. */
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

    /* A nested same-ply picker and changed refutations cannot overwrite the
     * parent's candidate snapshot. This is singular verification's lifetime. */
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
#ifndef MOVE_PICKER_EAGER
    failures += parent.quietCount != -1;
#endif
    /* History is read when quiet scoring is reached, then frozen for that
     * batch, not silently re-read on every next(). */
    td->history[WHITE][SQ_A2][SQ_A4] = HISTORY_MAX;
    failures += picker_next(&parent, td, pos, 1) != deferred;
    MovePicker frozen = parent;
    for (int from = 0; from < SQUARE_NB; ++from)
        for (int to = 0; to < SQUARE_NB; ++to)
            td->history[WHITE][from][to] = (int16_t)(to * 73 - from * 31);
    for (int i = 0; i < MAX_MOVES; ++i) {
        const Move a = picker_next(&parent, td, pos, 1);
        const Move b = picker_next(&frozen, td, pos, 1);
        failures += a != b;
        if (a == MOVE_NONE)
            break;
        if (i == MAX_MOVES - 1)
            ++failures;
    }

    /* All promotion scores are in the early band today: SEE declines to
     * judge non-normal moves. Exercise the same formulas at history limits,
     * rather than relying on a historical description of SEE. */
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

    /* Read each kind of mutable ordering evidence at its documented stage,
     * including contexts that share storage with a descendant's updates. */
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
    for (int context = 0; context < 2; ++context) {
        board_set_startpos(pos);
        memset(td->history, 0, sizeof(td->history));
        td->stack[0].move       = make_move(SQ_E7, SQ_E5);
        td->stack[0].movedPiece = B_PAWN;
        picker_init(&parent, td, pos, tt, MOVE_NONE, 1, MOVE_NONE);
        failures += picker_next(&parent, td, pos, 1) != tt;
        int16_t *entry   = context == 0
                               ? history_pawn_entry(&td->pawnHistory, pos->pawnKey, W_PAWN, SQ_A4)
                               : &td->contHist[0][B_PAWN][SQ_E5][W_PAWN][SQ_A4];
        *entry           = HISTORY_MAX;
        const Move first = picker_next(&parent, td, pos, 1);
        if (context != 0 || PAWN_HIST_WEIGHT != 0)
            failures += first != deferred;
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

/* Correction history remembers how far the static evaluation and the search have been
 * running apart for a given pawn structure. Pawn structure is the key because it survives
 * the moves a search makes, so the bias it carries is worth learning. */
static inline int16_t *corr_entry(SearchThread *td, const Position *pos) {
    return &td->pawnCorrHist[pos->sideToMove][pos->pawnKey & (CORRHIST_SIZE - 1)];
}

/* Nothing the search reports is corrected. The corrected value feeds `improving`, the
 * margins and the reductions - decisions that are already bets - while the table keeps the
 * raw evaluation, so a later probe re-corrects with whatever has been learned since. */
static Value corrected_eval(SearchThread *td, const Position *pos, Value raw) {
    if (raw == VALUE_NONE)
        return VALUE_NONE;

    /* Mate and tablebase scores are clamped away deliberately: a correction is evidence
     * about an evaluation, and letting one push a score into a range reserved for proven
     * results would have the search report a proof that nothing proved. */
    const int v = raw + (CORR_W_PAWN * *corr_entry(td, pos) / CORR_W_UNIT) / CORRHIST_GRAIN;
    return (Value)iclamp(v, VALUE_TB_LOSS_IN_MAX_PLY + 1, VALUE_TB_WIN_IN_MAX_PLY - 1);
}

/* Folds one observation in as an exponential moving average, weighted by depth because a
 * deeper search is better evidence about the same question. What counts as an observation
 * is the decision that matters, and it is made at the call site. */
static void corrhist_update(SearchThread *td, const Position *pos, Value searched, Value staticEval,
                            Depth depth) {
    int16_t *const e  = corr_entry(td, pos);
    const int weight  = imin(depth + 1, 16);
    const int diff    = (searched - staticEval) * CORRHIST_GRAIN;
    const int updated = (*e * (CORRHIST_WEIGHT_MAX - weight) + diff * weight) / CORRHIST_WEIGHT_MAX;

    *e = (int16_t)iclamp(updated, -CORRHIST_LIMIT, CORRHIST_LIMIT);
}

/* One consumer's share of the mapping; see the weights' declaration for why this scales
 * the deviation from 100 rather than the scale. Clamped at zero because a large enough
 * weight on a below-100 scale would otherwise produce a NEGATIVE margin, which is not a
 * tighter margin but a different rule. */
static inline int unc_apply(int scale, int weight) {
    const int scaled = 100 + (scale - 100) * weight / UNC_W_UNIT;
    return scaled < 0 ? 0 : scaled;
}

/*
 * How wide this node's margin-based prunes should be, as a percentage. Every margin above
 * is a global constant fitted by SPSA - a statement about the AVERAGE position - where the
 * residual it insures against is nowhere near homoscedastic.
 *
 * So they are scaled by whichever signal the loaded net offers: a trained uncertainty head
 * predicts the residual's scale directly, and otherwise |correction| stands in. The
 * corrhist defaults are centred rather than chosen, and a cold entry lands on the floor -
 * which is why the floor sits just under 100.
 */
/* Optional uncapped head output for rescue learning. Keeping it local to the node
 * avoids another inference and survives child searches without shared state. */
static inline int unc_scale(SearchThread *td, const Position *pos, int *errorEstimate) {
    if (errorEstimate)
        *errorEstimate = -1;
#ifdef EVAL_NNUE
    if (nnue_has_uncertainty()) {
        const int sigma = nnue_uncertainty(td->es, pos);
        const int scale = imin(UNC_SIGMA_BASE + sigma * UNC_SIGMA_SLOPE / 16, UNC_SCALE_MAX);
        if (errorEstimate)
            *errorEstimate = sigma;
        return unc_probe(sigma, scale);
    }
#endif
    const int c     = *corr_entry(td, pos);
    const int ac    = (c < 0 ? -c : c) / CORRHIST_GRAIN;
    const int scale = imin(UNC_SCALE_BASE + ac * UNC_SCALE_SLOPE, UNC_SCALE_MAX);
    return unc_probe(ac, scale);
}

/*
 * The signal above is a second inference through the net, and most nodes never reach a
 * margin that wants it: quiescence that stands pat, a PV node - every consumer below is
 * non-PV only - or a node whose tests are all gated off by depth. Deferring it to first
 * use cannot change a decision, because it is a function of the position and not of when
 * it is read, and it removes the inference outright wherever nothing asks.
 *
 * UNC_PENDING is the "not yet asked" state; 100 is "neutral, never ask", which is what a
 * node in check wants, since every consumer is disabled there.
 */
#define UNC_PENDING (-1)

/* Eager under UNC_PROBE on purpose: the probe's population is the set of nodes that READ
 * the signal, and deferring would silently resample it against the very mapping it exists
 * to re-centre. */
static inline int unc_start(bool neutral, int *errorEstimate, SearchThread *td,
                            const Position *pos) {
    if (neutral)
        return 100;
#ifdef UNC_PROBE
    return unc_scale(td, pos, errorEstimate);
#else
    (void)errorEstimate;
    (void)td;
    (void)pos;
    return UNC_PENDING;
#endif
}

static inline int unc_get(int *cache, int *errorEstimate, SearchThread *td, const Position *pos) {
    if (*cache == UNC_PENDING)
        *cache = unc_scale(td, pos, errorEstimate);
    return *cache;
}

#ifdef UNC_PROBE

/* The other half of what `probe err` pairs: this node's signal, and what the search ended
 * up saying about the same node. Read again here rather than carried down from
 * unc_scale() to keep the probe out of the search stack; it is the same number, being a
 * function of the position. */
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

/* Which constants the mapping is running on, and on which signal. The branch is the one
 * unc_scale() takes, written once here so a report of the mapping cannot describe a
 * branch the search is not taking. */
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

/*
 * Search only the forcing moves until the position is quiet. Without this the engine
 * hangs pieces at every depth: a search that stops counting material in the middle of an
 * exchange believes whatever the last capture left on the board. It is not an
 * optimisation, it is the difference between an engine that plays chess and one that does
 * not.
 */
static Value qsearch(SearchThread *td, Position *pos, Value alpha, Value beta, int ply) {
    count_node(td);
    update_seldepth(td, ply);

    if (search_stopped())
        return VALUE_ZERO;

    if (ply >= MAX_PLY - 1)
        return corrected_eval(td, pos, eval_evaluate(td->es, pos));

    const bool pvNode = beta - alpha > 1;
    const Key key     = pos->key;

    /* Quiescence probes the same table the main search writes to. Everything stored from
     * here carries depth 0, so a quiescence entry can never satisfy a main-search probe -
     * but a main-search entry, being deeper, answers a quiescence probe perfectly well. */
    TTEntry tte;
    const bool ttHit    = tt_probe(key, &tte);
    const Value ttValue = ttHit ? tt_value_from_tt(tt_entry_value(&tte), ply) : VALUE_NONE;
    Move ttMove         = ttHit ? tt_entry_move(&tte) : MOVE_NONE;

    /* Sixteen bits of key is not proof of identity, and the entry may predate several moves
     * of the game. Never play a probed move unvalidated. */
    if (ttMove != MOVE_NONE && !movegen_is_pseudo_legal(pos, ttMove))
        ttMove = MOVE_NONE;

    /* Quiescence PRESERVES the flag and never sets it. Seeding from `pvNode` looks
     * equivalent and is not: a PV node hands quiescence a full window, and 18% of the
     * classical bench tree would then reduce less for having once been a recapture on the
     * principal variation. */
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
        /* Stand pat: the side to move is never obliged to capture, so the static evaluation
         * is a lower bound on what it can achieve. In check there is no such option and
         * every reply must be searched. */
        rawEval    = ttHit && tt_entry_eval(&tte) != VALUE_NONE ? tt_entry_eval(&tte)
                                                                : eval_evaluate(td->es, pos);
        staticEval = corrected_eval(td, pos, rawEval);
        uncScale   = unc_start(false, NULL, td, pos);
        best       = staticEval;

        if (best >= beta) {
            tt_store(key, MOVE_NONE, best, rawEval, 0, BOUND_LOWER, ttPv, ply);
            return best;
        }
        if (best > alpha)
            alpha = best;
    }

    ScoredMove moves[MAX_MOVES];
    const int count = movegen_generate(pos, inCheck ? GEN_EVASIONS : GEN_CAPTURES, moves);
    MP_ADD(td, genCalls[3], 1);
    MP_ADD(td, generated[3], count);
    MP_ADD(td, scored[3], count);

    /* No counter-move: quiescence only reaches quiet moves when answering a check, and an
     * evasion is dictated by the check rather than by whatever the opponent played. */
    pick_first(moves, score_moves(td, pos, moves, count, ttMove, ply, MOVE_NONE));

    Move bestMove = MOVE_NONE;
    int legal     = 0;

    for (int i = 0; i < count; ++i) {
        if (i > 0)
            pick_move(moves, count, i);
        const Move m = moves[i].m;

        /* Drop the losing captures. score_moves already ran SEE and put them in the only
         * negative band a capture list contains, and pick_move has just selected the highest
         * remaining score, so once it is negative the whole tail can go. This is where
         * quiescence earns most of its speed. */
        if (!inCheck && moves[i].score < 0)
            break;

        /* Delta pruning: even winning this victim outright, plus a margin for what the rest
         * of the sequence swings, leaves the score short of alpha, and standing pat already
         * beats that. Skipped for promotions, whose gain is the new piece rather than the
         * captured one, and in check, where there is no stand pat. */
        if (!inCheck && type_of_move(m) != MT_PROMOTION && !is_mate_score(alpha)) {
            /* What the move wins outright, before any allowance for what the rest of the
             * sequence might swing. The allowance is added, so a capture that clears alpha
             * on material alone is never pruned whatever the head says - and most do, which
             * is what keeps the second inference out of quiescence. */
            const Value bare = staticEval + PieceValues[victim_of(pos, m)];

            if (bare <= alpha &&
                bare + DELTA_MARGIN * unc_apply(unc_get(&uncScale, NULL, td, pos), UNC_W_DELTA) /
                            100 <=
                    alpha)
                continue;
        }

        if (!movegen_is_legal(pos, m))
            continue;
        ++legal;

        /* Quiescence maintains the stack too, so a node below it reads the move that
         * actually led there rather than whatever the main search left at this ply on an
         * earlier visit. */
        td->stack[ply].move       = m;
        td->stack[ply].movedPiece = piece_on(pos, from_sq(m));
        td->stack[ply].staticEval = staticEval;

        board_do_move(pos, m);
        eval_state_push(td->es, pos, m);
        tt_prefetch(pos->key);
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

    /* Evasions are the complete move list, so no legal reply here really is checkmate -
     * worth detecting, since missing it makes the engine walk into mate believing it has
     * stand-pat equality. */
    if (inCheck && legal == 0)
        return mated_in(ply);

    /* Quiescence never searches the full move list, so it can never prove an exact score:
     * the value is a lower bound if it failed high and an upper bound otherwise. */
    tt_store(key, bestMove, best, rawEval, 0, best >= beta ? BOUND_LOWER : BOUND_UPPER, ttPv, ply);

    return best;
}

/*
 * `cutNode` is the caller's expectation, not a fact: it is true at a node the parent
 * believes will fail high. It costs nothing to propagate and it is the best available
 * prior on how a node will turn out, which is exactly what the reductions want; being
 * wrong about it is safe, since everything it feeds either re-searches or is bounded by
 * depth.
 */
static Value negamax(SearchThread *td, Position *pos, Depth depth, Value alpha, Value beta, int ply,
                     bool cutNode) {
    /* Cleared here rather than at each use, so a child that never extends the PV leaves a
     * length of zero behind. */
    td->pvLength[ply] = 0;

    if (depth <= 0)
        return qsearch(td, pos, alpha, beta, ply);

    count_node(td);
    update_seldepth(td, ply);

    if (search_stopped())
        return VALUE_ZERO;

    /* Read and clear in one step. A singular search sets this immediately before re-entering
     * at the same ply, and every other entry must see MOVE_NONE - including a later,
     * unrelated visit that would otherwise inherit an exclusion. */
    const Move excluded         = td->stack[ply].excludedMove;
    td->stack[ply].excludedMove = MOVE_NONE;
    const bool isExcluded       = excluded != MOVE_NONE;

    /* The root is handled by search_root, so this is never ply 0 - which is what lets the
     * draw and mate-distance tests below run unconditionally. */
    assert(ply > 0);

    if (board_is_draw(pos, ply))
        return VALUE_DRAW;

    if (ply >= MAX_PLY - 1)
        return corrected_eval(td, pos, eval_evaluate(td->es, pos));

    /* Determined before mate distance pruning narrows the window: a node is a PV node
     * because of where it sits in the tree, and must keep being treated as one even if the
     * narrowing leaves it with a null window. */
    const bool pvNode = beta - alpha > 1;

    /* Mate distance pruning. Once a mate is known at this ply, no line can beat it by
     * mating later and none can be worse than being mated now, so the window narrows to
     * that range for two comparisons. */
    if (alpha < mated_in(ply))
        alpha = mated_in(ply);
    if (beta > mate_in(ply + 1))
        beta = mate_in(ply + 1);
    if (alpha >= beta)
        return alpha;

    const Key key = pos->key;

    TTEntry tte;
    const bool ttHit    = tt_probe(key, &tte);
    const Value ttValue = ttHit ? tt_value_from_tt(tt_entry_value(&tte), ply) : VALUE_NONE;
    Move ttMove         = ttHit ? tt_entry_move(&tte) : MOVE_NONE;

    if (ttMove != MOVE_NONE && !movegen_is_pseudo_legal(pos, ttMove))
        ttMove = MOVE_NONE;

    /*
     * Was this position ever on a principal variation? `pvNode` says where the node sits in
     * THIS tree; the table says where the position sat in every tree before, which is the
     * better-informed question - a position worth an exact score once is worth accuracy
     * again now.
     *
     * It cannot run away: the flag is seeded only by genuine PV nodes and spreads only
     * through the table, never down the stack, so the set it marks is bounded by the
     * positions that have actually appeared on a principal variation.
     */
    const bool ttPv = pvNode || (ttHit && tt_entry_is_pv(&tte));

    /*
     * Cut off on a stored result at least as deep whose bound points the right way. Not at
     * PV nodes: a bound proves a cutoff but cannot name the move that caused it, so the
     * reported principal variation would be truncated here.
     *
     * Not near the fifty-move boundary either - a stored score says nothing about how many
     * reversible moves preceded the position. And never while verifying a singular move,
     * whose stored result was proved with the excluded move available.
     */
    if (!isExcluded && !pvNode && ttValue != VALUE_NONE && tt_entry_depth(&tte) >= depth &&
        pos->halfmoveClock < 90 &&
        (tt_entry_bound(&tte) & (ttValue >= beta ? BOUND_LOWER : BOUND_UPPER)))
        return ttValue;

    /*
     * Syzygy WDL probe. The halfmove-clock gate keeps this off the hot path and is also a
     * correctness condition: WDL tables assume a fresh fifty-move counter, so only the
     * capture or pawn move that entered the tablebase region probes and everything behind
     * it transposes through the entry stored here.
     *
     * The value is game-theoretic truth rather than a heuristic, so it is returned
     * outright; a win is only a lower bound (a search might still prefer the faster mate)
     * and a loss only an upper one. Stored a few plies deeper than asked so neighbouring
     * nodes transpose into it rather than re-probing the disk.
     */
    if (TbLimit != 0 && !isExcluded && pos->halfmoveClock == 0 && pos->castling == NO_CASTLING &&
        popcount(occupied_bb(pos)) <= TbLimit) {
        const Value tbValue = syzygy_probe_wdl(pos, ply);
        if (tbValue != VALUE_NONE) {
            ++td->tbHits;
            const Bound tbBound = tbValue > VALUE_DRAW   ? BOUND_LOWER
                                  : tbValue < VALUE_DRAW ? BOUND_UPPER
                                                         : BOUND_EXACT;
            tt_store(key, MOVE_NONE, tbValue, VALUE_NONE, (Depth)imin(depth + 6, MAX_PLY - 1),
                     tbBound, ttPv, ply);
            return tbValue;
        }
    }

    const Color us     = pos->sideToMove;
    const bool inCheck = board_checkers(pos) != BB_EMPTY;

    /* Internal iterative reduction: no table move means nothing has searched this node
     * deeply enough to leave an opinion, so the list will be ordered by heuristics alone -
     * and a badly ordered node at full depth is the most expensive kind there is. One ply
     * shallower is cheaper and leaves a table move behind for the revisit. */
    if (depth >= 4 && ttMove == MOVE_NONE && !inCheck)
        --depth;

    /* Reusing the evaluation cached in the table when this position has been seen before.
     * In check it is not computed at all: the score of a position whose king is attacked
     * says nothing useful, and every heuristic that would consume it is disabled while in
     * check anyway. The table keeps `rawEval`; everything below reasons with the corrected
     * one. */
    const Value rawEval =
        inCheck ? VALUE_NONE
                : (ttHit && tt_entry_eval(&tte) != VALUE_NONE ? tt_entry_eval(&tte)
                                                              : eval_evaluate(td->es, pos));

    const Value staticEval = corrected_eval(td, pos, rawEval);
    int evalError          = -1;

    /* Where the error estimate is wanted, unchanged: unc_scale() leaves it at -1 when it is
     * not asked for, and the rescue credit reads -1 as "no head". */
    int *const errSink = PAWN_HIST_WEIGHT != 0 && PAWN_RESCUE_WEIGHT != 0 ? &evalError : NULL;
    int uncScale       = unc_start(inCheck, errSink, td, pos);

    td->stack[ply].staticEval = staticEval;

    /* Is this side's position getting better? Compared against the GRANDPARENT, because that
     * is the last node where the same side was to move. A rising evaluation is more likely
     * to produce the fail high pruning is betting on, and a falling one deserves the benefit
     * of the doubt; in check and at the top of the tree it defaults to false, which prunes
     * less. */
    const bool improving = !inCheck && ply >= 2 && td->stack[ply - 2].staticEval != VALUE_NONE &&
                           staticEval > td->stack[ply - 2].staticEval;

    /* Reverse futility pruning: the static evaluation is so far above beta that conceding a
     * pawn-and-a-bit per remaining ply would not bring it down, so the node is not going to
     * fail low. The depth limit keeps it honest - deep enough searches find swings of any
     * size - and it is restricted to non-PV nodes, where a bound was all anyone wanted. */
    if (!pvNode && !inCheck && depth <= RFP_DEPTH && !is_mate_score(beta) &&
        /* The margin is subtracted, so failing this cannot be rescued by any scale the
         * head could report - and asking it is a whole inference. */
        staticEval >= beta &&
        staticEval - RFP_MARGIN * (depth - improving) *
                         unc_apply(unc_get(&uncScale, errSink, td, pos), UNC_W_RFP) / 100 >=
            beta)
        return staticEval;

    /* Razoring, the mirror image at the other end of the window: a position this far below
     * alpha is one the quiet moves will not save, and whether a tactic does is exactly what
     * quiescence answers. The qsearch is what makes it safe - the margin only decides the
     * node is worth a cheap second opinion, and the node is dropped only if that opinion
     * agrees. */
    if (!pvNode && !inCheck && depth <= RAZOR_DEPTH && !is_mate_score(alpha) &&
        /* Added, so no scale can bring this under alpha if the bare evaluation is not. */
        staticEval < alpha &&
        staticEval + RAZOR_MARGIN * depth *
                         unc_apply(unc_get(&uncScale, errSink, td, pos), UNC_W_RAZOR) / 100 <
            alpha) {
        const Value v = qsearch(td, pos, alpha - 1, alpha, ply);
        if (v < alpha)
            return v;
    }

    /* Null-move pruning: hand the opponent a free move, and if the position still fails high
     * it was too good to be worth searching properly. The non-pawn-material test is what
     * keeps this sound - in a king-and-pawn endgame zugzwang is often the whole content of
     * the position, and a side that would love to pass will "prove" a cutoff it cannot
     * achieve. Refusing two null moves in a row matters for the same reason. */
    if (!pvNode && !inCheck && !isExcluded && depth >= 3 && staticEval >= beta &&
        td->stack[ply - 1].move != MOVE_NULL && has_non_pawn_material(pos, us)) {
        const Depth r = NMP_BASE + depth / NMP_DEPTH_DIVISOR +
                        imin((staticEval - beta) / NMP_EVAL_DIVISOR, NMP_EVAL_MAX);

        td->stack[ply].move       = MOVE_NULL;
        td->stack[ply].movedPiece = NO_PIECE;

        board_do_null_move(pos);
        eval_state_push_null(td->es, pos);
        tt_prefetch(pos->key);
        const Value v = -negamax(td, pos, depth - r, -beta, -beta + 1, ply + 1, !cutNode);
        eval_state_pop(td->es);
        board_undo_null_move(pos);

        if (search_stopped())
            return VALUE_ZERO;

        /* A mate score proved by letting the opponent move twice is not a mate at all. Return
         * the bound the search is entitled to, not the claim. */
        if (v >= beta)
            return v >= VALUE_MATE_IN_MAX_PLY ? beta : v;
    }

    /*
     * ProbCut. The margin's declaration has the idea; what matters here is that quiescence
     * runs first, because it refutes most candidates for the price of one capture sequence
     * and only what survives is worth a real search.
     *
     * Skipped while verifying a singular move - this ignores `excluded`, so it could prove a
     * fail high with the very move being hidden - and skipped when the table already says
     * from a comparable depth that the bound is not reached. Skipped, too, when staticEval
     * is already past the raised bound: the SEE filter's threshold would go negative and
     * admit every capture on the board.
     */
    /* The gates that do not depend on the margin are tested first, so a node that could
     * not probcut whatever the margin came to never reads the uncertainty head for it. */
    const bool probCutGate =
        !pvNode && !inCheck && !isExcluded && depth >= PROBCUT_DEPTH && !is_mate_score(beta);
    const Value probCutBeta =
        probCutGate
            ? beta + PROBCUT_MARGIN *
                         unc_apply(unc_get(&uncScale, errSink, td, pos), UNC_W_PROBCUT) / 100
            : VALUE_NONE;

    if (probCutGate && !is_mate_score(probCutBeta) && staticEval < probCutBeta &&
        !(ttValue != VALUE_NONE && tt_entry_depth(&tte) >= depth - 3 && ttValue < probCutBeta)) {
        ScoredMove pcMoves[MAX_MOVES];
        const int pcCount = movegen_generate(pos, GEN_CAPTURES, pcMoves);
        MP_ADD(td, genCalls[4], 1);
        MP_ADD(td, generated[4], pcCount);
        MP_ADD(td, scored[4], pcCount);

        /* No counter-move: the list is captures, which the quiet heuristics have no opinion
         * about. The capture has to reach the raised bound on material alone - one needing
         * the search to find compensation is not what this is looking for. */
        pick_first(pcMoves, score_moves(td, pos, pcMoves, pcCount, ttMove, ply, MOVE_NONE));

        for (int i = 0; i < pcCount; ++i) {
            if (i > 0)
                pick_move(pcMoves, pcCount, i);
            const Move m = pcMoves[i].m;

            MP_ADD(td, pruningSee, 1);
            if (!see_ge(pos, m, probCutBeta - staticEval))
                continue;

            if (!movegen_is_legal(pos, m))
                continue;

            td->stack[ply].move       = m;
            td->stack[ply].movedPiece = piece_on(pos, from_sq(m));

            board_do_move(pos, m);
            eval_state_push(td->es, pos, m);
            tt_prefetch(pos->key);

            Value v = -qsearch(td, pos, -probCutBeta, -probCutBeta + 1, ply + 1);
            if (v >= probCutBeta)
                v = -negamax(td, pos, depth - PROBCUT_REDUCTION, -probCutBeta, -probCutBeta + 1,
                             ply + 1, !cutNode);

            eval_state_pop(td->es);
            board_undo_move(pos, m);

            if (search_stopped())
                return VALUE_ZERO;

            if (v >= probCutBeta) {
                /* Stored at the depth the evidence actually covers, so a later probe cannot
                 * read it as a full-depth result. */
                tt_store(key, m, v, rawEval, depth - PROBCUT_REDUCTION + 1, BOUND_LOWER, ttPv, ply);
                return v;
            }
        }
    }

    MovePicker picker;
    picker_init(&picker, td, pos, ttMove, excluded, ply, counter_move(td, ply));

    /* Moves already tried here, so the one that eventually cuts can penalise them. Bounded:
     * a node with more than this many is one where the ordering statistics were not going to
     * be decisive anyway. */
    Move quiets[64];
    int quietPawnMaluses[64];
    int quietCount = 0;
    Move captures[32];
    int captureCount = 0;

    /* The same continuation context score_moves used, kept for the pruning and reduction
     * decisions below: a quiet move the plan-aware tables like deserves the benefit of the
     * doubt that its position in the list denies it. */
    int16_t *slices[CONT_SLOTS];
    for (int i = 0; i < CONT_SLOTS; ++i)
        slices[i] = cont_slice(td, i, ply, ContPlies[i]);

    Value best    = -VALUE_INFINITE;
    Move bestMove = MOVE_NONE;
    int moveCount = 0;

    /* Tracked separately from `bestMove`, and the distinction is the whole difference
     * between an exact score and an upper bound. Every node ends up with a best-scoring
     * move, including one that failed low, so using `bestMove != MOVE_NONE` would mark every
     * fail-low entry BOUND_EXACT and a later probe would cut on it. */
    bool raisedAlpha = false;

    for (Move m; (m = picker_next(&picker, td, pos, ply)) != MOVE_NONE;) {
        MP_ADD(td, mainPicked, 1);
        if (!movegen_is_legal(pos, m))
            continue;
        MP_ADD(td, mainLegal, 1);
        ++moveCount;

        const bool tactical = is_tactical(pos, m);
        const Piece moved   = piece_on(pos, from_sq(m));

        /* How the plan-aware tables rate this move. Only meaningful for quiet moves - the
         * tactical ones are scored by what they win. */
        const int contScore = tactical ? 0 : cont_score(slices, moved, to_sq(m));
        const int pawnScore = tactical ? 0 : pawn_history_score(td, pos->pawnKey, moved, to_sq(m));

        /* Shallow-depth pruning of quiet moves, guarded on `best` beating a forced mate:
         * until the node has found something that is not losing outright, every remaining
         * move is a candidate escape. */
        if (!pvNode && !inCheck && best > VALUE_MATED_IN_MAX_PLY && !tactical) {
            /* Late move pruning: the list is ordered, so once this many quiets have been
             * tried without raising alpha the rest overwhelmingly will not. Unlike a
             * reduction this does not re-search, which is why it is confined to low depths -
             * and half as many get a look when the position is not improving. */
            if (depth <= LMP_DEPTH && moveCount >= (improving ? LMP_BASE + depth * depth
                                                              : (LMP_BASE + depth * depth) / 2))
                continue;

            /* Futility pruning: the position is so far below alpha that a quiet move, which
             * wins no material by definition, cannot close the gap in the depth remaining.
             * This prunes the whole quiet TAIL rather than one move, since the test does not
             * depend on which move it is; captures keep being searched, which is the point. */
            if (depth <= FUTILITY_DEPTH && staticEval <= alpha &&
                staticEval + FUTILITY_MARGIN * depth *
                                 unc_apply(unc_get(&uncScale, errSink, td, pos), UNC_W_FUTILITY) /
                                 100 <=
                    alpha)
                continue;
        }

        /*
         * SEE pruning: the move loses material outright and there is not enough depth left
         * for whatever it was playing for to appear. This is the one rule that applies to
         * captures as well as quiets, and that is why it is worth having - nothing above
         * touches a capture, so a piece thrown onto a defended square is otherwise searched
         * at full width.
         *
         * The thresholds scale differently on purpose: a capture that loses a little is
         * often a real sacrifice, so its allowance grows linearly with the depth left to
         * justify it, while a quiet move that hangs material has won nothing to weigh
         * against the loss.
         */
        if (!pvNode && !inCheck && best > VALUE_MATED_IN_MAX_PLY) {
            const Depth seeDepth = tactical ? SEE_CAPTURE_DEPTH : SEE_QUIET_DEPTH;

            /* The margin is computed under the depth guard rather than beside it, and that is
             * a range check: the quiet threshold is quadratic in depth, and at the sweep
             * bounds 120 * 246 * 246 * 500 does not fit in the int a Value is. Identical
             * pruning either way - see_ge() was never reached when the guard was false. */
            if (depth <= seeDepth) {
                const Value seeMargin =
                    tactical
                        ? -SEE_CAPTURE_MARGIN * depth *
                              unc_apply(unc_get(&uncScale, errSink, td, pos), UNC_W_SEE_CAPTURE) /
                              100
                        : -SEE_QUIET_MARGIN * depth * depth *
                              unc_apply(unc_get(&uncScale, errSink, td, pos), UNC_W_SEE_QUIET) /
                              100;

                MP_ADD(td, pruningSee, 1);
                if (!see_ge(pos, m, seeMargin))
                    continue;
            }
        }

        if (tactical && captureCount < (int)(sizeof(captures) / sizeof(captures[0]))) {
            captures[captureCount++] = m;
        }

        Depth extension = 0;

        /*
         * Singular extension. The table says this move was best here, from a search nearly as
         * deep as this one, and the question worth asking is not whether it is good but
         * whether it is the ONLY move that is - a forced line is where an extra ply buys the
         * most, because the branching that makes depth expensive is not there.
         *
         * One search of every OTHER move against a window just below the stored score gives
         * three outcomes: all fail low, so extend; the reduced search beats the real beta, so
         * several moves are good enough and this node fails high outright (multi-cut); or the
         * others reach the window while the table move already beats beta, so the node is
         * over-searched and plies come off.
         *
         * The verification runs at this same ply with `excludedMove` set, which is why the
         * flag is read-and-cleared on entry and why the table is neither trusted nor written
         * while it is set.
         */
        if (!isExcluded && depth >= SINGULAR_DEPTH && m == ttMove && ttValue != VALUE_NONE &&
            !is_mate_score(ttValue) && (tt_entry_bound(&tte) & BOUND_LOWER) &&
            tt_entry_depth(&tte) >= depth - 3) {
            const Value singularBeta =
                ttValue - SINGULAR_MARGIN * depth *
                              unc_apply(unc_get(&uncScale, errSink, td, pos), UNC_W_SINGULAR) /
                              100 / 16;
            const Depth singularDepth = (depth - 1) / 2;

            /* The verification searches this ply again and writes the PV table as it goes.
             * Nothing has been recorded here yet - the table move always sorts first - but
             * restoring it keeps that an observation rather than a dependency. */
            const int savedPvLength = td->pvLength[ply];

            td->stack[ply].excludedMove = m;
            const Value v =
                negamax(td, pos, singularDepth, singularBeta - 1, singularBeta, ply, cutNode);
            td->stack[ply].excludedMove = MOVE_NONE;

            td->pvLength[ply] = savedPvLength;

            if (search_stopped())
                return VALUE_ZERO;

            if (v < singularBeta)
                extension = 1;
            else if (singularBeta >= beta && !pvNode)
                return singularBeta;
            else if (ttValue >= beta)
                extension = -2;
        }

        td->stack[ply].move       = m;
        td->stack[ply].movedPiece = moved;

        board_do_move(pos, m);
        eval_state_push(td->es, pos, m);
        tt_prefetch(pos->key);

        const bool givesCheck = board_checkers(pos) != BB_EMPTY;

        /* Check extension: a check is the most forcing move in chess - the reply set is tiny
         * and often single - so the branch costs little and the tactics that decide games
         * live there. Bounded by ply so a perpetual cannot extend forever, and never stacked
         * on a singular extension: one ply is the answer to "this line is forced", however
         * many reasons there are to think so. */
        if (extension == 0 && givesCheck && ply < 2 * td->rootDepth)
            extension = 1;

        const Depth childDepth = depth - 1 + extension;
        MP_ADD(td, mainSearched, 1);
        Value v = VALUE_NONE;

        /* LMR retries reduced fail-highs at normal depth, but a false fail-low can still
         * hide a move. Forcing moves are exempt; failed-move learning below distinguishes
         * a reduced-only rejection from one that received a normal-depth search. */
        Depth r = 0;
        if (depth >= 3 && moveCount > 2 && !tactical && !inCheck && !givesCheck) {
            r = Reductions[imin(depth, 63)][imin(moveCount, 63)];

            /* The principal variation is where accuracy is worth paying for, and a position
             * that was on one before is still that position whatever window this visit uses.
             * Two arms rather than `if (ttPv) --r` so an ablation can switch the second off. */
            if (pvNode)
                --r;
            else if (ttPv)
                r -= TTPV_REDUCTION;

            /* A position that is not improving is one where the search has less to lose by
             * looking at the tail more cheaply. */
            if (!improving)
                ++r;

            /* Add structure-specific history BEFORE dividing: a context entry need not
             * earn a whole ply by itself to influence the existing history adjustment. */
            r -= (td->history[us][from_sq(m)][to_sq(m)] + pawnScore) / LMR_HIST_DIVISOR;
            r -= contScore / LMR_CONT_DIVISOR;

            if (r < 0)
                r = 0;
            else if (r > childDepth - 1)
                r = childDepth - 1;
        }

        /* Principal variation search: the first move is searched with the full window, and for
         * the rest the only question worth asking is whether anything BEATS it, which a null
         * window answers far more cheaply. A reduced null-window search is a bet that the
         * move fails low, so the child is by definition expected to fail high. */
        bool fullDepthSearch = r == 0;
        if (r > 0) {
            v = -negamax(td, pos, childDepth - r, -alpha - 1, -alpha, ply + 1, true);
            if (v > alpha) {
                fullDepthSearch = true;
                v = -negamax(td, pos, childDepth, -alpha - 1, -alpha, ply + 1, !cutNode);
            }
        } else if (!pvNode || moveCount > 1) {
            v = -negamax(td, pos, childDepth, -alpha - 1, -alpha, ply + 1, !cutNode);
        }

        /* A full-window search is never a cut node: the whole point is that its value is
         * wanted exactly, not as a bound. */
        if (pvNode && (moveCount == 1 || (v > alpha && v < beta))) {
            fullDepthSearch = true;
            v               = -negamax(td, pos, childDepth, -beta, -alpha, ply + 1, false);
        }

        eval_state_pop(td->es);
        board_undo_move(pos, m);

        if (search_stopped())
            return VALUE_ZERO;

        /* Only completed attempts enter the list. Global histories retain the original
         * malus; the optional pawn malus remembers how much evidence was requested. */
        if (!tactical && quietCount < (int)(sizeof(quiets) / sizeof(quiets[0]))) {
            quiets[quietCount] = m;
            if (PAWN_HIST_WEIGHT != 0 && PAWN_EVIDENCE_WEIGHT != 0) {
                const Depth evidence =
                    history_pawn_evidence_depth(depth, childDepth, r, fullDepthSearch);
                quietPawnMaluses[quietCount] =
                    history_pawn_evidence_malus(history_malus(depth), history_malus(evidence),
                                                isExcluded ? 0 : PAWN_EVIDENCE_WEIGHT);
            }
            ++quietCount;
        }

        if (v > best) {
            best     = v;
            bestMove = m;
            if (v > alpha) {
                alpha       = v;
                raisedAlpha = true;
                update_pv(td, ply, m);
                if (v >= beta) {
                    /* Fail high: the opponent would avoid this line. The move that cut is
                     * credited in whichever table describes it, and everything tried before it
                     * is blamed in both. */
                    /* The only consumer that wants the signal rather than the scale, and
                     * the one place the deferred read has to be forced. Same number
                     * whenever it is taken. */
                    int pawnExtraCredit = 0;
                    if (PAWN_HIST_WEIGHT != 0 && !tactical) {
                        unc_get(&uncScale, errSink, td, pos);
                        pawnExtraCredit = history_pawn_rescue_credit(
                            history_bonus(depth), staticEval, beta, v, evalError, isExcluded,
                            PAWN_RESCUE_WEIGHT, PAWN_RESCUE_FLOOR);
                    }
                    update_stats(td, pos, m, quiets, quietPawnMaluses, quietCount, captures,
                                 captureCount, depth, ply, pawnExtraCredit);
#ifdef MOVE_PICKER_PROFILE
                    const int stage = inCheck                                ? 6
                                      : m == ttMove                          ? 0
                                      : picker.stage == PICK_GOOD_TACTICALS  ? 1
                                      : picker.stage == PICK_GENERATE_QUIETS ? 3
                                      : picker.stage == PICK_QUIETS          ? 4
                                      : picker.stage == PICK_BAD_TACTICALS   ? 5
                                                                             : 2;
                    ++td->moveProfile.cutoffs[stage];
#endif
                    break;
                }
            }
        }
    }

    /* No legal move at all: mate if the king is attacked, stalemate if not, scored by ply so
     * the engine prefers the faster mate. Unless a move was excluded, in which case "no
     * moves" means the position has exactly one - the most singular a move can be - so
     * return alpha and let the caller read a fail low and extend. */
    if (moveCount == 0) {
        if (isExcluded)
            return alpha;

        best = inCheck ? mated_in(ply) : VALUE_DRAW;
        tt_store(key, MOVE_NONE, best, rawEval, depth, BOUND_EXACT, ttPv, ply);
        return best;
    }

    /*
     * A move that raised alpha without failing high proves an exact score, because every
     * alternative was searched and none beat it. At a null-window node that cannot happen,
     * so only PV nodes ever store an exact entry. `bestMove` is still stored on a fail low -
     * it is a hint about what to try first next time, which costs nothing to be wrong about.
     *
     * Never from a singular verification: its move list was missing a move, so its score is
     * not a fact about this position and must not be cached as one.
     */
    if (!isExcluded) {
        const Bound bound = best >= beta ? BOUND_LOWER : raisedAlpha ? BOUND_EXACT : BOUND_UPPER;
        tt_store(key, bestMove, best, rawEval, depth, bound, ttPv, ply);

        /* Compiled out entirely unless UNC_PROBE; see test/uncprobe.h. */
        unc_probe_node(td, pos, best, staticEval, bound, depth);

        /*
         * Learn from this node only where the search genuinely contradicted the static
         * evaluation, on the static evaluation's own terms. In check there is nothing to be
         * wrong about, and a proven score is a different kind of fact - the test must be
         * is_decisive_score(), because a tablebase score would otherwise be fed in as an
         * ~8,000,000cp "error" that saturates the entry on one observation.
         *
         * A tactical best move means the gap was material quiescence found rather than a
         * standing bias, and a bound is evidence only in the direction it bounds.
         */
        if (!inCheck && !is_decisive_score(best) &&
            (bestMove == MOVE_NONE || !is_tactical(pos, bestMove)) &&
            !(bound == BOUND_LOWER && best <= staticEval) &&
            !(bound == BOUND_UPPER && best >= staticEval))
            corrhist_update(td, pos, best, staticEval, depth);
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

/* Insertion sort by descending score. Stable, so equal-scoring moves keep their generation
 * order and the node count stays reproducible. */
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

/* One iteration of the root search. Written out rather than folded into negamax because
 * the root is the only place that has to honour `searchmoves`, keep its move list alive
 * between iterations so it can be reordered, and score every move rather than cutting off.
 * Returns VALUE_NONE if the iteration was interrupted, in which case its partial result
 * must be discarded. */
static Value search_root(SearchThread *td, Position *pos, ScoredMove *roots, int count, Depth depth,
                         Value alpha, Value beta, Move *bestMove) {
    Value best = -VALUE_INFINITE;

    td->pvLength[0] = 0;

    for (int i = 0; i < count; ++i) {
        const Move m = roots[i].m;

        td->stack[0].move       = m;
        td->stack[0].movedPiece = piece_on(pos, from_sq(m));
        td->stack[0].staticEval = VALUE_NONE;

        board_do_move(pos, m);
        eval_state_push(td->es, pos, m);
        tt_prefetch(pos->key);

        Value v;
        if (i == 0) {
            v = -negamax(td, pos, depth - 1, -beta, -alpha, 1, false);
        } else {
            /* As in negamax: the first root move establishes alpha with the full window, and
             * every later one only has to answer whether it beats that. The ones that do -
             * rare, once the list is sorted by the previous iteration - pay for a re-search,
             * and the null-window expectation is that the child fails high on its own terms. */
            v = -negamax(td, pos, depth - 1, -alpha - 1, -alpha, 1, true);
            if (v > alpha && v < beta && !search_stopped())
                v = -negamax(td, pos, depth - 1, -beta, -alpha, 1, false);
        }

        eval_state_pop(td->es);
        board_undo_move(pos, m);

        if (search_stopped())
            return VALUE_NONE;

        roots[i].score = (int)v;

        if (v > best) {
            best      = v;
            *bestMove = m;
            update_pv(td, 0, m);

            if (v > alpha)
                alpha = v;

            /* Beat the aspiration window: the caller has to widen and retry, so there is
             * nothing to gain from searching the rest against a bound already known wrong. */
            if (v >= beta)
                break;
        }
    }

    return best;
}

/* Plies-to-mate converted to the signed move count UCI wants. */
static int mate_in_moves(Value v) {
    return v > 0 ? (VALUE_MATE - v + 1) / 2 : -((VALUE_MATE + v + 1) / 2);
}

/*
 * What a proven tablebase result is reported as, in centipawns.
 *
 * The internal band sits directly below the mate scores, so printing one raw gives
 * `score cp 31753` - a number that means nothing to whoever is reading it and looks like
 * the engine has lost its mind. Two hundred pawns is clear of anything the evaluation
 * produces and clear of the mate band, which is what "won, no mate in sight yet" should
 * look like; it is the convention Stockfish reports tablebase scores in, so GUIs already
 * display it the way people expect.
 */
enum { TB_REPORT_CP = 20000 };

/* Keeping the ply offset the probe put on the score, so a conversion that is nearer still
 * reads as better than one further away. */
static int tb_in_cp(Value v) {
    const int ply = VALUE_TB_WIN - (v > 0 ? v : -v);
    return v > 0 ? TB_REPORT_CP - ply : ply - TB_REPORT_CP;
}

/*
 * The line is assembled in full and written once. main.c makes stdout unbuffered and the
 * UCI thread answers `isready` while this worker searches, so a sequence of printf calls
 * lets another thread's output land in the middle: the GUI sees
 * `info depth 12 seldepth 18 readyok`, which is neither a parsable info line nor a
 * readyok. One fputs of a complete line cannot be split that way.
 */
static void print_iteration(const SearchThread *td, Depth depth, Value value, int64_t elapsed,
                            bool chess960) {
    /* Cannot be outgrown: the fixed prefix is under 200 characters and the PV is at most
     * MAX_PLY moves of five characters plus a space. */
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

    /* The whole pool's nodes, not this thread's share: a GUI showing one thread's nps on a
     * 64-thread search is reporting a number that describes nothing. */
    const uint64_t nodes = nodes_including(td);

    /* Clamped so a sub-millisecond iteration cannot divide by zero. */
    const int64_t ms = elapsed > 0 ? elapsed : 1;
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

/*
 * Which iterations a helper thread skips.
 *
 * Lazy SMP only pays when the threads are looking at DIFFERENT things: N threads
 * running the same iteration in lockstep mostly re-derive each other's cutoffs and the
 * speedup collapses towards one. Each helper takes a phase and a period out of these
 * tables and sits out the iterations that fall the wrong side of it, so at any moment
 * the pool is spread over several depths and the table is being filled from several
 * distances at once. The pattern repeats every 20 helpers, which is why it needs no
 * relation to the thread count.
 *
 * The tables are Stockfish's, from the years its search used this scheme; they are
 * kept because they are known to work, not because these particular numbers were
 * derived here.
 */
/* clang-format off */
static const int SkipSize[]  = {1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4};
static const int SkipPhase[] = {0, 1, 0, 1, 2, 3, 0, 1, 2, 3, 4, 5, 0, 1, 2, 3, 4, 5, 6, 7};
/* clang-format on */

#define SKIP_PATTERNS ((int)(sizeof(SkipSize) / sizeof(SkipSize[0])))

/* One thread's iterative deepening over its own copy of the root. Thread 0 owns the
 * clock, the `info` lines and the aspiration schedule the engine is judged on; a helper
 * runs the same loop with the time management taken out and its own skip pattern in. */
static void thread_search(SearchThread *td) {
    Position *const pos = &td->rootPos;
    const bool isMain   = td->id == 0;

    /*
     * This thread's accumulator stack starts empty. It has to: a level keeps the key of
     * the position it describes and reuses its accumulator when the key matches, so a
     * level left over from the previous search would be reused verbatim if this search
     * starts from the same position - which is exactly what happens when `setoption name
     * EvalFile` swapped the net in between. One accumulation per search per thread is
     * nothing; a whole search scored by the net that was replaced is a silent loss.
     */
    eval_state_clear(td->es);

    td->bestMove       = MOVE_NONE;
    td->ponderMove     = MOVE_NONE;
    td->rootScore      = VALUE_NONE;
    td->completedDepth = 0;
    td->rootPvLength   = 0;

    if (isMain)
        timeman_init(&Timer, &Limits, pos->sideToMove, pos->gamePly);

    ScoredMove roots[MAX_MOVES];
    int rootCount = collect_root_moves(pos, roots);

    /* Checkmate, stalemate, or a `searchmoves` list with nothing legal in it. MOVE_NONE
     * prints as `bestmove 0000`, which is what GUIs expect. */
    if (rootCount == 0)
        return;

    Value tbRootValue = VALUE_NONE;

    /*
     * Syzygy at the root, which settles the move and the score together. WDL alone cannot
     * convert, because every winning move scores the same and a search free to choose among
     * them can shuffle until the fifty-move rule takes the win away; DTZ names one that
     * provably makes progress, and the root list is cut down to it so the PV, the info lines
     * and time management all still work.
     *
     * The score matters too: interior nodes only probe at halfmoveClock == 0, so the children
     * of a five-man root are searched heuristically and the tree would hand back an
     * evaluation. That is the difference between a labelled KNP-vs-KP position scoring 0 and
     * scoring +3.
     */
    if (TbLimit != 0 && Limits.searchmovesCount == 0) {
        const SyzygyRoot tb = syzygy_probe_root(pos);
        if (tb.value != VALUE_NONE) {
            ++td->tbHits;
            tbRootValue = tb.value;

            /* The one thing a centipawn score cannot carry: how far the win is from a
             * capture or a pawn move, and - when a won position scores as a draw anyway -
             * why. Printed once, because the root probe cannot change under the search,
             * and only by the thread that owns the output. */
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

    /* Something legal to play from the first moment, so a `stop` that arrives before any
     * iteration completes still produces a move rather than `bestmove 0000`. */
    td->bestMove = roots[0].m;

    const Depth maxDepth = Limits.depth > 0 && Limits.depth < MAX_PLY ? Limits.depth : MAX_PLY - 1;

    Value prevScore = VALUE_NONE;

    /* Consecutive completed iterations that agreed on the best move, feeding the time
     * manager. Tracked against its own previous value rather than against the best move,
     * which starts out holding an unsearched move. */
    int stability = 0;
    Move prevBest = MOVE_NONE;

    /* The helper's seat in the skip tables. Offset by the root's ply so two searches from
     * different positions in the same game do not hand every thread the same schedule. */
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

        /* Aspiration windows: the score at depth N is nearly always close to the score at
         * N-1, so a narrow window produces far more cutoffs, at the price of a re-search
         * whenever the score moves outside it. Widening geometrically bounds that price. */
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

        /* An interrupted iteration searched some moves under a window the others never saw,
         * so its ordering is meaningless: keep the last completed iteration's move. */
        if (search_stopped())
            break;

        stability = (prevBest != MOVE_NONE && iterationBest == prevBest) ? stability + 1 : 0;
        prevBest  = iterationBest;

        /* A proven result outranks the tree's opinion of it. `prevScore` keeps the tree's own
         * number so the next aspiration window is centred on something the tree can return,
         * while everything outward-facing reports the proof.
         *
         * A mate the tree has actually found is the exception, because it is the stronger
         * proof of the two: it names the distance, where the tablebase value only says
         * "won". The search knows the fifty-move rule, so a mate it returns is one that
         * really arrives before the counter does. Without this a mate in one under a
         * five-man root reports as "won by tablebase". */
        const Value reported =
            tbRootValue != VALUE_NONE && !is_mate_score(value) ? tbRootValue : value;

        prevScore          = value;
        td->bestMove       = iterationBest;
        td->rootScore      = reported;
        td->completedDepth = depth;

        td->rootPvLength = td->pvLength[0];
        memcpy(td->rootPv, td->pvTable[0], (size_t)td->rootPvLength * sizeof(Move));

        /* Cleared, not left alone, when this iteration has no second PV move: the stale entry
         * belongs to a line the search has abandoned, and pondering on a move that no longer
         * follows `best` wastes the ponder search and desynchronises the GUI on ponderhit. */
        td->ponderMove = td->rootPvLength > 1 ? td->rootPv[1] : MOVE_NONE;

        if (isMain && !Silent)
            print_iteration(td, depth, reported, elapsed_ms(), pos->chess960);
        sort_root_moves(roots, rootCount);

        if (!isMain)
            continue;

        if (Limits.mate && is_mate_score(value) && value > 0 && mate_in_moves(value) <= Limits.mate)
            break;

        /* Do not begin an iteration there is no realistic chance of finishing: each costs
         * several times the last, so starting one at 90% of the budget burns the remainder
         * and throws the result away. */
        if (!Limits.infinite && !atomic_load(&Pondering) &&
            elapsed_ms() >= timeman_optimum(&Timer, stability))
            break;
    }

    atomic_store(&td->publishedNodes, td->nodeCount);
    atomic_store(&td->publishedTbHits, td->tbHits);
}

/*
 * Whose move gets played.
 *
 * Deeper first, and on a tie the better score. Depth first because a thread that
 * completed depth 20 examined everything the depth-18 thread did and more; score second
 * because at equal depth the threads searched the same tree with different move orders,
 * and the one that found more was looking in a better place. A thread that never
 * completed an iteration has nothing to offer and is skipped outright.
 *
 * Stockfish weighs votes across threads instead, which is a real and different rule -
 * it protects against one thread's single deep fluke - and swapping this for it is a
 * change with its own SPRT, not a refactor.
 */
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

/* Everything a thread must forget between searches: a stale excluded move or grandparent
 * evaluation left here by the previous search would make this one depend on it. */
static void thread_prepare(SearchThread *td) {
    td->rootPos   = RootPos;
    td->nodeCount = 0;
    td->tbHits    = 0;
    td->selDepth  = 0;
#ifdef MOVE_PICKER_PROFILE
    memset(&td->moveProfile, 0, sizeof(td->moveProfile));
#endif

    atomic_store(&td->publishedNodes, 0);
    atomic_store(&td->publishedTbHits, 0);

    memset(td->stack, 0, sizeof(td->stack));
}

/*
 * Thread 0's work once its own iterations are done: hold if UCI says it must, stop the
 * helpers, wait for them, and announce the result.
 *
 * The order is load-bearing. StopFlag is what ends a helper's search, and it is also
 * what the hold below is waiting for, so setting it early would end the hold as well and
 * send `bestmove` during a `go infinite` - which UCI forbids and which GUIs report as a
 * lost game rather than as a protocol error.
 */
static void finish_search(void) {
    /* UCI forbids sending `bestmove` during a ponder or an infinite search: the GUI owns that
     * decision and will send `stop` or `ponderhit` first. Replying early desynchronises the
     * GUI and shows up as spurious losses. The helpers keep searching throughout, which is
     * the only useful thing anyone can do with the time. */
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

#ifdef MOVE_PICKER_PROFILE
    /* Helpers are parked before their counters are read. Per-position lines
     * keep bench's final OpenBench line untouched and can be summed offline. */
    static const char *const bands[] = {"tactical", "quiet", "evasion", "qsearch", "probcut"};
    for (int t = 0; t < ThreadCount; ++t) {
        const MoveProfile *p = &Threads[t]->moveProfile;
        printf("info string movepick nodes=%llu picked=%llu legal=%llu searched=%llu "
               "orderingSEE=%llu pruningSEE=%llu\n",
               (unsigned long long)p->mainNodes, (unsigned long long)p->mainPicked,
               (unsigned long long)p->mainLegal, (unsigned long long)p->mainSearched,
               (unsigned long long)p->orderingSee, (unsigned long long)p->pruningSee);
        for (int i = 0; i < 5; ++i)
            printf("info string movepick %s calls=%llu generated=%llu scored=%llu\n", bands[i],
                   (unsigned long long)p->genCalls[i], (unsigned long long)p->generated[i],
                   (unsigned long long)p->scored[i]);
        printf("info string movepick cuts tt=%llu good=%llu killer=%llu counter=%llu "
               "quiet=%llu bad=%llu evasion=%llu\n",
               (unsigned long long)p->cutoffs[0], (unsigned long long)p->cutoffs[1],
               (unsigned long long)p->cutoffs[2], (unsigned long long)p->cutoffs[3],
               (unsigned long long)p->cutoffs[4], (unsigned long long)p->cutoffs[5],
               (unsigned long long)p->cutoffs[6]);
    }
#endif

    /* A GUI's last `info` line is where its evaluation display comes from, and it has
     * been thread 0's all search. When somebody else's iteration wins, saying so is the
     * difference between a coherent report and a PV that does not start with the move
     * the engine just played. */
    if (best->id != 0 && best->completedDepth > 0 && !Silent)
        print_iteration(best, best->completedDepth, best->rootScore, elapsed_ms(),
                        RootPos.chess960);

    atomic_store(&Searching, false);
    uci_print_bestmove(best->bestMove, best->ponderMove);
}

/* A pooled thread's whole life: park, search, park again. Created once when `Threads`
 * is set and joined only when it changes or the engine exits. */
static void thread_entry(void *arg) {
    SearchThread *const td = (SearchThread *)arg;

    thread_bind(td->id, ThreadCount);

    /* Without one the evaluation is still correct and several times slower, so it is
     * worth saying which thread is running that way rather than leaving an unexplained
     * collapse in nps. */
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

        /* Cleared here and SET by whoever started the search, which is the whole
         * handshake: a thread that marked itself busy on waking would leave a window
         * in which the starter has already returned and a waiter sees an idle pool
         * that has not begun. That window is not theoretical - it makes `bench` read
         * a node count of zero and start the next position on top of this one. */
        mutex_lock(&ThreadMutex);
        td->searching = false;
        cond_broadcast(&ThreadCv);
        mutex_unlock(&ThreadMutex);
    }

    eval_state_free();
}

/* Blocks until no pooled thread is searching. `bestmove` is printed by thread 0 before
 * it clears its own flag, so a caller that returns from here has seen it. */
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

/*
 * Allocates the pool's blocks. Every one is about 8 MB, so this is where a `Threads`
 * setting is paid for - once, rather than on every `go`, which is the whole reason the
 * threads are parked between searches instead of created per search.
 *
 * A block that cannot be allocated ends the pool where it is: the engine runs with fewer
 * threads than asked rather than failing to start a search. It never ends with none,
 * because thread 0's block is the one a synchronous search runs in too.
 */
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

/*
 * Starts whatever is not running yet, and answers whether anything is.
 *
 * Separate from the allocation above, and that separation is the whole point: a process
 * that only ever runs SYNCHRONOUS searches then never gains a second thread at all.
 * tools/datagen.c is that process, and it forks - and fork() in a multithreaded program
 * is a far narrower contract than in a single-threaded one, for a thread it was never
 * going to use.
 */
static bool pool_start(void) {
    for (int i = 0; i < ThreadCount; ++i) {
        SearchThread *const td = Threads[i];

        if (!td->started)
            td->started = thread_create(&td->handle, thread_entry, td);

        if (td->started)
            continue;

        /* Everything above this index is unstarted too, since they are started in order,
         * so the pool becomes what did start. Thread 0's BLOCK is kept whether or not its
         * thread was: it is where an inline search runs, and where a synchronous one
         * always runs. */
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

    /* Started here rather than at the first `go`: a GUI that asks for 128 threads should
     * pay for them where it asked, not in the middle of the first move's clock. */
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

/* Common to both entry points: everything that must be true before the threads run. */
static void search_setup(const Position *pos, const SearchLimits *limits, bool silent) {
    RootPos = *pos;
    Limits  = *limits;
    Silent  = silent;

    /* Read once here rather than per node, and once for the whole pool: a `SyzygyPath`
     * that changed mid-search would otherwise mean two threads disagreeing about how many
     * pieces a probe is legal at. */
    TbLimit = syzygy_max_pieces();

    atomic_store(&StopFlag, false);
    atomic_store(&ClockOrigin, limits->startTime);
    atomic_store(&Searching, true);

    /* Here rather than inside thread 0's iterations, which is where it used to be: the
     * helpers are woken at the same moment thread 0 is, so a bump made from inside the
     * search is a bump made while the rest of the pool is already storing. Those entries
     * get stamped with the previous generation and are born one search stale, which puts
     * the helpers' contribution - the entire point of Lazy SMP - first in line for
     * replacement. Nothing is running yet at this point in the setup. */
    tt_new_search();

    for (int i = 0; i < ThreadCount; ++i)
        thread_prepare(Threads[i]);
}

void search_start(const Position *pos, const SearchLimits *limits) {
    /* Only one search at a time, so the previous one has to be finished - but waiting is not
     * enough on its own. A `go infinite` search parks until StopFlag is set, so a bare wait
     * would block the UCI thread inside `go` while the `stop` that would release it can only
     * arrive on that same thread. Asking it to stop first makes the wait bounded. */
    search_stop();
    search_wait();

    if (ThreadCount == 0) {
        /* Not one block could be allocated, so there is nowhere to search. Answering the
         * protocol is all that is left - a GUI that gets no `bestmove` at all hangs until
         * it times the engine out, which hides the reason. */
        printf("info string no search thread could be allocated; cannot search\n");
        atomic_store(&Searching, false);
        uci_print_bestmove(MOVE_NONE, MOVE_NONE);
        return;
    }

    search_setup(pos, limits, false);
    atomic_store(&Pondering, limits->ponder);

    /* The first `go` of a session is where the default pool is actually created. One
     * thread costs tens of microseconds; a pool the GUI asked for was started when it
     * asked. */
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
        /* No pooled thread to run it, so the UCI thread searches inline. It still produces a
         * legal game - it just cannot be interrupted - which beats not moving at all. The
         * ponder/infinite hold has to be dropped with it: that loop waits for a `stop` only
         * the UCI thread can deliver, and the UCI thread is the one about to search. */
        Limits.infinite = false;
        atomic_store(&Pondering, false);

        thread_search(Threads[0]);
        atomic_store(&StopFlag, true);
        atomic_store(&Searching, false);
        uci_print_bestmove(Threads[0]->bestMove, Threads[0]->ponderMove);
    }
}

void search_run_sync(const Position *pos, const SearchLimits *limits, SearchResult *out) {
    /* Same bounded-wait reasoning as search_start(). */
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

    /* A synchronous ponder search would wait for a `stop` that no one is around to send, so
     * the flag is dropped rather than honoured. */
    atomic_store(&Pondering, false);

    /*
     * One thread, on the caller's own stack, whatever `Threads` says. Everything that calls
     * this - datagen above all - wants a search that is a function of its position, its seed
     * and its node limit and of nothing else, and a parallel search is not: the pool reaches
     * the shared table in an order the operating system chooses, so the same position would
     * label differently on two runs of the same shard.
     */
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
    /* Only a pondering search may have its clock restarted. Without this guard a stray
     * `ponderhit` during an ordinary timed search resets the origin every elapsed-time test
     * is measured against, handing the search a second full budget. */
    if (!atomic_load(&Pondering))
        return;

    /* The opponent played our predicted move, so the ponder search becomes a real one and the
     * clock is now ours. Only the origin moves - the budget was derived from the clock the
     * GUI reported, which has not changed. */

    atomic_store(&ClockOrigin, time_ms());
    atomic_store(&Pondering, false);
}

void search_wait(void) { pool_wait(); }
