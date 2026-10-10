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
    /* Not GetTickCount64: ~15ms granularity. Racing threads store the same frequency. */
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

/* The hard ceiling: at most this fraction of the clock above the bank on one move. */
#define MAX_CLOCK_FRACTION_NUM 1
#define MAX_CLOCK_FRACTION_DEN 2

/* Never planned for. Fixed rather than proportional, because it covers process and GUI
 * latency the engine cannot see (one move in 440 arrived 50ms+ late under match load).
 * Without it the clock settled near two increments at 8+0.08 (E43). */
#define CLOCK_BANK_MS 300

/* The last move before `movestogo` replenishes the clock may use more of it. */
#define LAST_MOVE_FRACTION_NUM 4
#define LAST_MOVE_FRACTION_DEN 5

/* How far past the nominal allocation an unsettled search may run. */
#define MAX_NOMINAL_MULT 5

/* The divisor of the remaining clock, and the cap on `movestogo`. It sets how fast the
 * clock decays; 20 left too little for the endgame, with or without increment. */
#define MOVESTOGO_CAP 50

static int64_t clamp64(int64_t v, int64_t lo, int64_t hi) { return v < lo ? lo : v > hi ? hi : v; }
static int64_t imin64(int64_t a, int64_t b) { return a < b ? a : b; }
static int64_t imax64(int64_t a, int64_t b) { return a > b ? a : b; }

/* Percent of the nominal allocation by gamePly / 16: most in the middlegame. */
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

/* Percent of the optimum by how many iterations the best move has held. */
static const int StabilityPercent[] = {170, 130, 110, 100, 92, 86, 82, 80};

#define STABILITY_BUCKETS ((int)(sizeof(StabilityPercent) / sizeof(StabilityPercent[0])))

/* Percent of the optimum by the best move's node share (thousandths): (BASE - share) *
 * SCALE / 1000. Neutral at the average share where STC searches stop, so it moves time
 * between moves without changing the total. */
#define NODE_SHARE_BASE  1400
#define NODE_SHARE_SCALE 135

bool timeman_has_clock(const TimeManager *tm) { return tm->optimum < INT64_MAX / 256; }

int64_t timeman_optimum(const TimeManager *tm, int stability, int bestNodesPermille) {
    /* Scaling the no-clock sentinel would overflow. */
    if (!timeman_has_clock(tm))
        return tm->optimum;

    if (stability < 0)
        stability = 0;
    else if (stability >= STABILITY_BUCKETS)
        stability = STABILITY_BUCKETS - 1;

    const int64_t share       = clamp64(bestNodesPermille, 0, 1000);
    const int64_t nodePercent = (NODE_SHARE_BASE - share) * NODE_SHARE_SCALE / 1000;

    const int64_t scaled = tm->optimum * StabilityPercent[stability] * nodePercent / 10000;

    return scaled < tm->maximum ? scaled : tm->maximum;
}

void timeman_init(TimeManager *tm, const SearchLimits *limits, Color us, int gamePly) {
    /* Exact, less the overhead. Keyed on presence: `go movetime 0` gets the 1ms floor. */
    if (limits->movetimeGiven) {
        tm->optimum = tm->maximum = clamp64(limits->movetime - uci_move_overhead(), 1, INT64_MAX);
        return;
    }

    /* No clock (depth, nodes, infinite, bench): a time limit would break bench. */
    if (!limits->timeGiven) {
        tm->optimum = tm->maximum = INT64_MAX;
        return;
    }

    const int64_t moves = limits->movestogo > 0 && limits->movestogo < MOVESTOGO_CAP
                              ? limits->movestogo
                              : MOVESTOGO_CAP;

    /* A clock at or below zero gives the 1ms floor everywhere: move at once. */
    const int64_t clock    = limits->time[us] > 0 ? limits->time[us] : 0;
    const int64_t overhead = uci_move_overhead();
    const int64_t inc      = limits->inc[us] > 0 ? limits->inc[us] : 0;

    /* Move Overhead is owed on every move still to come, so reserve it up front, capped at
     * half the clock. */
    const int64_t reserve = clamp64(overhead * (moves + 2), 0, clock / 2);

    /* `spendable` is what this move can use without flagging; `budget` is what it may use
     * given the rest of the game. */
    const int64_t spendable = clamp64(clock - overhead, 1, INT64_MAX);
    const int64_t budget    = clamp64(clock - reserve, 1, INT64_MAX);

    /* Three quarters of the increment: the whole of it measured flat (E48, E49). */
    const int64_t nominal = budget / moves + inc * 3 / 4;
    const int64_t optimum = nominal * phase_percent(gamePly) / 100;

    /* The floor under the ceiling once the clock is inside the bank: 3/4 of the increment
     * plus a share of the clock, at most a quarter of what is left, so the bank refills. */
    const int64_t refill = imin64(inc * 3 / 4 + spendable / (2 * moves), spendable / 4);
    const int64_t ceiling =
        moves > 1
            ? imax64((spendable - CLOCK_BANK_MS) * MAX_CLOCK_FRACTION_NUM / MAX_CLOCK_FRACTION_DEN,
                     refill)
            : spendable * LAST_MOVE_FRACTION_NUM / LAST_MOVE_FRACTION_DEN;

    /* From the nominal allocation, so the phase curve moves the target but not the limit. */
    const int64_t maximum =
        nominal * MAX_NOMINAL_MULT < ceiling ? nominal * MAX_NOMINAL_MULT : ceiling;

    tm->maximum = clamp64(maximum, 1, spendable);
    tm->optimum = clamp64(optimum, 1, tm->maximum);
}
