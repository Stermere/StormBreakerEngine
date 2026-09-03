/*
 * timeman.c - monotonic clock and time allocation.
 */
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
    /* QueryPerformanceCounter rather than GetTickCount64: the latter has a
     * ~15ms granularity, which is coarse enough to distort nps readings and to
     * matter at the ultra-fast time controls used for SPRT testing. */
    /*
     * The frequency is fixed for the life of the process, so caching it is
     * safe - but the cache is read by both the UCI thread and the search
     * worker, and a plain lazily-initialised static would be a data race the
     * sanitiser build reports. _Atomic makes the publication well defined;
     * both threads racing to fill it store the identical value, so the
     * unsynchronised check-then-set below needs nothing stronger.
     */
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

/* Never plan to use more than this fraction of the clock on one move, however
 * good the reason. Flagging loses the whole game; thinking for one move too
 * briefly costs a fraction of one.
 *
 * This bound only bites once the clock has fallen to somewhere around fifteen
 * times the increment - above that `MAX_NOMINAL_MULT` is the smaller of the
 * two and this one never applies - so it has exactly one job: leaving a clock
 * to play the next move from when this move went badly. At the clock this
 * allocator converges on, anything looser is the difference between a scramble
 * and a loss. */
#define MAX_CLOCK_FRACTION_NUM 1
#define MAX_CLOCK_FRACTION_DEN 2

/* The exception, and the reason the bound above can afford to be strict: when
 * `movestogo` says this is the last move before the clock is replenished, no
 * further move comes out of it and there is nothing left to protect. Held
 * short of the whole clock anyway, because the search stops on a poll and a
 * plan to use every last millisecond has no room to be slightly wrong. */
#define LAST_MOVE_FRACTION_NUM 4
#define LAST_MOVE_FRACTION_DEN 5

/* How far past the nominal allocation one move may run when the search will
 * not settle. Bounded by MAX_CLOCK_FRACTION above, which is what stops this
 * from being a licence to spend the whole clock late in a game. */
#define MAX_NOMINAL_MULT 5

/* Beyond this many moves to the time control the horizon stops mattering, and
 * treating it as sudden death allocates better than dividing by a huge N. */
#define MOVESTOGO_CAP 50

static int64_t clamp64(int64_t v, int64_t lo, int64_t hi) { return v < lo ? lo : v > hi ? hi : v; }

/*
 * How much of the nominal allocation to spend, in percent, as a function of
 * how far into the game we are.
 *
 * The shape is deliberate. The first few moves are near-book and the position
 * is the one every game shares, so thinking hard there buys the least. The
 * middlegame is where games are decided and where an extra ply changes the
 * move, so it gets the most. Deep into the game the position is simplified,
 * the branching factor has collapsed, and the search reaches a useful depth
 * far sooner - so the same quality of move costs less time, and what is saved
 * is better banked against the scramble at the end.
 *
 * Indexed by gamePly / 16, i.e. roughly every eight full moves.
 */
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

/*
 * Percent of the optimum to spend, by how many iterations running the best
 * move has survived. Falls off quickly and then flattens: the difference
 * between "changed last iteration" and "stable for two" is real information,
 * the difference between stable for eight and stable for twelve is not.
 */
static const int StabilityPercent[] = {170, 130, 110, 100, 92, 86, 82, 80};

#define STABILITY_BUCKETS ((int)(sizeof(StabilityPercent) / sizeof(StabilityPercent[0])))

int64_t timeman_optimum(const TimeManager *tm, int stability) {
    /* No clock at all - `go depth`, `go nodes`, `go infinite`, bench. Scaling
     * a sentinel would overflow, and there is nothing here to scale. */
    if (tm->optimum >= INT64_MAX / 256)
        return tm->optimum;

    if (stability < 0)
        stability = 0;
    else if (stability >= STABILITY_BUCKETS)
        stability = STABILITY_BUCKETS - 1;

    const int64_t scaled = tm->optimum * StabilityPercent[stability] / 100;

    /* The soft target may never cross the hard ceiling. */
    return scaled < tm->maximum ? scaled : tm->maximum;
}

