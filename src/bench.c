/*
 * bench.c - deterministic node-count benchmark. See bench.h for why the
 * determinism requirement is strict.
 */
#include "bench.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "search.h"
#include "timeman.h"
#include "tt.h"

/*
 * A fixed, diverse set of positions: openings, quiet middlegames, tactical
 * middlegames, and endgames of several material configurations. Breadth
 * matters more than count - a bench dominated by one phase will happily report
 * a speedup for a patch that only helps that phase.
 *
 * Treat this list as frozen. Changing it changes every historical node count,
 * so old bench numbers stop being comparable to new ones.
 */
static const char *BenchPositions[] = {
    /* --- opening and early middlegame --- */
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    "r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R b KQkq - 3 3",
    "r1bqkb1r/pp3ppp/2n1pn2/2pp4/3P1B2/2PBPN2/PP3PPP/RN1QK2R w KQkq - 0 7",
    "rnbqkb1r/pp2pppp/3p1n2/8/3NP3/2N5/PPP2PPP/R1BQKB1R b KQkq - 0 6",
    "r1bqk2r/pppp1ppp/2n2n2/2b1p3/2B1P3/3P1N2/PPP2PPP/RNBQK2R w KQkq - 0 5",

    /* --- rich middlegames --- */
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
    "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
    "2kr3r/pppq1ppp/2n1bn2/1B2p3/4P3/2N2N2/PPP2PPP/R1BQ1RK1 w - - 4 10",
    "r2q1rk1/pp2ppbp/2np1np1/8/3NP3/1BN1BP2/PPPQ2PP/R3K2R b KQ - 0 10",
    "3r1rk1/p3qppp/2bb1n2/1p6/3P4/1B3N2/PP2QPPP/R1B2RK1 w - - 2 18",
    "r1b2rk1/2q1bppp/p2p1n2/np2p3/3PP3/2P2N1P/PPB2PP1/RNBQR1K1 w - - 0 13",
    "2r2rk1/1bqnbpp1/1p1ppn1p/pP6/N1P1P3/P2B1N1P/1B2QPP1/R2R2K1 b - - 0 17",
    "4rrk1/pp1n1pp1/2pb1q1p/3p4/3P1B2/2NB1Q1P/PPP2PP1/3RR1K1 w - - 0 18",
    "r1bq1rk1/pp2bppp/2n1pn2/3p4/3P4/2NBPN2/PP3PPP/R1BQ1RK1 w - - 4 9",
    "2rq1rk1/pb1nbppp/1p2pn2/8/2BP4/2N1PN2/PPQ2PPP/R1B2RK1 w - - 6 12",

    /* --- tactical positions --- */
    "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
    "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
    "1k1r4/pp1b1R2/3q2pp/4p3/2B5/4Q3/PPP2B2/2K5 b - - 0 1",
    "3r1k2/4npp1/1ppr3p/p6P/P2PPPP1/1NR5/5K2/2R5 w - - 0 1",
    "6k1/1b1nqpbp/pp4p1/5P2/1PN5/4Q3/P5PP/1B2B1K1 b - - 0 1",
    "r3r1k1/2p2ppp/p1p1bn2/8/1q2P3/2NPQN2/PPP3PP/R4RK1 b - - 2 15",
    "q3k1nr/1pp1nQpp/3p4/1P2p3/4P3/B1PP1b2/B5PP/5K1R w k - 0 17",
    "3r3k/2r4p/1p1b3q/p4P2/P2Pp3/1B2P3/3BQ1RP/6K1 w - - 0 1",

    /* --- endgames: a search that is only tested on middlegames will be
           badly tuned here, and endgames decide close games --- */
    "8/8/8/8/8/6k1/6p1/6K1 w - - 0 1",
    "8/2k5/8/8/8/8/5PPP/6K1 w - - 0 1",
    "8/5k2/8/8/8/8/1P1P1P2/2K5 w - - 0 1",
    "4k3/8/8/8/8/8/4P3/4K3 w - - 0 1",
    "8/8/1p1r1k2/p1pPN1p1/P3KnP1/1P6/8/3R4 b - - 0 1",
    "8/2p5/8/2kPKp1p/2p4P/2P5/3P4/8 w - - 0 1",
    "6k1/5ppp/8/8/8/8/5PPP/6K1 w - - 0 1",
    "8/1r3k2/8/2R5/8/5K2/8/8 w - - 0 1",
    "8/8/4k3/8/2n5/8/3K4/8 b - - 0 1",
    "8/8/2Q5/8/5k2/8/8/6K1 w - - 0 1",
    "5k2/8/8/8/8/8/2B1N3/4K3 w - - 0 1",
    "8/8/8/4bk2/8/8/2K5/4B3 w - - 0 1",
    "8/8/3P3k/8/1p6/8/1P6/1K3n2 b - - 0 1",
    "4k3/8/8/1p6/1P6/8/8/4K3 w - - 0 1",
    "8/p7/8/1P6/K1k3p1/6P1/8/8 w - - 0 1",

    /* --- unusual structures that stress evaluation --- */
    "3k4/3p4/8/K1P4r/8/8/8/8 b - - 0 1",
    "8/8/4k3/3nn3/8/8/4K3/8 w - - 0 1",
    "rnbqkbnr/1ppppppp/p7/8/8/P7/1PPPPPPP/RNBQKBNR w KQkq - 0 2",
    "8/3k4/8/8/8/8/3K1R2/8 w - - 0 1",
};

#define BENCH_POSITION_COUNT ((int)(sizeof(BenchPositions) / sizeof(BenchPositions[0])))

void bench_run(int depth) {
    if (depth <= 0)
        depth = BENCH_DEFAULT_DEPTH;

    /* Fixed hash size so the node count cannot depend on whatever the Hash
     * option happened to be set to. */
    tt_resize(16);

    uint64_t totalNodes = 0;
    const int64_t start = time_ms();

    for (int i = 0; i < BENCH_POSITION_COUNT; ++i) {
        Position pos;
        memset(&pos, 0, sizeof(pos));

        if (!board_set_fen(&pos, BenchPositions[i])) {
            printf("bench: invalid FEN at index %d: %s\n", i, BenchPositions[i]);
            continue;
        }

        /* Clear between positions: leftover entries would make each result
         * depend on the positions searched before it, and therefore on the
         * order of the list. */
        tt_clear();
        search_clear();

        SearchLimits limits;
        search_limits_clear(&limits);
        limits.depth = depth;

        search_start(&pos, &limits);
        search_wait();

        totalNodes += search_nodes();
    }

    /* Clamp so a sub-millisecond run cannot divide by zero. */
    int64_t elapsed = time_ms() - start;
    if (elapsed <= 0)
        elapsed = 1;

    const uint64_t nps = (uint64_t)((double)totalNodes * 1000.0 / (double)elapsed);

    printf("\n===========================\n");
    printf("Positions  : %d\n", BENCH_POSITION_COUNT);
    printf("Depth      : %d\n", depth);
    printf("Time       : %lldms\n", (long long)elapsed);

    /* THIS LINE IS THE CONTRACT. OpenBench parses exactly this shape; keep the
     * two numbers, the words `nodes` and `nps`, and the ordering. */
    printf("%llu nodes %llu nps\n", (unsigned long long)totalNodes, (unsigned long long)nps);
}
