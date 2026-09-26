/* timeman.c - monotonic clock and time allocation. */
#include "timeman.h"

#include <stdatomic.h>

#include "uci.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <time.h>
#endif

int64_t time_ms(void) {
#if defined(_WIN32)
    /* QueryPerformanceCounter rather than GetTickCount64, whose ~15ms granularity is
     * coarse enough to distort nps at SPRT time controls. The frequency is fixed for
     * the life of the process and _Atomic makes the lazy publication well defined -
     * both threads racing to fill it store the identical value. */
    static _Atomic int64_t frequency;
    LARGE_INTEGER counter;

    int64_t hz = atomic_load_explicit(&frequency, memory_order_relaxed);
    if (hz == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        hz = (int64_t)f.QuadPart;
        atomic_store_explicit(&frequency, hz, memory_order_relaxed);
    }

    QueryPerformanceCounter(&counter);
    return (int64_t)((counter.QuadPart * 1000) / hz);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

/* Never plan to use more than this fraction of the clock above the bank on one move:
 * flagging loses the whole game, where thinking briefly for one move costs a fraction
 * of one. It only bites once the clock has fallen to around fifteen times the
 * increment, so its job is leaving a clock to play the next move from when this one
 * went badly. */
#define MAX_CLOCK_FRACTION_NUM 1
#define MAX_CLOCK_FRACTION_DEN 2

/*
 * The part of the clock no plan may touch, in milliseconds - held back from the
 * fraction above, so the ceiling is half of what lies ABOVE it.
 *
 * Without it the ceiling was half the clock however little was left. Once the clock is
 * low the allocation settles on three quarters of the increment, the stability and
 * node-share scales push the soft target up to the ceiling on every unsettled move, and
 * each such move spends half of what remains - so at 8+0.08 the clock came to rest near
 * two increments, and fell below 0.2s in one game in six (E43). There, a move that
 * arrives 150ms late forfeits.
 *
 * And moves do arrive late, through no fault of the search: logged under full match
 * load, the search stopped within 2ms of its ceiling at the 99.9th percentile, while the
 * GUI measured one move in 440 at least 50ms longer than the engine did - time lost
 * waking to read `go` and getting `bestmove` back out, which no clock inside the
 * process can see. So the bank is fixed rather than proportional to the clock or the
 * increment: a scheduling delay's size depends on neither.
 */
#define CLOCK_BANK_MS 300

/* The exception that lets the bound above be strict: when `movestogo` says this is
 * the last move before the clock is replenished, no further move comes out of it.
 * Still held short of the whole clock, because the search stops on a poll. */
#define LAST_MOVE_FRACTION_NUM 4
#define LAST_MOVE_FRACTION_DEN 5

/* How far past the nominal allocation one move may run when the search will not
 * settle. MAX_CLOCK_FRACTION above is what stops it becoming a licence to spend the
 * whole clock late in a game. */
#define MAX_NOMINAL_MULT 5

/*
 * What the clock is divided by when nothing narrows it: the cap on `movestogo`, and
 * the whole horizon without one. Not a guess at the game's length - a fraction of what
 * is *left* is spent every move, so this is the rate the clock decays at.
 *
 * Twenty decayed too fast in both directions. With no increment it is a geometric
 * slide - 2.5s on move 1 at 60+0, 11ms by move 80 - so the phase that has to be
 * converted is played on moves too short to search. With one it drains the bank to
 * about `moves / 4` times the increment, 400ms at 8+0.08, and every sharp position
 * after that gets whatever the increment happens to pay.
 *
 * Fifty spends under half as much before move 20 and more than twenty did on every
 * move after about 25, because the time is still there to spend. The opening pays for
 * it, which is where the phase curve below already says the least is at stake.
 */
#define MOVESTOGO_CAP 50

static int64_t clamp64(int64_t v, int64_t lo, int64_t hi) { return v < lo ? lo : v > hi ? hi : v; }
static int64_t imin64(int64_t a, int64_t b) { return a < b ? a : b; }
static int64_t imax64(int64_t a, int64_t b) { return a > b ? a : b; }

/* Percent of the nominal allocation to spend, by how far into the game we are. The
 * opening is near-book and shared by every game, the middlegame is where an extra ply
 * changes the move, and a simplified late position reaches a useful depth sooner, so
 * what is saved there banks against the scramble. Indexed by gamePly / 16. */
static const int PhasePercent[] = {85, 100, 110, 110, 105, 100, 95, 90, 85, 80};

#define PHASE_BUCKETS ((int)(sizeof(PhasePercent) / sizeof(PhasePercent[0])))

static int phase_percent(int gamePly) {
    int bucket = gamePly / 16;

    if (bucket < 0)
        bucket = 0;
    else if (bucket >= PHASE_BUCKETS)
        bucket = PHASE_BUCKETS - 1;

    return PhasePercent[bucket];
}

/* Percent of the optimum, by how many iterations the best move has survived. Falls
 * off quickly then flattens: "changed last iteration" against "stable for two" is
 * real information, stable for eight against twelve is not. */
static const int StabilityPercent[] = {170, 130, 110, 100, 92, 86, 82, 80};

#define STABILITY_BUCKETS ((int)(sizeof(StabilityPercent) / sizeof(StabilityPercent[0])))

/* Percent of the optimum by the share of the search's nodes the best move took, as
 * (BASE - share) * SCALE with the share in thousandths: a move that took 90% of the tree
 * spends 67%, one that took 40% spends 135%. BASE puts the neutral point on the share at
 * which STC searches actually stop - a mean of 670 over 60 book positions at 6+0.08 -
 * so this moves time between moves rather than changing how much a game uses. The
 * textbook 1500 would have lengthened the average think by 11%, which is a second
 * change riding on the first. */
#define NODE_SHARE_BASE  1400
#define NODE_SHARE_SCALE 135

bool timeman_has_clock(const TimeManager *tm) { return tm->optimum < INT64_MAX / 256; }

int64_t timeman_optimum(const TimeManager *tm, int stability, int bestNodesPermille) {
    /* No clock at all - `go depth`, `go nodes`, `go infinite`, bench. Scaling a
     * sentinel would overflow, and there is nothing here to scale. */
    if (!timeman_has_clock(tm))
        return tm->optimum;

    if (stability < 0)
        stability = 0;
    else if (stability >= STABILITY_BUCKETS)
        stability = STABILITY_BUCKETS - 1;

    const int64_t share       = clamp64(bestNodesPermille, 0, 1000);
    const int64_t nodePercent = (NODE_SHARE_BASE - share) * NODE_SHARE_SCALE / 1000;

    const int64_t scaled = tm->optimum * StabilityPercent[stability] * nodePercent / 10000;

    /* The soft target may never cross the hard ceiling. */
    return scaled < tm->maximum ? scaled : tm->maximum;
}

void timeman_init(TimeManager *tm, const SearchLimits *limits, Color us, int gamePly) {
    /* A fixed movetime is an instruction, not a budget: honour it exactly, less the
     * overhead reserved for GUI and network latency. Keyed on the field being PRESENT,
     * so `go movetime 0` asks for a move now and gets the one-millisecond floor below
     * rather than a search with no deadline. */
    if (limits->movetimeGiven) {
        tm->optimum = tm->maximum = clamp64(limits->movetime - uci_move_overhead(), 1, INT64_MAX);
        return;
    }

    /* No clock was given - `go depth`, `go nodes`, `go infinite` and bench all land
     * here. The search enforces those limits, and a time limit on top of them would
     * make bench non-deterministic. */
    if (!limits->timeGiven) {
        tm->optimum = tm->maximum = INT64_MAX;
        return;
    }

    /* A move count to the next control divides the clock directly, capped because
     * dividing by 40 early in a long control wastes time on moves that do not decide the
     * game. Without one, that cap is the horizon. */
    const int64_t moves = limits->movestogo > 0 && limits->movestogo < MOVESTOGO_CAP
                              ? limits->movestogo
                              : MOVESTOGO_CAP;

    /* A clock at or below zero is one that has already run out. Everything below still
     * runs on it, and every clamp is a floor of one millisecond, so the budget comes out
     * at 1ms and the search returns the move the root list already holds. Answering at
     * once is the only useful thing left to do; thinking about it is the behaviour this
     * floor exists to prevent. */
    const int64_t clock    = limits->time[us] > 0 ? limits->time[us] : 0;
    const int64_t overhead = uci_move_overhead();
    const int64_t inc      = limits->inc[us] > 0 ? limits->inc[us] : 0;

    /*
     * Move Overhead is owed on every move still to be played, not only on this one.
     * Spending `remaining / moves + 3/4 inc` and being paid `inc` back is a
     * contraction, so the clock converges rather than decays - on `moves / 4` times the
     * increment, which at 8+0.08 is a second with every later move's latency still to
     * come out of it.
     *
     * Reserving the latency up front moves that convergence point up by the whole
     * reserve, so a declared 100ms overhead buys a wide margin without anyone asking.
     * Capped at half the clock, so a large declared overhead cannot leave nothing to
     * divide.
     */
    const int64_t reserve = clamp64(overhead * (moves + 2), 0, clock / 2);

    /* Two different questions: `spendable` is what this move can physically use without
     * flagging, `budget` what it may use given that the rest of the game comes out of
     * the same clock. */
    const int64_t spendable = clamp64(clock - overhead, 1, INT64_MAX);
    const int64_t budget    = clamp64(clock - reserve, 1, INT64_MAX);

    const int64_t nominal = budget / moves + inc * 3 / 4;
    const int64_t optimum = nominal * phase_percent(gamePly) / 100;

    /* Inside the bank, what refills it: three quarters of the increment, so the clock
     * climbs back by the last quarter each move, plus a share of the clock so a control
     * without one still moves rather than answering in a millisecond. Never more than a
     * quarter of what is physically left, which is what still refills a clock that is
     * nearly gone. Half the increment refilled faster and measured -1.06 +/- 6.50 over
     * 2956 games (E43); the moves spent refilling are the only price the bank has. */
    const int64_t refill = imin64(inc * 3 / 4 + spendable / (2 * moves), spendable / 4);
    const int64_t ceiling =
        moves > 1
            ? imax64((spendable - CLOCK_BANK_MS) * MAX_CLOCK_FRACTION_NUM / MAX_CLOCK_FRACTION_DEN,
                     refill)
            : spendable * LAST_MOVE_FRACTION_NUM / LAST_MOVE_FRACTION_DEN;

    /* The ceiling applies to the nominal allocation rather than the phase-scaled one, so
     * the phase curve moves the target without moving the limit that protects the
     * clock. */
    const int64_t maximum =
        nominal * MAX_NOMINAL_MULT < ceiling ? nominal * MAX_NOMINAL_MULT : ceiling;

    /* An allocation larger than the clock is how engines lose on time in won
     * positions. */
    tm->maximum = clamp64(maximum, 1, spendable);
    tm->optimum = clamp64(optimum, 1, tm->maximum);
}
