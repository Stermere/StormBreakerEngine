/*
 * smptest.c - the parallel-search gate.
 *
 * Everything here is a claim `make bench` cannot make. Bench pins one thread on
 * purpose (invariant 1), so it says nothing at all about the pool: whether it
 * starts, whether the helpers actually search, whether resizing it mid-session is
 * safe, and - the one that matters most - whether a session that used it can
 * still be measured afterwards.
 *
 * That last check is the reason this file exists. A parallel search is not
 * reproducible and cannot be gated on a node count, but the search AFTER it can:
 * if returning to one thread does not reproduce the one-thread node counts to the
 * node, then something the pool touched is still there, and every bench and every
 * SPRT taken in that session afterwards is measuring an engine nobody described.
 */
#include "smptest.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "../board.h"
#include "../search.h"
#include "../thread.h"
#include "../tt.h"

/* An opening, a sharp middlegame and a pawn endgame, because a pool bug that only
 * shows where the tree is wide is exactly the kind one opening position hides. */
static const char *const Positions[] = {
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
    "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
};

#define POSITION_COUNT ((int)(sizeof(Positions) / sizeof(Positions[0])))

/* Shallow enough that the gate costs seconds, deep enough that the pool has
 * several iterations to disagree over. */
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

/*
 * One fixed-depth search from a cleared table, driven through search_start() and
 * search_wait() - the path a GUI takes - so the gate cannot pass on a code path
 * the engine never uses. Returns the whole pool's nodes, or 0 if the FEN failed.
 */
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

    /* Enough threads to be a real pool, but not so many that a CI runner spends the
     * gate context-switching. Four is the floor because the skip patterns only begin
     * to differ from one another above two. */
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

    /* Several sizes, one of them not a power of two: the skip schedule indexes by
     * thread id, and an off-by-one there shows up at an odd count and nowhere else. */
    const int sizes[] = {2, 3, threads};

    for (int s = 0; s < (int)(sizeof(sizes) / sizeof(sizes[0])); ++s) {
        const int want = sizes[s];
        search_set_threads(want);

        const int got = search_threads();
        snprintf(detail, sizeof(detail), "asked %d, got %d", want, got);
        report("the pool resizes to what was asked", got == want, detail);

        uint64_t nodes[POSITION_COUNT];
        const uint64_t total = search_all(nodes);

        /*
         * Not a node count - a parallel one is not reproducible - but a claim that is
         * still exact. Every position here is searched from a cleared table to a fixed
         * depth, so a pool whose helpers were never released would reproduce the
         * single-threaded total to the node. More than it means they searched.
         */
        snprintf(detail, sizeof(detail), "%d threads: %llu nodes against %llu at one", got,
                 (unsigned long long)total, (unsigned long long)singleTotal);
        report("the helpers actually search", total > singleTotal, detail);
    }

    /*
     * The gate this file exists for. Back to one thread, same positions, same cleared
     * table: the node counts have to be the ones from before the pool ran, exactly.
     */
    search_set_threads(1);
    search_all(after);

    for (int i = 0; i < POSITION_COUNT; ++i) {
        snprintf(detail, sizeof(detail), "position %d: %llu before, %llu after", i + 1,
                 (unsigned long long)before[i], (unsigned long long)after[i]);
        report("one thread again reproduces the baseline exactly", before[i] == after[i], detail);
    }

    /* Left where a session expects to find it. */
    search_set_threads(1);

    printf("smp: %d checks, %d failures\n", Checks, Failures);
    fflush(stdout);

    /* Nonzero when nothing ran, so an empty gate cannot pass by testing nothing. */
    return Checks == 0 ? 1 : Failures;
}
