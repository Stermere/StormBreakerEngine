/*
 * perft.c - move generation correctness testing.
 */
#include "perft.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "movegen.h"
#include "timeman.h"

/*
 * In check the evasion generator is both narrower and faster, and it is what
 * the search uses - so perft exercises that path rather than leaving it
 * unverified. Debug builds additionally assert that the two generators agree
 * on the legal moves, which keeps GEN_ALL honest under the same suites.
 */
static int generate_for_perft(const Position *pos, ScoredMove *list) {
    if (!board_checkers(pos))
        return movegen_generate(pos, GEN_ALL, list);

#ifndef NDEBUG
    {
        ScoredMove all[MAX_MOVES];
        const int n  = movegen_generate(pos, GEN_ALL, all);
        int legalAll = 0;
        for (int i = 0; i < n; ++i)
            legalAll += movegen_is_legal(pos, all[i].m);

        const int m       = movegen_generate(pos, GEN_EVASIONS, list);
        int legalEvasions = 0;
        for (int i = 0; i < m; ++i)
            legalEvasions += movegen_is_legal(pos, list[i].m);

        assert(legalAll == legalEvasions);
        return m;
    }
#else
    return movegen_generate(pos, GEN_EVASIONS, list);
#endif
}

uint64_t perft(Position *pos, int depth) {
    ScoredMove moves[MAX_MOVES];

    if (depth <= 0)
        return 1;

    const int count = generate_for_perft(pos, moves);
    uint64_t nodes  = 0;

    for (int i = 0; i < count; ++i) {
        if (!movegen_is_legal(pos, moves[i].m))
            continue;

        /* Bulk counting: at depth 1 the legal moves ARE the leaves, so there
         * is no need to make and unmake each one. Roughly a 5x speedup, and
         * the counts are identical. */
        if (depth == 1) {
            ++nodes;
            continue;
        }

        board_do_move(pos, moves[i].m);
        nodes += perft(pos, depth - 1);
        board_undo_move(pos, moves[i].m);
    }

    return nodes;
}

void perft_divide(Position *pos, int depth) {
    ScoredMove moves[MAX_MOVES];
    char buf[8];

    if (depth <= 0) {
        printf("\nNodes searched: 1\n");
        return;
    }

    const int count = generate_for_perft(pos, moves);
    uint64_t total  = 0;

    for (int i = 0; i < count; ++i) {
        if (!movegen_is_legal(pos, moves[i].m))
            continue;

        board_do_move(pos, moves[i].m);
        const uint64_t nodes = perft(pos, depth - 1);
        board_undo_move(pos, moves[i].m);

        total += nodes;
        printf("%s: %llu\n", move_to_str(moves[i].m, buf), (unsigned long long)nodes);
    }

    /* Blank line then "Nodes searched:" matches Stockfish's `go perft` output,
     * so the two can be diffed directly. */
    printf("\nNodes searched: %llu\n", (unsigned long long)total);
}

bool perft_run_suite(const char *path, int maxDepth) {
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("error: cannot open perft suite '%s'\n", path);
        return false;
    }

    char line[1024];
    int positions       = 0;
    int failures        = 0;
    uint64_t totalNodes = 0;
    const int64_t start = time_ms();

    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t')
            ++p;
        if (*p == '\n' || *p == '\r' || *p == '\0' || *p == '#')
            continue;

        char *semi = strchr(p, ';');
        if (!semi)
            continue;
        *semi = '\0';

        Position pos;
        memset(&pos, 0, sizeof(pos));
        if (!board_set_fen(&pos, p)) {
            printf("FAIL  invalid FEN: %s\n", p);
            ++failures;
            continue;
        }

        ++positions;
        bool positionOk = true;
        char *spec      = semi + 1;

        while (spec) {
            int depth                   = 0;
            unsigned long long expected = 0;
            if (sscanf(spec, "D%d %llu", &depth, &expected) == 2 &&
                (maxDepth <= 0 || depth <= maxDepth)) {
                const uint64_t got = perft(&pos, depth);
                totalNodes += got;

                if (got != (uint64_t)expected) {
                    printf("FAIL  depth %d: expected %llu, got %llu\n      %s\n", depth, expected,
                           (unsigned long long)got, p);
                    positionOk = false;
                    ++failures;
                }
            }

            spec = strchr(spec, ';');
            if (spec)
                ++spec;
        }

        if (positionOk)
            printf("ok    %s\n", p);
    }

    fclose(f);

    const int64_t elapsed = time_ms() - start;
    printf("\n%d positions, %d failures, %llu nodes in %lldms\n", positions, failures,
           (unsigned long long)totalNodes, (long long)elapsed);

    /* A suite that checked nothing reports success, which would be a silent
     * false pass in CI. Say so explicitly; the caller still treats it as a
     * failure via the `totalNodes > 0` term below. */
    if (failures == 0 && totalNodes == 0)
        printf("NOTE: no positions were checked - the suite file parsed but every "
               "depth was filtered out by maxDepth.\n");

    return failures == 0 && totalNodes > 0;
}
