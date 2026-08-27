/*
 * search.c - search driver and worker thread.
 *
 * Iterative deepening over a principal variation search, with a quiescence
 * search at the leaves.
 *
 * Almost everything here exists to make alpha-beta see the best move first.
 * That is not a detail: alpha-beta only approaches its theoretical node count
 * under perfect ordering, and the gap between good and bad ordering is a
 * factor of ten in tree size long before it is anything else. In rough order
 * of how much they contribute:
 *
 *   - the transposition table move, which was actually best last time;
 *   - MVV-LVA on captures and promotions;
 *   - killers, counter-moves and butterfly history on the quiet moves.
 *
 * With the ordering good enough to rely on, two prunings pay for themselves:
 * null-move pruning, which skips positions too good to need searching, and
 * late move reductions, which search the tail of the move list shallower and
 * re-search anything that surprises us.
 *
 * Anything added from here is a BEHAVIOURAL change and must not be committed
 * without a passing SPRT. See docs/TESTING.md; roughly half of the patches
 * that look obviously good measure neutral or worse.
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

/* What is being searched at each ply. The child needs to know the move that
 * led to it - to look up its counter-move, and to refuse a second null move in
 * a row - and the piece that made it, which the board no longer says once the
 * move has been played. */
typedef struct {
    Move move;
    Piece movedPiece;
    Value staticEval; /* kept so a node can compare against its grandparent */

    /* Set only while a singular search is running at this ply: the move that
     * search must pretend does not exist. See the singular extension. */
    Move excludedMove;
} SearchStack;

static SearchStack Stack[MAX_PLY];

/* Nominal depth of the iteration currently running. Extensions are bounded
 * relative to it, so a line that can be extended indefinitely - a perpetual
 * check, say - cannot grow the tree without limit. */
static Depth RootDepth;

/* Score and depth of the last iteration that ran to completion, published for
 * search_run_sync. The UCI path reads the same numbers off the `info` lines,
 * which is why nothing but an offline tool ever needed them exposed. */
static Value RootScore;
static Depth CompletedDepth;

/* Suppresses the `info` lines. Set only for the duration of a synchronous
 * search: datagen runs millions of them and wants its own stdout. */
static bool Silent;

/* Deepest ply any line reached this iteration, quiescence included. Reported
 * as `info seldepth`, which is how a GUI tells a search that is genuinely
 * looking deep along forcing lines from one that is merely reporting a big
 * nominal depth after heavy reductions. */
static int SelDepth;

static inline void update_seldepth(int ply) {
    if (ply > SelDepth)
        SelDepth = ply;
}

/*
 * Late move reduction amounts, indexed by [depth][move number] and built once
 * by init_reductions().
 */
static uint8_t Reductions[64][64];

/*
 * The ordering heuristics. Declared here rather than beside the code that uses
 * them because search_clear() has to reset every one: anything that carries
 * information from one search into the next makes a result depend on what was
 * searched before it, which quietly destroys reproducibility. See the move
 * ordering section for what each of them means.
 */
static Move Killers[MAX_PLY][2];
static int16_t History[COLOR_NB][SQUARE_NB][SQUARE_NB];
static Move CounterMoves[PIECE_NB][SQUARE_NB];

/*
 * Continuation history, indexed [slot][previous piece][previous to][this
 * piece][this to]. 2 MB per slot, which is why these are file-scope arrays
 * rather than anything per-node. See the note beside their use for what they
 * buy.
 *
 * Three slots, keyed on the move played one, two and four plies above. One ply
 * back is the direct reply and the strongest single signal. Two plies back is
 * the same side's own previous move, which is what makes a plan legible to the
 * ordering - "knight to d2 then knight to f1" scores as a unit rather than as
 * two unrelated moves. Four plies back catches the slower manoeuvres that a
 * two-ply window still reads as noise.
 */
#define CONT_SLOTS 3

/* How far back each slot looks. Sized independently of CONT_SLOTS so that
 * lowering the slot count is a one-flag change; only the first CONT_SLOTS
 * entries are ever read. */
static const int ContPlies[3] = {1, 2, 4};

static int16_t ContHist[CONT_SLOTS][PIECE_NB][SQUARE_NB][PIECE_NB][SQUARE_NB];

/*
 * Capture history, indexed [moving piece][to][captured type].
 *
 * MVV-LVA says what a capture takes and SEE says what it wins, and between
 * them they still cannot tell two equal-looking captures apart. This does, on
 * the same evidence the quiet history runs on: whether the capture has been
 * producing cutoffs lately. It is what orders the exchanges that SEE calls
 * level, which in a sharp middlegame is most of them.
 */
static int16_t CaptureHist[PIECE_NB][SQUARE_NB][PIECE_TYPE_NB];

/*
 * Correction history. See the section just above quiescence for what these
 * hold; what follows is only which structures they are keyed by.
 *
 * Four families, all learning the same quantity - how far the static
 * evaluation has been running from what the search actually returned - keyed
 * by four different descriptions of "positions like this one". Pawn structure
 * was the first and is measured (E14); the other three exist because pawn
 * structure is not the only thing an evaluation is systematically wrong about.
 */
#define CORRHIST_SIZE       16384                 /* power of two; the index is a mask */
#define CORRHIST_GRAIN      256                   /* fixed point, so sub-pawn drift survives */
#define CORRHIST_LIMIT      (CORRHIST_GRAIN * 32) /* at most 32cp from any one table */
#define CORRHIST_WEIGHT_MAX 256                   /* denominator of the moving average */

/* Ceiling on the four summed. Deliberately not four times CORRHIST_LIMIT: the
 * tables are keyed differently but fitted to the same residual, so when they
 * agree they are largely restating one another's evidence rather than adding
 * to it.
 *
 * It sits at CORRHIST_LIMIT - the single-table bound E14 shipped - rather than
 * above it, because the first configuration tried was above it and measured
 * worse. At 48cp the correction could exceed anything the proven pawn-only
 * table could produce, the bench rose 16.8% against a baseline the same change
 * had previously moved 7% the other way, and 914 games read -6.84 +/- 15.42.
 * The reading was never decisive, but nothing about it argued for keeping the
 * larger bound. See E16. */
#define CORRHIST_TOTAL_LIMIT CORRHIST_LIMIT

static int16_t PawnCorrHist[COLOR_NB][CORRHIST_SIZE];
static int16_t MinorCorrHist[COLOR_NB][CORRHIST_SIZE];

/* [side to move][whose material][key]. Indexed by colour twice on purpose: the
 * bias in how White's pieces are priced is a different fact from the bias in
 * how Black's are, and which side is on move changes what either is worth. */
static int16_t NonPawnCorrHist[COLOR_NB][COLOR_NB][CORRHIST_SIZE];

/* Keyed on the move that led here, exactly as continuation history is. The
 * structure it names is "positions arrived at by playing this piece to this
 * square", which catches the biases the three static keys cannot see - a
 * sacrifice whose compensation the evaluation never scores, most of all. No
 * side-to-move index: the previous move's piece already carries its colour. */
static int16_t ContCorrHist[PIECE_NB][SQUARE_NB];

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int iclamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

#ifdef DATAGEN
static SearchNodeVisitor NodeVisitor;
static void *NodeVisitorCtx;

void search_set_node_visitor(SearchNodeVisitor fn, void *ctx) {
    NodeVisitor    = fn;
    NodeVisitorCtx = ctx;
}
#endif

/* Nodes between clock checks. Fine enough that a search cannot overrun its
 * budget by more than a millisecond or two, coarse enough that the clock read
 * does not show up in a profile. Must be a power of two. */
#define CHECK_INTERVAL 2048

void search_limits_clear(SearchLimits *limits) {
    memset(limits, 0, sizeof(*limits));
    limits->startTime = time_ms();
}

/* round(ln(i) * 1024) for i in 0..63, with ln(0) folded to 0 so index 0 is
 * safe. Hard-coded rather than computed with libm on purpose: log() is allowed
 * to differ by an ULP between platforms, and a single ULP either side of an
 * integer boundary changes a reduction, which changes the tree, which breaks
 * the cross-machine bench determinism OpenBench requires. */
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

/*
 * Reduction ~ log(depth) * log(moveNumber) / 2.4, in exact integer arithmetic.
 *
 * Both logarithms matter. Scaling with move number is the whole idea - the
 * twentieth move tried is far less likely to be best than the fourth. Scaling
 * with depth is what stops the reduction being reckless near the leaves, where
 * there is no depth left to absorb a mistake.
 */
static void init_reductions(void) {
    for (int d = 0; d < 64; ++d)
        for (int m = 0; m < 64; ++m)
            Reductions[d][m] =
                (uint8_t)((int64_t)LogFixed[d] * LogFixed[m] * 10 / (1024LL * 1024 * 24));
}

void search_init(void) {
    atomic_store(&Searching, false);
    atomic_store(&StopFlag, false);
    atomic_store(&Pondering, false);
    atomic_store(&Nodes, 0);
    atomic_store(&ClockOrigin, 0);
    init_reductions();
    board_set_startpos(&RootPos);
}

