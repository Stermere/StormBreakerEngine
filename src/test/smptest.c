/*
 * smptest.c - the parallel-search gate. Bench pins one thread, so this checks the pool:
 * that it resizes, that the helpers search, and above all that one thread afterwards
 * reproduces its node counts exactly, so nothing the pool touched survives.
 */
#include "smptest.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "../board.h"
#include "../search.h"
#include "../thread.h"
#include "../tt.h"

/* An opening, a sharp middlegame and a pawn endgame. */
static const char *const Positions[] = {
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
    "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
};

#define POSITION_COUNT ((int)(sizeof(Positions) / sizeof(Positions[0])))

#define GATE_DEPTH 10

static int Failures;
static int Checks;

static void report(const char *what, bool ok, const char *detail) {
    ++Checks;
    if (!ok)
        ++Failures;

    printf("%-4s %-54s %s\n", ok ? "ok" : "FAIL", what, detail ? detail : "");
    fflush(stdout);
}

/* A fixed-depth search from a cleared table, through search_start() like a GUI. Returns
 * the pool's nodes, or 0 if the FEN failed. */
static uint64_t search_position(const char *fen) {
    Position pos;
    memset(&pos, 0, sizeof(pos));

    if (!board_set_fen(&pos, fen))
        return 0;

    tt_clear();
    search_clear();

    SearchLimits limits;
    search_limits_clear(&limits);
    limits.depth = (Depth)GATE_DEPTH;

    search_start(&pos, &limits);
    search_wait();

    return search_nodes();
}

static uint64_t search_all(uint64_t *out) {
    uint64_t total = 0;

    for (int i = 0; i < POSITION_COUNT; ++i) {
        out[i] = search_position(Positions[i]);
        total += out[i];
    }
    return total;
}

int smp_selftest(int maxThreads) {
    Failures = 0;
    Checks   = 0;

    const int hardware = thread_hardware_concurrency();

    /* At least 4, where the skip patterns start to differ; at most 32, for CI runners. */
    int threads = maxThreads > 0 ? maxThreads : hardware;
    if (threads < 4)
        threads = 4;
    if (threads > 32)
        threads = 32;

    char detail[160];
    snprintf(detail, sizeof(detail), "%d logical processors, testing up to %d threads", hardware,
             threads);
    report("the machine's size is visible", hardware >= 1, detail);

    if (!tt_resize(16)) {
        report("16 MB hash for the gate", false, "allocation failed");
        return Failures;
    }

    uint64_t before[POSITION_COUNT];
    uint64_t after[POSITION_COUNT];

    search_set_threads(1);
    const uint64_t singleTotal = search_all(before);

    snprintf(detail, sizeof(detail), "%llu nodes over %d positions at depth %d",
             (unsigned long long)singleTotal, POSITION_COUNT, GATE_DEPTH);
    report("a single-threaded baseline", singleTotal > 0, detail);

    /* Including an odd size, where an off-by-one in the skip schedule would show. */
    const int sizes[] = {2, 3, threads};

    for (int s = 0; s < (int)(sizeof(sizes) / sizeof(sizes[0])); ++s) {
        const int want = sizes[s];
        search_set_threads(want);

        const int got = search_threads();
        snprintf(detail, sizeof(detail), "asked %d, got %d", want, got);
        report("the pool resizes to what was asked", got == want, detail);

        uint64_t nodes[POSITION_COUNT];
        const uint64_t total = search_all(nodes);

        /* Idle helpers would reproduce the single-threaded total exactly; more means they
         * searched. */
        snprintf(detail, sizeof(detail), "%d threads: %llu nodes against %llu at one", got,
                 (unsigned long long)total, (unsigned long long)singleTotal);
        report("the helpers actually search", total > singleTotal, detail);
    }

    /* The main check: back at one thread, the counts must match the baseline exactly. */
    search_set_threads(1);
    search_all(after);

    for (int i = 0; i < POSITION_COUNT; ++i) {
        snprintf(detail, sizeof(detail), "position %d: %llu before, %llu after", i + 1,
                 (unsigned long long)before[i], (unsigned long long)after[i]);
        report("one thread again reproduces the baseline exactly", before[i] == after[i], detail);
    }

    search_set_threads(1);

    printf("smp: %d checks, %d failures\n", Checks, Failures);
    fflush(stdout);

    return Checks == 0 ? 1 : Failures;
}
