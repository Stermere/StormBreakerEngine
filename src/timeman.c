/*
 * timeman.c - monotonic clock and time allocation.
 */
#include "timeman.h"

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
    static LARGE_INTEGER frequency;
    LARGE_INTEGER counter;

    if (frequency.QuadPart == 0)
        QueryPerformanceFrequency(&frequency);

    QueryPerformanceCounter(&counter);
    return (int64_t)((counter.QuadPart * 1000) / frequency.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

/* Never plan to use more than this fraction of the remaining clock on one
 * move, however good the reason. Flagging loses the whole game; thinking for
 * one move too briefly costs a fraction of one. */
#define MAX_CLOCK_FRACTION_NUM 4
#define MAX_CLOCK_FRACTION_DEN 5

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

    const int64_t remaining = clamp64(limits->time[us] - uci_move_overhead(), 1, INT64_MAX);
    const int64_t inc       = limits->inc[us] > 0 ? limits->inc[us] : 0;

    /*
     * Sudden death plus increment: assume the game has about twenty moves left
     * in it and bank three quarters of the increment, keeping a little back so
     * a stream of slightly-over-budget moves cannot drain the clock.
     *
     * With a move count to the next control, divide by that instead - but cap
     * it, because dividing by 40 early in a long control just wastes time that
     * would have been better spent on the moves that decide the game.
     */
    const int64_t moves =
        limits->movestogo > 0
            ? (limits->movestogo < MOVESTOGO_CAP ? limits->movestogo : MOVESTOGO_CAP)
            : 20;
    const int64_t nominal = remaining / moves + inc * 3 / 4;
    const int64_t optimum = nominal * phase_percent(gamePly) / 100;
    const int64_t ceiling = remaining * MAX_CLOCK_FRACTION_NUM / MAX_CLOCK_FRACTION_DEN;

    /* The ceiling is computed off the nominal allocation rather than the
     * phase-scaled one, so the phase curve moves the target without also
     * moving the hard limit that protects the clock. */
    const int64_t maximum = nominal * 5 < ceiling ? nominal * 5 : ceiling;

    /* Both are clamped into [1, remaining]: an allocation larger than the
     * clock is how engines lose on time in won positions. */
    tm->maximum = clamp64(maximum, 1, remaining);
    tm->optimum = clamp64(optimum, 1, tm->maximum);
}