void search_clear(void) {
    tt_clear();
    memset(Killers, 0, sizeof(Killers));
    memset(History, 0, sizeof(History));
    memset(CounterMoves, 0, sizeof(CounterMoves));
    memset(ContHist, 0, sizeof(ContHist));
    memset(CaptureHist, 0, sizeof(CaptureHist));
    memset(PawnCorrHist, 0, sizeof(PawnCorrHist));
    memset(MinorCorrHist, 0, sizeof(MinorCorrHist));
    memset(NonPawnCorrHist, 0, sizeof(NonPawnCorrHist));
    memset(ContCorrHist, 0, sizeof(ContCorrHist));
    memset(Stack, 0, sizeof(Stack));
    eval_state_clear();
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

/*
 * Ordering bands, far enough apart that nothing scored within one band can be
 * promoted past a band above it.
 *
 *   TT move > winning captures and promotions > killers > counter-move
 *           > quiets by history > losing captures
 *
 * The transposition table move is the best-informed guess available - it was
 * actually best last time this position was searched, usually to a greater
 * depth - so nothing generated locally should displace it.
 */
#define SCORE_TT       (1 << 24)
#define SCORE_CAPTURE  1000000
#define SCORE_KILLER_1 900000
#define SCORE_KILLER_2 800000
#define SCORE_COUNTER  700000

/* Below every quiet move, since history is bounded by HISTORY_MAX. */
#define SCORE_BAD_CAPTURE (-1000000)

/* Bound on a history entry, chosen so the whole quiet band stays well below
 * SCORE_COUNTER and so the value fits an int16_t with room for the update
 * arithmetic. */
#define HISTORY_MAX 16384

/* ------------------------------------------------------ pruning margins -- */

/*
 * Centipawns per ply of remaining depth that the search is willing to assume a
 * position cannot swing by. Every one of these is a guess that trades safety
 * for speed, and every one of them is wrong sometimes - which is exactly why
 * each arrived behind its own SPRT rather than on the argument that it looks
 * reasonable.
 *
 * They are also all wrong TOGETHER whenever the evaluation changes underneath
 * them. Each was fitted against the classical model's scale and its noise
 * profile, and the network shares neither; docs/NNUE.md Task 4 is explicit
 * that re-fitting them is where a real part of the network's value is found.
 *
 * So the margins below are written with TUNABLE rather than #define. In a
 * normal build that expands to an enum constant and the compiler folds it
 * exactly as it folded the macro - the released engine is unchanged. Under
 * `make TUNE_SEARCH=on` it expands to a variable and uci.c advertises each one as a
 * spin option, so sweeping a candidate value costs a `setoption` instead of a
 * rebuild, and a sweep can drive the whole set from one binary.
 */
#ifdef TUNE_SEARCH
#define TUNABLE(name, def) int name = (def)
#else
#define TUNABLE(name, def) enum { name = (def) }
#endif

TUNABLE(RFP_MARGIN, 80); /* reverse futility: how far above beta is "safely won" */
#define RFP_DEPTH 7

#define LMP_DEPTH 8 /* deepest node that will discard its late quiet moves */

#define FUTILITY_DEPTH 6

/*
 * Futility margin per ply of remaining depth.
 *
 * Much smaller than the "value of a quiet move" intuition suggests, and the
 * reason is worth recording: at a null-window node alpha tracks the search
 * window, which tracks the evaluation, so staticEval sits very close to alpha
 * far more often than not. A margin of 100/ply asks for a full pawn of slack
 * at depth 1 and essentially never fires - measured at 0.008% of the bench
 * tree. The useful range is narrow and the whole curve is worth re-measuring
 * before anyone "corrects" this number upwards.
 */
TUNABLE(FUTILITY_MARGIN, 40);

/* Half-width of the first aspiration window, and the depth below which the
 * previous score is too unreliable to aim one at. */
#define ASPIRATION_DELTA     18
#define ASPIRATION_MIN_DEPTH 5

/*
 * Razoring: how far below alpha the static evaluation has to sit before the
 * node is dropped straight into quiescence.
 *
 * The claim is narrower than futility pruning's and that is why the margin is
 * so much larger. Futility discards individual quiet moves; this discards the
 * whole node on the strength of a quiescence search, so it has to be nearly
 * certain no quiet move here recovers the deficit. Verified rather than
 * assumed - the qsearch actually runs, and only its result can prune.
 */
TUNABLE(RAZOR_MARGIN, 240);
#define RAZOR_DEPTH 3

/*
 * SEE pruning thresholds, in centipawns per unit of depth.
 *
 * A move that loses material outright is worth searching only if there is
 * enough depth left to show what it wins back. Captures scale linearly and
 * quiets quadratically because a quiet move that hangs a piece has no
 * compensation to demonstrate in the first place - the bar for keeping one
 * should rise much faster as depth falls.
 */
#define SEE_CAPTURE_DEPTH 6
TUNABLE(SEE_CAPTURE_MARGIN, 100);
#define SEE_QUIET_DEPTH 8
TUNABLE(SEE_QUIET_MARGIN, 28);

/*
 * Delta pruning margin for quiescence.
 *
 * Standing pat is always available, so a capture that cannot bring the
 * evaluation within a minor piece of alpha even after winning its victim
 * outright is not going to raise it. The margin has to cover what the rest of
 * the sequence might swing, which is why it is a piece rather than a pawn.
 */
TUNABLE(DELTA_MARGIN, 200);

/*
 * Singular extension: the shallowest depth worth testing, and how far below
 * the stored score the verification window sits, in sixteenths of a centipawn
 * per ply of depth.
 *
 * The depth floor is what keeps this affordable - the verification is a real
 * search, so testing it near the leaves costs more than the extension can
 * return. The margin has to be wide enough that a move which is merely best
 * does not read as singular, and narrow enough that a genuinely forced line
 * still does.
 */
#define SINGULAR_DEPTH 7
TUNABLE(SINGULAR_MARGIN, 32);

/*
 * How much each correction history family is believed, out of CORR_W_UNIT.
 *
 * The pawn table sits at unit weight because that is the configuration E14
 * measured at +17.25 on the network, and this change is deliberately additive
 * to it: if the three new keys turn out to hold nothing, they contribute noise
 * around zero on top of an unchanged proven term rather than diluting it.
 *
 * The newcomers were first tried at a quarter each and measured slightly
 * negative (E16). An eighth is the same hypothesis at half strength, and the
 * reason for testing it rather than abandoning the keys is that the failure
 * had the shape of over-correction and not of bad evidence: the bench moved
 * 16.8% in the direction E14 says a correction pushed further from beta moves
 * it. Against a network that has already absorbed most of this structure, the
 * marginal table has less to say than it did against eval.c, and the weight
 * should reflect that.
 *
 * If an eighth also fails to beat pawn-only, the honest conclusion is that the
 * extra keys are not worth their cache on this evaluation - not that some
 * third weight would have worked. They are TUNABLE so the margin sweep can
 * settle them if they survive, rather than leaving guesses in the hot path.
 */
#define CORR_W_UNIT 128
TUNABLE(CORR_W_PAWN, 128);
TUNABLE(CORR_W_MINOR, 16);
TUNABLE(CORR_W_NONPAWN, 16);
TUNABLE(CORR_W_CONT, 16);

#ifdef TUNE_SEARCH

/*
 * The sweep's view of the margins above: a name to set them by and the range
 * a sweep is allowed to explore.
 *
 * Written out by hand rather than generated from a macro list, because a list
 * that generated both could not carry the comments above - and those comments
 * are the whole reason someone choosing a range picks a sensible one instead
 * of a symmetric guess around the default.
 *
 * The ranges are deliberately wider than any value that looks plausible now.
 * A sweep that cannot leave the neighbourhood of the current value can only
 * ever confirm it, and the point of re-fitting these against the network is
 * that the neighbourhood itself may be wrong.
 */
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
    {"SingularMargin", &SINGULAR_MARGIN, 4, 128},
    {"CorrWPawn", &CORR_W_PAWN, 0, 256},
    {"CorrWMinor", &CORR_W_MINOR, 0, 256},
    {"CorrWNonPawn", &CORR_W_NONPAWN, 0, 256},
    {"CorrWCont", &CORR_W_CONT, 0, 256},
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
            /* Clamped rather than rejected: a sweep that walks outside the
             * range should stay at the edge and keep playing, not have one
             * engine silently keep the previous value for the rest of a match
             * while the other moved. */
            *Tunables[i].value = iclamp(value, Tunables[i].min, Tunables[i].max);
            return true;
        }
    return false;
}
#endif /* TUNE_SEARCH */

/*
 * Divisors that convert a history score into plies of reduction, and capture
 * history into ordering points. Larger means less influence.
 */
#define LMR_HIST_DIVISOR 8192
#define LMR_CONT_DIVISOR 8192
#define CAPHIST_DIVISOR  4