void timeman_init(TimeManager *tm, const SearchLimits *limits, Color us, int gamePly) {
    /* A fixed movetime is an instruction, not a budget: honour it exactly,
     * less the overhead reserved for GUI and network latency. */
    if (limits->movetime > 0) {
        tm->optimum = tm->maximum = clamp64(limits->movetime - uci_move_overhead(), 1, INT64_MAX);
        return;
    }

    /* No clock was given at all - `go depth`, `go nodes`, `go infinite` and
     * bench all land here. Those limits are enforced by the search, and
     * imposing a time limit on top of them would make bench non-deterministic. */
    if (limits->time[us] <= 0) {
        tm->optimum = tm->maximum = INT64_MAX;
        return;
    }

    /*
     * Sudden death plus increment: assume the game has about twenty moves left
     * in it, keeping a little of the increment back so a stream of
     * slightly-over-budget moves cannot drain the clock.
     *
     * With a move count to the next control, divide by that instead - but cap
     * it, because dividing by 40 early in a long control just wastes time that
     * would have been better spent on the moves that decide the game.
     */
    const int64_t moves =
        limits->movestogo > 0
            ? (limits->movestogo < MOVESTOGO_CAP ? limits->movestogo : MOVESTOGO_CAP)
            : 20;

    const int64_t clock    = limits->time[us];
    const int64_t overhead = uci_move_overhead();
    const int64_t inc      = limits->inc[us] > 0 ? limits->inc[us] : 0;

    /*
     * Move Overhead is owed on every move still to be played, not only on this
     * one.
     *
     * Charging it once is right for the move in hand and wrong for the game.
     * Spending `remaining / moves + 3/4 inc` and being paid `inc` back is a
     * contraction, so the clock does not decay - it converges, on five times
     * the increment. At 8+0.08 that is around 400ms in hand for the rest of
     * the game, with the latency of every one of those moves still to come out
     * of it, and nothing left to absorb a slow GUI round trip or one search
     * that overshoots.
     *
     * Reserving the latency up front moves that convergence point up by the
     * whole reserve, in proportion to how much latency was declared - a
     * network game with a 100ms overhead gets a wide margin without anyone
     * having to ask for one. Capped at half the clock, so a large declared
     * overhead cannot leave the allocator with no budget to divide.
     */
    const int64_t reserve = clamp64(overhead * (moves + 2), 0, clock / 2);

    /* Two different questions. `spendable` is what this move can physically
     * use without flagging; `budget` is what it may use given that the rest of
     * the game comes out of the same clock. The allocation is divided out of
     * the budget, and the hard ceiling is measured against what is really
     * there. */
    const int64_t spendable = clamp64(clock - overhead, 1, INT64_MAX);
    const int64_t budget    = clamp64(clock - reserve, 1, INT64_MAX);

    const int64_t nominal = budget / moves + inc * 3 / 4;
    const int64_t optimum = nominal * phase_percent(gamePly) / 100;
    const int64_t ceiling = moves > 1 ? spendable * MAX_CLOCK_FRACTION_NUM / MAX_CLOCK_FRACTION_DEN
                                      : spendable * LAST_MOVE_FRACTION_NUM / LAST_MOVE_FRACTION_DEN;

    /* The ceiling is applied to the nominal allocation rather than the
     * phase-scaled one, so the phase curve moves the target without also
     * moving the hard limit that protects the clock. */
    const int64_t maximum =
        nominal * MAX_NOMINAL_MULT < ceiling ? nominal * MAX_NOMINAL_MULT : ceiling;

    /* Both are clamped into [1, spendable]: an allocation larger than the
     * clock is how engines lose on time in won positions. */
    tm->maximum = clamp64(maximum, 1, spendable);
    tm->optimum = clamp64(optimum, 1, tm->maximum);
}