/*
 * The three quiet-move heuristics, all declared at the top of the file so
 * search_clear() can reset them:
 *
 *   Killers[ply]        - quiet moves that caused a beta cutoff at this ply
 *                         somewhere else in the tree. Sibling nodes tend to
 *                         share refutations (the same fork, the same back-rank
 *                         threat), so a move that worked one branch over is
 *                         worth trying early with nothing else to recommend it.
 *
 *   History[c][from][to] - how well a quiet move has been doing lately. Not
 *                         tied to a ply, so it carries across the whole tree,
 *                         and it is what orders the long tail of quiet moves
 *                         no other heuristic has an opinion about.
 *
 *   CounterMoves[pc][to] - the quiet reply that most recently refuted this
 *                         exact move. Many threats have one specific answer
 *                         regardless of the rest of the position.
 *
 *   ContHist[..][..]    - continuation history: how well "this move, given the
 *                         move played N plies ago" has been doing.
 *                         CounterMoves remembers a single best reply; this
 *                         scores EVERY reply against the same context, which
 *                         is what lets the ordering understand plans rather
 *                         than one-move refutations - the bishop retreat that
 *                         is right only after the opponent played h6, and
 *                         wrong otherwise.
 *
 *   CaptureHist[..]     - the same idea for the tactical moves, which the
 *                         quiet tables never see. MVV-LVA and SEE between them
 *                         cannot separate two exchanges that win the same
 *                         material; recent success can.
 */

/*
 * True if the move is a capture or a promotion - the moves that change material
 * and so must never be reduced or written to the quiet history.
 *
 * Castling needs the explicit exclusion: it is encoded king-captures-own-rook,
 * so a naive look at the destination square finds a friendly rook there and
 * calls it the capture of a rook by a king. That both orders a quiet
 * developing move ahead of every real threat on the board and hides it from
 * the history tables.
 */
static inline bool is_tactical(const Position *pos, Move m) {
    switch (type_of_move(m)) {
    case MT_CASTLING: return false;
    case MT_EN_PASSANT:
    case MT_PROMOTION: return true;
    default: return piece_on(pos, to_sq(m)) != NO_PIECE;
    }
}

/*
 * Static exchange evaluation: is the material won by playing `m` at least
 * `threshold`, once both sides have exchanged optimally on the target square?
 *
 * MVV-LVA answers "what does this capture take"; SEE answers "what does this
 * capture WIN", which is a different and much more useful question. QxP looks
 * excellent to MVV-LVA and is a disaster if the pawn is defended. Searching
 * those first wastes the whole benefit of ordering, and in quiescence - where
 * every capture is searched and there is no depth limit to stop it - it is the
 * single largest source of wasted nodes.
 *
 * The algorithm is the standard swap-off: play the least valuable attacker
 * each time, tracking the running balance, and stop as soon as the side to
 * move would rather stand pat than continue. Removing each attacker from
 * `occupied` is what reveals the sliders x-raying through it.
 *
 * Answers a >= question rather than computing the exact value, because that
 * allows the early exits above and every caller only ever wanted a comparison.
 */
static bool see_ge(const Position *pos, Move m, Value threshold) {
    /* Castling captures nothing, en passant and promotions change material in
     * ways the swap loop does not model. Decline to judge them. */
    if (type_of_move(m) != MT_NORMAL)
        return VALUE_ZERO >= threshold;

    const Square from = from_sq(m);
    const Square to   = to_sq(m);

    /* Balance after the first capture, from the mover's point of view. */
    int swap = PieceValues[type_of(piece_on(pos, to))] - threshold;
    if (swap < 0)
        return false; /* even winning the victim for free falls short */

    swap = PieceValues[type_of(piece_on(pos, from))] - swap;
    if (swap <= 0)
        return true; /* the recapture cannot take back enough to matter */

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

        /*
         * Capture with the least valuable attacker available. Each branch
         * reveals only the sliders that could have been behind the piece just
         * removed, which is why the x-ray refresh differs per piece type.
         */
        Bitboard bb;
        if ((bb = mine & pos->byType[PAWN])) {
            if ((swap = PieceValues[PAWN] - swap) < result)
                break;
            occupied ^= square_bb(lsb(bb));
            attackers |= bishop_attacks(to, occupied) & (pos->byType[BISHOP] | pos->byType[QUEEN]);
        } else if ((bb = mine & pos->byType[KNIGHT])) {
            if ((swap = PieceValues[KNIGHT] - swap) < result)
                break;
            occupied ^= square_bb(lsb(bb)); /* a knight never unblocks anything */
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
            /* Only the king is left, and capturing into a defended square is
             * illegal - so if the other side still attacks it, the side to move
             * cannot continue and loses the exchange by default. */
            return (attackers & ~color_bb(pos, stm)) ? (result ^ 1) != 0 : result != 0;
        }
    }
    return result != 0;
}

/*
 * The continuation-history slice for the move played `back` plies above `ply`,
 * or NULL when there is no such move - the top of the tree, or a null move,
 * which is nobody's plan and must not have continuations attributed to it.
 *
 * The returned block is the trailing [PIECE_NB][SQUARE_NB] of the table,
 * flattened; index it with cont_index().
 */
static inline int16_t *cont_slice(int slot, int ply, int back) {
    if (ply < back)
        return NULL;

    const Move prev = Stack[ply - back].move;
    if (!is_ok_move(prev))
        return NULL;

    return &ContHist[slot][Stack[ply - back].movedPiece][to_sq(prev)][0][0];
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

/* The victim a move takes, or NO_PIECE_TYPE if it takes nothing. Castling is
 * encoded king-captures-own-rook, so it needs the explicit exclusion. */
static inline PieceType victim_of(const Position *pos, Move m) {
    switch (type_of_move(m)) {
    case MT_EN_PASSANT: return PAWN;
    case MT_CASTLING: return NO_PIECE_TYPE;
    default: return type_of(piece_on(pos, to_sq(m)));
    }
}

/*
 * MVV-LVA for the tactical moves: capture the most valuable victim with the
 * least valuable attacker first, with SEE deciding which band the capture
 * lands in and capture history separating the ones SEE calls equal. Quiet
 * moves fall through to the heuristic tables above.
 */
static void score_moves(const Position *pos, ScoredMove *list, int count, Move ttMove, int ply,
                        Move counter) {
    const Color us     = pos->sideToMove;
    const Move killer0 = Killers[ply][0];
    const Move killer1 = Killers[ply][1];

    /* Context for continuation history: the moves that led to this node. */
    int16_t *slices[CONT_SLOTS];
    for (int i = 0; i < CONT_SLOTS; ++i)
        slices[i] = cont_slice(i, ply, ContPlies[i]);

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

            /* Divided down so it refines the MVV-LVA order rather than
             * overturning it: history is evidence about a capture, not a
             * replacement for knowing what it takes. */
            const int capHist = CaptureHist[moved][to_sq(m)][victim] / CAPHIST_DIVISOR;

            /* A capture that loses material once the recaptures are played out
             * is worse than almost any quiet move, not better than all of them.
             * Order it below them instead of ahead of the whole list. */
            score =
                (see_ge(pos, m, VALUE_ZERO) ? SCORE_CAPTURE : SCORE_BAD_CAPTURE) + mvvLva + capHist;
        }

        if (mt == MT_PROMOTION)
            score += SCORE_CAPTURE + PieceValues[promotion_type(m)];

        /* Nothing tactical matched, so the move is quiet. Tested explicitly
         * rather than by `score == 0`: capture history can push a capture's
         * score anywhere inside its band, including onto zero. */
        if (victim == NO_PIECE_TYPE && mt != MT_PROMOTION) {
            if (m == killer0)
                score = SCORE_KILLER_1;
            else if (m == killer1)
                score = SCORE_KILLER_2;
            else if (m == counter)
                score = SCORE_COUNTER;
            else
                score = History[us][from_sq(m)][to_sq(m)] + cont_score(slices, moved, to_sq(m));
        }

        list[i].score = score;
    }
}

/* The counter-move registered against whatever was played to reach `ply`. */
static Move counter_move(int ply) {
    const Move prev = Stack[ply - 1].move;
    return is_ok_move(prev) ? CounterMoves[Stack[ply - 1].movedPiece][to_sq(prev)] : MOVE_NONE;
}

/* A cutoff found deep in the tree is much stronger evidence than one found
 * next to the leaves, so the bonus grows with depth - but is capped, because
 * beyond a point the extra confidence is not real and a single deep cutoff
 * should not be able to saturate an entry on its own. */
static int history_bonus(Depth depth) {
    const Depth d = imin(depth, 20);
    return d * d * 4;
}

/*
 * Applies `bonus`, decaying the entry towards zero in proportion to how large
 * it already is.
 *
 * That gravity term is the whole design. A plain running total saturates: once
 * an entry is large, further evidence cannot move it, and the table ends up
 * describing the opening rather than the position on the board. Scaling the
 * decay by the current value makes the entry an exponential moving average of
 * recent success instead, which is what ordering actually wants.
 */
static void history_update(int16_t *entry, int bonus) {
    const int b = bonus > HISTORY_MAX ? HISTORY_MAX : bonus < -HISTORY_MAX ? -HISTORY_MAX : bonus;
    *entry += (int16_t)(b - (int)*entry * (b < 0 ? -b : b) / HISTORY_MAX);
}

/* Credit or blame `pc -> to` in every continuation slot that exists here. */
static void cont_hist_update(int ply, Piece pc, Square to, int bonus) {
    const int idx = cont_index(pc, to);

    for (int i = 0; i < CONT_SLOTS; ++i) {
        int16_t *const slice = cont_slice(i, ply, ContPlies[i]);
        if (slice)
            history_update(&slice[idx], bonus);
    }
}

/* Capture history is keyed on what the capture takes, so the victim has to be
 * read off the board - which means this must run AFTER the move was undone. */
static void capture_hist_update(const Position *pos, Move m, int bonus) {
    const Piece moved = piece_on(pos, from_sq(m));
    history_update(&CaptureHist[moved][to_sq(m)][victim_of(pos, m)], bonus);
}

/*
 * Records that `best` caused a cutoff, and that everything tried before it did
 * not. Knowing what fails is worth as much to ordering as knowing what works:
 * without the penalty, a move that is tried early and always fails keeps its
 * position forever because nothing ever pushes it down.
 *
 * Both lists are penalised whichever kind of move cut, because both were tried
 * and both failed. Only the winner's own table is credited - a capture teaches
 * the quiet heuristics nothing, and a quiet cutoff says nothing about which
 * exchange was worth making.
 */
static void update_stats(const Position *pos, Move best, const Move *quiets, int quietCount,
                         const Move *captures, int captureCount, Depth depth, int ply) {
    const Color us   = pos->sideToMove;
    const int bonus  = history_bonus(depth);
    const bool quiet = !is_tactical(pos, best);

    if (quiet) {
        if (Killers[ply][0] != best) {
            Killers[ply][1] = Killers[ply][0];
            Killers[ply][0] = best;
        }

        history_update(&History[us][from_sq(best)][to_sq(best)], bonus);
        cont_hist_update(ply, piece_on(pos, from_sq(best)), to_sq(best), bonus);

        const Move prev = Stack[ply - 1].move;
        if (is_ok_move(prev))
            CounterMoves[Stack[ply - 1].movedPiece][to_sq(prev)] = best;
    } else {
        capture_hist_update(pos, best, bonus);
    }

    for (int i = 0; i < quietCount; ++i) {
        if (quiets[i] == best)
            continue;
        history_update(&History[us][from_sq(quiets[i])][to_sq(quiets[i])], -bonus);
        cont_hist_update(ply, piece_on(pos, from_sq(quiets[i])), to_sq(quiets[i]), -bonus);
    }

    for (int i = 0; i < captureCount; ++i)
        if (captures[i] != best)
            capture_hist_update(pos, captures[i], -bonus);
}

/* Anything but kings and pawns. The test that decides whether null-move
 * pruning is safe: see the note at its call site. */
static inline bool has_non_pawn_material(const Position *pos, Color c) {
    return (pieces2_bb(pos, c, KNIGHT, BISHOP) | pieces2_bb(pos, c, ROOK, QUEEN)) != BB_EMPTY;
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

/* --------------------------------------------------- correction history -- */

/*
 * The static evaluation is a guess about a position; the search is the truth
 * about it. Correction history remembers how far apart the two have been
 * running lately for a given pawn structure, and shifts the next static
 * evaluation by that much.
 *
 * Pawn structure is the key because it is the feature that survives the moves
 * a search actually makes. A blocked centre, a ruined king shelter, a passer
 * the evaluation undervalues - each is wrong in the same direction across a
 * whole region of the tree, and a standing bias is exactly what is worth
 * learning. Keying on the full position instead would be a transposition table
 * with a worse replacement policy: every entry written once and never
 * confirmed.
 *
 * Nothing the search reports as a score is corrected. The corrected value
 * feeds `improving`, the pruning margins and the reductions - decisions that
 * are already bets - while what goes into the transposition table is the raw
 * evaluation, so a later probe re-corrects with whatever the table has learned
 * since instead of inheriting a stale adjustment.
 */

#define CORR_INDEX(key) ((key) & (CORRHIST_SIZE - 1))

/* The continuation entry, or NULL where there is no previous move to key on -
 * the root, and after a null move, which is nobody's plan and must not have a
 * correction attributed to it. Same rule as cont_slice(), for the same reason. */
static inline int16_t *cont_corr_entry(int ply) {
    if (ply < 1)
        return NULL;

    const Move prev = Stack[ply - 1].move;
    if (!is_ok_move(prev))
        return NULL;

    return &ContCorrHist[Stack[ply - 1].movedPiece][to_sq(prev)];
}

/* The four tables' combined opinion, in CORRHIST_GRAIN fixed point. */
static int corr_total(const Position *pos, int ply) {
    const Color us = pos->sideToMove;

    int total = CORR_W_PAWN * PawnCorrHist[us][CORR_INDEX(pos->pawnKey)];
    total += CORR_W_MINOR * MinorCorrHist[us][CORR_INDEX(pos->minorKey)];
    total += CORR_W_NONPAWN * (NonPawnCorrHist[us][WHITE][CORR_INDEX(pos->nonPawnKey[WHITE])] +
                               NonPawnCorrHist[us][BLACK][CORR_INDEX(pos->nonPawnKey[BLACK])]);

    const int16_t *const cont = cont_corr_entry(ply);
    if (cont)
        total += CORR_W_CONT * *cont;

    return iclamp(total / CORR_W_UNIT, -CORRHIST_TOTAL_LIMIT, CORRHIST_TOTAL_LIMIT);
}

/* Mate scores are clamped away deliberately: a correction is evidence about an
 * evaluation, and letting one push a score into mate range would have the
 * search report a mate that nothing proved. */
static Value corrected_eval(const Position *pos, Value raw, int ply) {
    if (raw == VALUE_NONE)
        return VALUE_NONE;

    const int v = raw + corr_total(pos, ply) / CORRHIST_GRAIN;
    return (Value)iclamp(v, VALUE_MATED_IN_MAX_PLY + 1, VALUE_MATE_IN_MAX_PLY - 1);
}

/*
 * Folds one observation into the entry as an exponential moving average,
 * weighted by depth because a deeper search is better evidence about the same
 * question. The gravity term is the same idea as in history_update: an entry
 * that cannot be moved once it is large describes the opening, not the board.
 *
 * What counts as an observation is the decision that matters, and it is made
 * at the call site rather than here.
 */
static void corr_fold(int16_t *e, int diff, int weight) {
    const int updated = (*e * (CORRHIST_WEIGHT_MAX - weight) + diff * weight) / CORRHIST_WEIGHT_MAX;
    *e                = (int16_t)iclamp(updated, -CORRHIST_LIMIT, CORRHIST_LIMIT);
}

/*
 * Every family sees the same observation at the same weight. They differ only
 * in what they are keyed by, which is the whole design: one node teaches each
 * table something about a different description of the position it was in, and
 * the tables that key on a description the error does not depend on average
 * that same evidence away to nothing over the tree.
 */
static void corrhist_update(const Position *pos, Value searched, Value staticEval, Depth depth,
                            int ply) {
    const Color us   = pos->sideToMove;
    const int weight = imin(depth + 1, 16);
    const int diff   = (searched - staticEval) * CORRHIST_GRAIN;

    corr_fold(&PawnCorrHist[us][CORR_INDEX(pos->pawnKey)], diff, weight);
    corr_fold(&MinorCorrHist[us][CORR_INDEX(pos->minorKey)], diff, weight);
    corr_fold(&NonPawnCorrHist[us][WHITE][CORR_INDEX(pos->nonPawnKey[WHITE])], diff, weight);
    corr_fold(&NonPawnCorrHist[us][BLACK][CORR_INDEX(pos->nonPawnKey[BLACK])], diff, weight);

    int16_t *const cont = cont_corr_entry(ply);
    if (cont)
        corr_fold(cont, diff, weight);
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
    update_seldepth(ply);

    if (search_stopped())
        return VALUE_ZERO;

    if (ply >= MAX_PLY - 1)
        return corrected_eval(pos, eval_evaluate(pos), ply);

    const bool pvNode = beta - alpha > 1;
    const Key key     = pos->key;

    /*
     * Quiescence probes the same table the main search writes to. Everything
     * stored from here carries depth 0, so a quiescence entry can never
     * satisfy a main-search probe - but a main-search entry, which is deeper,
     * answers a quiescence probe perfectly well.
     */
    TTEntry tte;
    const bool ttHit    = tt_probe(key, &tte);
    const Value ttValue = ttHit ? tt_value_from_tt(tt_entry_value(&tte), ply) : VALUE_NONE;
    Move ttMove         = ttHit ? tt_entry_move(&tte) : MOVE_NONE;

    /* Sixteen bits of key is not proof of identity, and the entry may predate
     * several moves of the game. Never play a probed move unvalidated. */
    if (ttMove != MOVE_NONE && !movegen_is_pseudo_legal(pos, ttMove))
        ttMove = MOVE_NONE;

    if (!pvNode && ttValue != VALUE_NONE &&
        (tt_entry_bound(&tte) & (ttValue >= beta ? BOUND_LOWER : BOUND_UPPER)))
        return ttValue;

    const bool inCheck = board_checkers(pos) != BB_EMPTY;
    Value best         = -VALUE_INFINITE;
    Value staticEval   = VALUE_NONE;
    Value rawEval      = VALUE_NONE;

    if (!inCheck) {
        /* Stand pat: the side to move is never obliged to capture, so the
         * static evaluation is a lower bound on what it can achieve. In check
         * there is no such option - every reply must be searched. */
        rawEval =
            ttHit && tt_entry_eval(&tte) != VALUE_NONE ? tt_entry_eval(&tte) : eval_evaluate(pos);
        staticEval = corrected_eval(pos, rawEval, ply);
        best       = staticEval;

        if (best >= beta) {
            tt_store(key, MOVE_NONE, best, rawEval, 0, BOUND_LOWER, ply);
            return best;
        }
        if (best > alpha)
            alpha = best;
    }

    ScoredMove moves[MAX_MOVES];
    const int count = movegen_generate(pos, inCheck ? GEN_EVASIONS : GEN_CAPTURES, moves);

    /* No counter-move: quiescence only reaches quiet moves when answering a
     * check, and an evasion is dictated by the check rather than by whatever
     * the opponent played to arrive here. */
    score_moves(pos, moves, count, ttMove, ply, MOVE_NONE);

    Move bestMove = MOVE_NONE;
    int legal     = 0;

    for (int i = 0; i < count; ++i) {
        pick_move(moves, count, i);
        const Move m = moves[i].m;

        /*
         * Drop the losing captures.
         *
         * score_moves already ran SEE on every capture and put the ones that
         * lose material into the only negative band a capture list contains, so
         * this costs nothing beyond the comparison. pick_move has just selected
         * the highest remaining score, so once it is negative every move left
         * is a losing capture and the whole tail can go.
         *
         * This is where quiescence earns most of its speed. Standing pat is
         * always available out of check, so a capture that ends up down
         * material cannot beat it, and searching the recapture sequence to find
         * that out is pure cost. In check there is no stand pat and every reply
         * has to be searched, however bad it looks.
         */
        if (!inCheck && moves[i].score < 0)
            break;

        /*
         * Delta pruning: even winning this victim outright, and granting a
         * margin for whatever the rest of the sequence swings, leaves the
         * score short of alpha. Standing pat already beats that, so searching
         * the capture cannot change the answer.
         *
         * Skipped for promotions, whose material gain is the new piece rather
         * than the captured one, and while in check, where there is no stand
         * pat to fall back on and every reply must be searched.
         */
        if (!inCheck && type_of_move(m) != MT_PROMOTION && !is_mate_score(alpha) &&
            staticEval + PieceValues[victim_of(pos, m)] + DELTA_MARGIN <= alpha)
            continue;

        if (!movegen_is_legal(pos, m))
            continue;
        ++legal;

        /* Quiescence maintains the stack too, so that a node below it reads the
         * move that actually led there rather than whatever the main search
         * left at this ply on an earlier visit. */
        Stack[ply].move       = m;
        Stack[ply].movedPiece = piece_on(pos, from_sq(m));
        Stack[ply].staticEval = staticEval;

        board_do_move(pos, m);
        eval_state_push(pos, m);
        tt_prefetch(pos->key);
        const Value v = -qsearch(pos, -beta, -alpha, ply + 1);
        eval_state_pop();
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

    /* Evasions are the complete move list, so no legal reply here really is
     * checkmate - worth detecting, since missing it makes the engine walk into
     * mate believing it has stand-pat equality. */
    if (inCheck && legal == 0)
        return mated_in(ply);

    /* Quiescence never searches the full move list, so it can never prove an
     * exact score: the value is a lower bound if it failed high and an upper
     * bound otherwise. */
    tt_store(key, bestMove, best, rawEval, 0, best >= beta ? BOUND_LOWER : BOUND_UPPER, ply);

    return best;
}

/* --------------------------------------------------------------- search -- */

/*
 * `cutNode` is the caller's expectation, not a fact: it is true at a node the
 * parent believes will fail high. It costs nothing to propagate and it is the
 * best available prior on how a node will turn out, which is exactly what the
 * reductions want - a node expected to cut is one where searching the tail
 * cheaply is least likely to lose anything. Being wrong about it is safe:
 * everything it feeds either re-searches or is bounded by depth.
 */
static Value negamax(Position *pos, Depth depth, Value alpha, Value beta, int ply, bool cutNode) {
    /* Cleared here rather than at each use, so that a child which never
     * extends the PV (a quiescence node) leaves a length of zero behind. */
    PvLength[ply] = 0;

    if (depth <= 0)
        return qsearch(pos, alpha, beta, ply);

    count_node();
    update_seldepth(ply);

    if (search_stopped())
        return VALUE_ZERO;

    /*
     * Read and clear in one step. A singular search sets this immediately
     * before re-entering at the same ply, and every other entry must see
     * MOVE_NONE - including a later, unrelated visit to this ply, which would
     * otherwise inherit an exclusion from whatever ran here before.
     */
    const Move excluded     = Stack[ply].excludedMove;
    Stack[ply].excludedMove = MOVE_NONE;
    const bool isExcluded   = excluded != MOVE_NONE;

    /* The root is handled by search_root, so this is never ply 0 - which is
     * what lets the draw and mate-distance tests below run unconditionally. */
    assert(ply > 0);

    if (board_is_draw(pos, ply))
        return VALUE_DRAW;

    if (ply >= MAX_PLY - 1)
        return corrected_eval(pos, eval_evaluate(pos), ply);

    /* Determined before mate distance pruning narrows the window: a node is a
     * PV node because of where it sits in the tree, and must keep being
     * treated as one even if the narrowing leaves it with a null window. */
    const bool pvNode = beta - alpha > 1;

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

    const Key key = pos->key;

    TTEntry tte;
    const bool ttHit    = tt_probe(key, &tte);
    const Value ttValue = ttHit ? tt_value_from_tt(tt_entry_value(&tte), ply) : VALUE_NONE;
    Move ttMove         = ttHit ? tt_entry_move(&tte) : MOVE_NONE;

    /* Sixteen bits of key is not proof of identity, and the entry may predate
     * several moves of the game. Never play a probed move unvalidated. */
    if (ttMove != MOVE_NONE && !movegen_is_pseudo_legal(pos, ttMove))
        ttMove = MOVE_NONE;

    /*
     * Cut off on a stored result that is at least as deep and whose bound
     * points the right way.
     *
     * Not at PV nodes: a bound is enough to prove a cutoff but not to name the
     * move that caused it, so returning one there leaves the reported
     * principal variation truncated at this node.
     *
     * Not near the fifty-move boundary either. A stored score says nothing
     * about how many reversible moves preceded the position, and a won
     * position that is four plies from a draw claim is not worth what the same
     * position was worth eighty plies earlier.
     */
    /* Never while verifying a singular move: the stored result was proved with
     * the excluded move available, so it answers a different question than the
     * one this search is asking. */
    if (!isExcluded && !pvNode && ttValue != VALUE_NONE && tt_entry_depth(&tte) >= depth &&
        pos->halfmoveClock < 90 &&
        (tt_entry_bound(&tte) & (ttValue >= beta ? BOUND_LOWER : BOUND_UPPER)))
        return ttValue;

    const Color us     = pos->sideToMove;
    const bool inCheck = board_checkers(pos) != BB_EMPTY;

    /*
     * Internal iterative reduction.
     *
     * No table move means nothing has ever searched this node deeply enough to
     * leave an opinion, so the move list will be ordered by heuristics alone.
     * A badly ordered node at full depth is the most expensive kind there is -
     * alpha-beta degenerates towards its worst case exactly when the best move
     * is tried last. Searching one ply shallower is cheaper, and it leaves a
     * table move behind for when this node is revisited, which is worth more
     * than the ply given up.
     */
    if (depth >= 4 && ttMove == MOVE_NONE && !inCheck)
        --depth;

    /*
     * Static evaluation, reusing the one cached in the table when this position
     * has been seen before. In check it is not computed at all: the score of a
     * position whose king is attacked says nothing useful, and every heuristic
     * below that would consume it is disabled while in check anyway.
     */
    const Value rawEval = inCheck
                              ? VALUE_NONE
                              : (ttHit && tt_entry_eval(&tte) != VALUE_NONE ? tt_entry_eval(&tte)
                                                                            : eval_evaluate(pos));

    /* The table keeps `rawEval`; everything below reasons with the corrected
     * one. See the correction history section for why those differ. */
    const Value staticEval = corrected_eval(pos, rawEval, ply);

    Stack[ply].staticEval = staticEval;

    /*
     * Is this side's position getting better?
     *
     * Compared against the grandparent, because that is the last node where the
     * same side was to move - the parent's evaluation belongs to the opponent
     * and is roughly the negation of ours. A side whose evaluation is rising
     * has the initiative, and both pruning and reductions want to know: a
     * rising position is more likely to produce the fail high that pruning is
     * betting on, and a falling one deserves the benefit of the doubt.
     *
     * In check there is no static evaluation to compare, and at the top of the
     * tree there is no grandparent, so both default to false - the cautious
     * answer, since it prunes less.
     */
    const bool improving = !inCheck && ply >= 2 && Stack[ply - 2].staticEval != VALUE_NONE &&
                           staticEval > Stack[ply - 2].staticEval;

    /*
     * Reverse futility pruning, also called static null move pruning.
     *
     * If the static evaluation is so far above beta that even conceding a
     * pawn-and-a-bit per remaining ply would not bring it back down, the node
     * is not going to fail low, and searching it to prove that is wasted work.
     *
     * The depth limit is what keeps it honest: the margin is a claim about how
     * much a position can plausibly swing in the plies left, and past a handful
     * of plies that claim stops being true - deep enough searches find swings
     * of any size. Restricted to non-PV nodes with no mate score in the window,
     * because giving up a proof is only acceptable where a bound is all anyone
     * was going to use.
     */
    if (!pvNode && !inCheck && depth <= RFP_DEPTH && !is_mate_score(beta) &&
        staticEval - RFP_MARGIN * (depth - improving) >= beta)
        return staticEval;

    /*
     * Razoring: the mirror image of reverse futility, at the other end of the
     * window.
     *
     * A position this far below alpha is one where the quiet moves are not
     * going to save it, and what is left to check is whether a tactic does.
     * That is precisely the question quiescence answers, so ask it directly
     * rather than spending a full-width search arriving at the same place.
     *
     * The qsearch is what makes this safe. Nothing is pruned on the margin
     * alone - the margin only decides that the node is worth a cheap second
     * opinion, and the node is dropped only if that opinion agrees.
     */
    if (!pvNode && !inCheck && depth <= RAZOR_DEPTH && !is_mate_score(alpha) &&
        staticEval + RAZOR_MARGIN * depth < alpha) {
        const Value v = qsearch(pos, alpha - 1, alpha, ply);
        if (v < alpha)
            return v;
    }

    /*
     * Null-move pruning: hand the opponent a free move. If the position still
     * fails high after that, it was far too good to be worth searching
     * properly, and a shallow verification is enough to say so.
     *
     * The non-pawn-material test is what keeps this sound. Its assumption is
     * that having to move is a burden, and in a king-and-pawn endgame that is
     * routinely false - zugzwang is often the entire content of the position,
     * and a side that would love to pass will happily "prove" a cutoff it
     * cannot actually achieve. Refusing a second null move in a row matters
     * for the same reason: two passes in succession prove nothing about a
     * game in which passing is illegal.
     */
    if (!pvNode && !inCheck && !isExcluded && depth >= 3 && staticEval >= beta &&
        Stack[ply - 1].move != MOVE_NULL && has_non_pawn_material(pos, us)) {
        const Depth r = 3 + depth / 4 + imin((staticEval - beta) / 200, 3);

        Stack[ply].move       = MOVE_NULL;
        Stack[ply].movedPiece = NO_PIECE;

        board_do_null_move(pos);
        eval_state_push_null(pos);
        tt_prefetch(pos->key);
        const Value v = -negamax(pos, depth - r, -beta, -beta + 1, ply + 1, !cutNode);
        eval_state_pop();
        board_undo_null_move(pos);

        if (search_stopped())
            return VALUE_ZERO;

        /* A mate score proved by letting the opponent move twice is not a mate
         * at all. Return the bound the search is entitled to, not the claim. */
        if (v >= beta)
            return v >= VALUE_MATE_IN_MAX_PLY ? beta : v;
    }

    ScoredMove moves[MAX_MOVES];
    const int count = movegen_generate(pos, inCheck ? GEN_EVASIONS : GEN_ALL, moves);
    score_moves(pos, moves, count, ttMove, ply, counter_move(ply));

    /* Moves already tried here, so that the one that eventually cuts can
     * penalise them. Bounded: a node with more than this many is one where the
     * ordering statistics were not going to be decisive anyway. */
    Move quiets[64];
    int quietCount = 0;
    Move captures[32];
    int captureCount = 0;

    /* The same continuation context score_moves used, kept for the pruning and
     * reduction decisions below - a quiet move the plan-aware tables like
     * deserves the benefit of the doubt that its position in the list denies
     * it. */
    int16_t *slices[CONT_SLOTS];
    for (int i = 0; i < CONT_SLOTS; ++i)
        slices[i] = cont_slice(i, ply, ContPlies[i]);

    Value best    = -VALUE_INFINITE;
    Move bestMove = MOVE_NONE;
    int moveCount = 0;

    /*
     * Tracked separately from `bestMove`, and the distinction is the whole
     * difference between an exact score and an upper bound. Every node ends up
     * with a best-scoring move, including one that failed low - so using
     * `bestMove != MOVE_NONE` to mean "we proved a score" would mark every
     * fail-low entry BOUND_EXACT, and a later probe would happily read that
     * upper bound back as a proven lower bound and cut on it.
     */
    bool raisedAlpha = false;

    for (int i = 0; i < count; ++i) {
        pick_move(moves, count, i);
        const Move m = moves[i].m;

        /* The move this search is proving the rest of the list can live
         * without. */
        if (m == excluded)
            continue;

        if (!movegen_is_legal(pos, m))
            continue;
        ++moveCount;

        const bool tactical = is_tactical(pos, m);
        const Piece moved   = piece_on(pos, from_sq(m));

        /* How the plan-aware tables rate this move. Only meaningful for quiet
         * moves - the tactical ones are scored by what they win. */
        const int contScore = tactical ? 0 : cont_score(slices, moved, to_sq(m));

        /*
         * Shallow-depth pruning of quiet moves.
         *
         * Guarded on `best` being better than a forced mate: until the node has
         * found something that is not losing outright, every remaining move is
         * a candidate escape and none of them can be discarded on a heuristic.
         *
         * Late move pruning: the move list is ordered, so once this many quiet
         * moves have been tried without one of them raising alpha, the rest are
         * overwhelmingly unlikely to. Unlike a reduction this does not
         * re-search, so it can genuinely lose a move - which is why it is
         * confined to low depths, where the cost of being wrong is smallest and
         * the number of nodes saved is largest.
         */
        if (!pvNode && !inCheck && best > VALUE_MATED_IN_MAX_PLY && !tactical) {
            /* Half as many quiets get a look when the position is not
             * improving: the moves are less likely to be worth it and the node
             * is less likely to be the one that matters. */
            if (depth <= LMP_DEPTH &&
                moveCount >= (improving ? 3 + depth * depth : (3 + depth * depth) / 2))
                continue;

            /*
             * A continuation-history pruning rule sat here and was removed: it
             * never fired. A quiet move whose continuation history is bad
             * enough to trip a threshold of that shape also scores low enough
             * to sort to the end of the list, where late move pruning has
             * already discarded it - so LMP subsumes it entirely at these
             * depths. See E9 in docs/EXPERIMENTS.md for the measurement.
             */

            /*
             * Futility pruning: the position is already so far below alpha
             * that a quiet move - which by definition wins no material - has
             * no realistic way of closing the gap in the depth remaining.
             *
             * Note this prunes the whole quiet TAIL, not just this move: the
             * test does not depend on which move it is, so once it fires it
             * fires for every quiet still to come. Captures keep being
             * searched, which is the point - material is exactly what a quiet
             * move cannot produce and a capture can.
             */
            if (depth <= FUTILITY_DEPTH && staticEval + FUTILITY_MARGIN * depth <= alpha)
                continue;
        }

        /*
         * SEE pruning. The move loses material outright and there is not
         * enough depth left for whatever it was playing for to appear.
         *
         * This is the one pruning rule that applies to captures as well as
         * quiets, and it is the reason it is worth having: nothing above
         * touches a capture, so a piece thrown onto a defended square is
         * otherwise searched at full width no matter how bad it is.
         *
         * The thresholds scale differently on purpose. A capture that loses a
         * little is often a real sacrifice, so its allowance grows linearly
         * with the depth left to justify it. A quiet move that hangs material
         * has won nothing to weigh against the loss, so its allowance is
         * squeezed much harder as depth falls.
         *
         * see_ge() declines to judge castling, en passant and promotions, and
         * answers VALUE_ZERO >= threshold for them - which against a negative
         * threshold is always true, so those moves are never pruned here.
         */
        if (!pvNode && !inCheck && best > VALUE_MATED_IN_MAX_PLY) {
            const Depth seeDepth = tactical ? SEE_CAPTURE_DEPTH : SEE_QUIET_DEPTH;
            const Value seeMargin =
                tactical ? -SEE_CAPTURE_MARGIN * depth : -SEE_QUIET_MARGIN * depth * depth;

            if (depth <= seeDepth && !see_ge(pos, m, seeMargin))
                continue;
        }

        if (!tactical) {
            if (quietCount < (int)(sizeof(quiets) / sizeof(quiets[0])))
                quiets[quietCount++] = m;
        } else if (captureCount < (int)(sizeof(captures) / sizeof(captures[0]))) {
            captures[captureCount++] = m;
        }

        Depth extension = 0;

        /*
         * Singular extension.
         *
         * The table says this move was best here, and says so from a search
         * nearly as deep as this one. The question worth asking is not whether
         * it is good but whether it is the ONLY move that is: a position with
         * one playable move is a forced line, and forced lines are where an
         * extra ply buys the most, because the branching that usually makes
         * depth expensive is not there.
         *
         * The test is a search of every OTHER move against a window just below
         * the stored score. If they all fail low, the move is singular and gets
         * a ply. Three outcomes come out of one search:
         *
         *   - all others fail low: singular, extend.
         *   - the reduced search beats the real beta: several moves are good
         *     enough here, so this node fails high and none of them needs
         *     proving. That is multi-cut - a whole node skipped rather than a
         *     ply added.
         *   - others reach the window and the table move already beats beta:
         *     this node is easy and over-searched. Take plies AWAY.
         *
         * The verification runs at this same ply with `excludedMove` set, so
         * everything it can see is exactly what this node can see minus the one
         * move. That re-entrancy is why the excluded flag is read-and-cleared
         * on entry and why the TT is neither trusted nor written while it is
         * set.
         */
        if (!isExcluded && depth >= SINGULAR_DEPTH && m == ttMove && ttValue != VALUE_NONE &&
            !is_mate_score(ttValue) && (tt_entry_bound(&tte) & BOUND_LOWER) &&
            tt_entry_depth(&tte) >= depth - 3) {
            const Value singularBeta  = ttValue - SINGULAR_MARGIN * depth / 16;
            const Depth singularDepth = (depth - 1) / 2;

            /* The verification searches this ply again and writes the PV table
             * as it goes. Nothing has been recorded here yet - the table move
             * always sorts first - but restoring it keeps that an observation
             * rather than a dependency. */
            const int savedPvLength = PvLength[ply];

            Stack[ply].excludedMove = m;
            const Value v =
                negamax(pos, singularDepth, singularBeta - 1, singularBeta, ply, cutNode);
            Stack[ply].excludedMove = MOVE_NONE;

            PvLength[ply] = savedPvLength;

            if (search_stopped())
                return VALUE_ZERO;

            if (v < singularBeta)
                extension = 1;
            else if (singularBeta >= beta && !pvNode)
                return singularBeta;
            else if (ttValue >= beta)
                extension = -2;
        }

        Stack[ply].move       = m;
        Stack[ply].movedPiece = moved;

        board_do_move(pos, m);
        eval_state_push(pos, m);
        tt_prefetch(pos->key);

        const bool givesCheck = board_checkers(pos) != BB_EMPTY;

        /*
         * Check extension: search a checking move to full depth rather than
         * one less.
         *
         * A check is the most forcing move in chess - the reply set is tiny and
         * often single - so the branch costs little and the tactics that decide
         * games live there. Without it the search reaches its horizon in the
         * middle of a forcing sequence and evaluates a position that is about
         * to change completely.
         *
         * Bounded by ply so a perpetual-check line cannot extend forever: past
         * twice the nominal iteration depth, stop paying for it. Never stacked
         * on top of a singular extension - one ply is the answer to "this line
         * is forced", however many reasons there are to think so.
         */
        if (extension == 0 && givesCheck && ply < 2 * RootDepth)
            extension = 1;

        const Depth childDepth = depth - 1 + extension;
        Value v                = VALUE_NONE;

        /*
         * Late move reductions.
         *
         * Move ordering is good enough that a quiet move tried this late is
         * very unlikely to be best, so search it shallower and cheaply. The
         * safety net is the re-search: anything that beats alpha despite the
         * reduction gets searched again at full depth, so a reduction can cost
         * time but cannot lose a move. Captures, checks, and replies to check
         * are excluded - they are forcing, and reducing a forcing line is how
         * an engine walks into a tactic it had the depth to see.
         */
        Depth r = 0;
        if (depth >= 3 && moveCount > 2 && !tactical && !inCheck && !givesCheck) {
            r = Reductions[imin(depth, 63)][imin(moveCount, 63)];

            /* The PV is where accuracy is worth paying for. */
            if (pvNode)
                --r;

            /* A position that is not improving is one where the search has
             * less to lose by looking at the tail more cheaply. */
            if (!improving)
                ++r;

            /*
             * An extra two plies of reduction at cut nodes was tried here and
             * measured WORSE - about 16 Elo worse over 1405 games (E6 in
             * docs/EXPERIMENTS.md). It is standard practice in stronger
             * engines, but they carry verification re-searches and much richer
             * reduction curves to catch what it throws away; on this LMR
             * formula it simply reduces too hard. `cutNode` still earns its
             * place propagating the expectation to the child searches below.
             */

            /* A quiet move the history tables like is not "late" in any sense
             * that matters, whatever its position in the list. Both tables get
             * a say: the butterfly history knows the move, the continuation
             * tables know the move in this context. */
            r -= History[us][from_sq(m)][to_sq(m)] / LMR_HIST_DIVISOR;
            r -= contScore / LMR_CONT_DIVISOR;

            if (r < 0)
                r = 0;
            else if (r > childDepth - 1)
                r = childDepth - 1;
        }

        /*
         * Principal variation search. The first move is searched with the full
         * window; for the rest the only question worth asking is whether
         * anything BEATS it, and a null window answers that far more cheaply.
         * Whatever does beat it is then re-searched properly.
         */
        if (r > 0) {
            /* A reduced null-window search is a bet that the move fails low,
             * so the child is by definition expected to fail high. */
            v = -negamax(pos, childDepth - r, -alpha - 1, -alpha, ply + 1, true);
            if (v > alpha)
                v = -negamax(pos, childDepth, -alpha - 1, -alpha, ply + 1, !cutNode);
        } else if (!pvNode || moveCount > 1) {
            v = -negamax(pos, childDepth, -alpha - 1, -alpha, ply + 1, !cutNode);
        }

        /* A full-window search is never a cut node: the whole point is that its
         * value is wanted exactly, not as a bound. */
        if (pvNode && (moveCount == 1 || (v > alpha && v < beta)))
            v = -negamax(pos, childDepth, -beta, -alpha, ply + 1, false);

        eval_state_pop();
        board_undo_move(pos, m);

        if (search_stopped())
            return VALUE_ZERO;

        if (v > best) {
            best     = v;
            bestMove = m;
            if (v > alpha) {
                alpha       = v;
                raisedAlpha = true;
                update_pv(ply, m);
                if (v >= beta) {
                    /* Fail high: the opponent would avoid this line. Everything
                     * tried here learns from it - the move that cut is credited
                     * in whichever table describes it, and everything tried
                     * before it is blamed in both. */
                    update_stats(pos, m, quiets, quietCount, captures, captureCount, depth, ply);
                    break;
                }
            }
        }
    }

    /*
     * No legal move at all: mate if the king is attacked, stalemate if not.
     * Scoring mate by ply is what makes the engine prefer the faster one.
     *
     * Unless a move was excluded, in which case "no moves" means only that the
     * position has exactly one, and the position is neither mate nor stalemate
     * - it is the most singular a move can be. Return alpha so the caller reads
     * a fail low and extends.
     */
    if (moveCount == 0) {
        if (isExcluded)
            return alpha;

        best = inCheck ? mated_in(ply) : VALUE_DRAW;
        tt_store(key, MOVE_NONE, best, rawEval, depth, BOUND_EXACT, ply);
        return best;
    }

    /*
     * A move that raised alpha without failing high proves an exact score,
     * because every alternative was searched and none beat it. At a null-window
     * node that cannot happen - raising alpha there is already a fail high - so
     * only PV nodes ever store an exact entry. Everything else failed low, and
     * all its score proves is that the true value is no higher.
     *
     * `bestMove` is still stored on a fail low. It is only a hint about which
     * move to try first next time, which costs nothing to be wrong about, and
     * it is what stops a re-search of this node starting from scratch.
     */
    /* Never from a singular verification: its move list was missing a move, so
     * its score is not a fact about this position and must not be cached as
     * one. */
    if (!isExcluded) {
        const Bound bound = best >= beta ? BOUND_LOWER : raisedAlpha ? BOUND_EXACT : BOUND_UPPER;
        tt_store(key, bestMove, best, rawEval, depth, bound, ply);

        /*
         * Learn from this node only where the search genuinely contradicted the
         * static evaluation, on the static evaluation's own terms.
         *
         * In check there is no static evaluation to be wrong about. A mate
         * score is not an evaluation error, it is a different kind of fact. A
         * tactical best move means the gap was material that quiescence found
         * rather than a standing bias, and crediting it would teach the table
         * that every structure which once contained a hanging piece is worth a
         * pawn more than it is. And a bound is evidence only in the direction
         * it bounds: a fail high proves the truth is at least `best`, which
         * says nothing at all if the evaluation was already above that.
         */
        if (!inCheck && !is_mate_score(best) &&
            (bestMove == MOVE_NONE || !is_tactical(pos, bestMove)) &&
            !(bound == BOUND_LOWER && best <= staticEval) &&
            !(bound == BOUND_UPPER && best >= staticEval))
            corrhist_update(pos, best, staticEval, depth, ply);
    }

#ifdef DATAGEN
    /* A singular verification is skipped on purpose: its move list was missing
     * a move, so neither the position's value nor its best move is a fact
     * about the position, and a sampler must not learn from one. */
    if (NodeVisitor && !isExcluded) {
        const SearchNodeInfo info = {depth, ply, inCheck, bestMove,
                                     best >= beta  ? NODE_FAIL_HIGH
                                     : raisedAlpha ? NODE_EXACT
                                                   : NODE_FAIL_LOW};
        NodeVisitor(pos, &info, NodeVisitorCtx);
    }
#endif

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
static Value search_root(Position *pos, ScoredMove *roots, int count, Depth depth, Value alpha,
                         Value beta, Move *bestMove) {
    Value best = -VALUE_INFINITE;

    PvLength[0] = 0;

    for (int i = 0; i < count; ++i) {
        const Move m = roots[i].m;

        Stack[0].move       = m;
        Stack[0].movedPiece = piece_on(pos, from_sq(m));
        Stack[0].staticEval = VALUE_NONE; /* no grandparent above the root */

        board_do_move(pos, m);
        eval_state_push(pos, m);
        tt_prefetch(pos->key);

        /*
         * Principal variation search, as in negamax: the first root move
         * establishes alpha with the full window, and every later one only has
         * to answer the cheap question of whether it beats that. The ones that
         * do - rare, once the root list is sorted by the previous iteration -
         * pay for a proper re-search.
         */
        Value v;
        if (i == 0) {
            v = -negamax(pos, depth - 1, -beta, -alpha, 1, false);
        } else {
            /* The expectation behind a null-window root search is that the move
             * is worse than the one already found - which is the child failing
             * high on its own terms. */
            v = -negamax(pos, depth - 1, -alpha - 1, -alpha, 1, true);
            if (v > alpha && v < beta && !search_stopped())
                v = -negamax(pos, depth - 1, -beta, -alpha, 1, false);
        }

        eval_state_pop();
        board_undo_move(pos, m);

        if (search_stopped())
            return VALUE_NONE;

        roots[i].score = (int)v;

        if (v > best) {
            best      = v;
            *bestMove = m;
            update_pv(0, m);

            if (v > alpha)
                alpha = v;

            /* Beat the aspiration window: the caller has to widen and retry,
             * so there is nothing to gain from searching the rest of the list
             * against a bound already known to be wrong. */
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

static void print_iteration(Depth depth, Value value, int64_t elapsed) {
    char buf[8];

    printf("info depth %d seldepth %d ", depth, SelDepth);

    if (is_mate_score(value))
        printf("score mate %d ", mate_in_moves(value));
    else
        printf("score cp %d ", value);

    /* Clamped so a sub-millisecond iteration cannot divide by zero. */
    const int64_t ms = elapsed > 0 ? elapsed : 1;
    printf("nodes %llu nps %llu time %lld hashfull %d pv", (unsigned long long)NodeCount,
           (unsigned long long)((NodeCount * 1000ULL) / (uint64_t)ms), (long long)elapsed,
           tt_hashfull());

    for (int i = 0; i < PvLength[0]; ++i)
        printf(" %s", move_to_str(PvTable[0][i], buf));

    printf("\n");
    fflush(stdout);
}

static Move iterative_deepening(Position *pos, Move *ponderMove) {
    *ponderMove = MOVE_NONE;

    RootScore      = VALUE_NONE;
    CompletedDepth = 0;

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

    Value prevScore = VALUE_NONE;

    /* Consecutive completed iterations that agreed on the best move. Feeds the
     * time manager: a search still changing its mind is one worth letting run
     * a little longer. Tracked against its own previous value rather than
     * against `best`, which starts out holding an unsearched move. */
    int stability = 0;
    Move prevBest = MOVE_NONE;

    for (Depth depth = 1; depth <= maxDepth; ++depth) {
        RootDepth          = depth;
        SelDepth           = 0;
        Move iterationBest = MOVE_NONE;

        /*
         * Aspiration windows.
         *
         * The score at depth N is nearly always close to the score at depth
         * N-1, so searching the whole range from -infinity to +infinity throws
         * away information we already have. A narrow window around the previous
         * score produces far more cutoffs; the price is that when the score
         * does move, the search fails at the window edge and has to be redone.
         *
         * Widening geometrically is what keeps that price bounded - a position
         * whose score is genuinely collapsing reaches a full window in a few
         * re-searches rather than dozens. On a fail low the opposite edge is
         * pulled in towards the score too, because a fail low means the true
         * value is below the window and there is no reason to keep believing
         * the optimistic side of it.
         */
        Value alpha = -VALUE_INFINITE;
        Value beta  = VALUE_INFINITE;
        Value delta = ASPIRATION_DELTA;

        if (depth >= ASPIRATION_MIN_DEPTH && prevScore != VALUE_NONE && !is_mate_score(prevScore)) {
            alpha = prevScore - delta > -VALUE_INFINITE ? prevScore - delta : -VALUE_INFINITE;
            beta  = prevScore + delta < VALUE_INFINITE ? prevScore + delta : VALUE_INFINITE;
        }

        Value value;
        for (;;) {
            value = search_root(pos, roots, rootCount, depth, alpha, beta, &iterationBest);

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

        /* An interrupted iteration searched some moves under a window the
         * others never saw, so its ordering is meaningless. Keep the last
         * completed iteration's move instead. */
        if (search_stopped())
            break;

        stability = (prevBest != MOVE_NONE && iterationBest == prevBest) ? stability + 1 : 0;
        prevBest  = iterationBest;

        prevScore      = value;
        best           = iterationBest;
        RootScore      = value;
        CompletedDepth = depth;

        /* Cleared, not left alone, when this iteration has no second PV move:
         * the stale entry belongs to a line the search has since abandoned, and
         * pondering on a move that no longer follows `best` wastes the whole
         * ponder search and desynchronises the GUI on ponderhit. */
        *ponderMove = PvLength[0] > 1 ? PvTable[0][1] : MOVE_NONE;

        if (!Silent)
            print_iteration(depth, value, elapsed_ms());
        sort_root_moves(roots, rootCount);

        if (Limits.mate && is_mate_score(value) && value > 0 && mate_in_moves(value) <= Limits.mate)
            break;

        /*
         * Do not begin an iteration there is no realistic chance of finishing.
         * Each one costs several times the last, so starting one at 90% of the
         * budget just burns the remainder and throws the result away.
         */
        if (!Limits.infinite && !atomic_load(&Pondering) &&
            elapsed_ms() >= timeman_optimum(&Timer, stability))
            break;
    }

    atomic_store(&Nodes, NodeCount);
    return best;
}

/* ------------------------------------------------------------ lifecycle -- */

/* One search over RootPos under Limits, shared by the worker thread and by
 * search_run_sync so the two can never drift apart. */
static Move run_search(Move *ponderMove) {
    NodeCount = 0;
    SelDepth  = 0;
    atomic_store(&Nodes, 0);

    /* Per-search state, not per-game: a stale excluded move or a stale
     * grandparent evaluation left here by the previous search would make this
     * one depend on it. */
    memset(Stack, 0, sizeof(Stack));

    *ponderMove = MOVE_NONE;
    return iterative_deepening(&RootPos, ponderMove);
}

static void worker_entry(void *arg) {
    (void)arg;

    Move ponderMove;
    Move best = run_search(&ponderMove);

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
    Silent = false;

    WorkerStarted = thread_create(&Worker, worker_entry, NULL);
    if (!WorkerStarted) {
        /* Thread creation failed. Searching inline still produces a legal game
         * - it just cannot be interrupted - which beats not moving at all. */
        worker_entry(NULL);
    }
}

void search_run_sync(const Position *pos, const SearchLimits *limits, SearchResult *out) {
    search_wait(); /* never run two searches at once */

    RootPos = *pos;
    Limits  = *limits;

    atomic_store(&StopFlag, false);
    /* A synchronous ponder search would wait for a `stop` that no one is
     * around to send, so the flag is dropped rather than honoured. */
    atomic_store(&Pondering, false);
    atomic_store(&ClockOrigin, limits->startTime);
    atomic_store(&Searching, true);
    Silent = true;

    Move ponderMove;
    const Move best = run_search(&ponderMove);

    Silent = false;
    atomic_store(&Searching, false);

    out->best  = best;
    out->score = RootScore;
    out->depth = CompletedDepth;
    out->nodes = NodeCount;
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
