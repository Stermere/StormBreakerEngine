/* movepicktest.c - perft alone never walks the search's move picker. */
#include "movepicktest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../movegen.h"
#include "../perft.h"

static int Checks, Failures;
static uint64_t Rng;

static unsigned next_random(void) {
    Rng ^= Rng << 13;
    Rng ^= Rng >> 7;
    Rng ^= Rng << 17;
    return (unsigned)Rng;
}

static void report(const char *what, bool ok) {
    ++Checks;
    if (!ok)
        ++Failures;
    printf("%-4s %s\n", ok ? "ok" : "FAIL", what);
}

static int occurrences(const ScoredMove *list, int count, Move m) {
    int n = 0;
    for (int i = 0; i < count; ++i)
        n += list[i].m == m;
    return n;
}

static bool check_position(const Position *pos, bool exhaustive) {
    ScoredMove all[MAX_MOVES], tactical[MAX_MOVES], quiet[MAX_MOVES];
    const int n  = movegen_generate(pos, GEN_ALL, all);
    const int nt = movegen_generate(pos, GEN_TACTICALS, tactical);
    const int nq = movegen_generate(pos, GEN_NON_TACTICALS, quiet);
    if (n != nt + nq)
        return false;
    for (int i = 0; i < n; ++i) {
        const Move m = all[i].m;
        if (occurrences(all, n, m) != 1 ||
            occurrences(tactical, nt, m) + occurrences(quiet, nq, m) != 1 ||
            !movegen_is_pseudo_legal(pos, m))
            return false;
    }
    for (int i = 0; i < nq; ++i)
        if (type_of_move(quiet[i].m) == MT_PROMOTION)
            return false;

    if (exhaustive) {
        /* Full encoded membership, not just legal moves: the direct-move
         * validator must be exactly the gate the generator used to provide. */
        bool present[65536] = {false};
        for (int i = 0; i < n; ++i)
            present[all[i].m] = true;
        for (unsigned m = 0; m <= UINT16_MAX; ++m)
            if (movegen_is_pseudo_legal(pos, (Move)m) != present[m]) {
                printf("     validator disagrees on encoding 0x%04x\n", m);
                return false;
            }
    }

    PickerTestMoves hints = {0};
    if (n) {
        hints.tt      = all[next_random() % (unsigned)n].m;
        hints.killer0 = all[next_random() % (unsigned)n].m;
        hints.killer1 = hints.killer0;
        hints.counter = all[next_random() % (unsigned)n].m;
        if (next_random() & 1)
            hints.excluded = all[next_random() % (unsigned)n].m;
    }
    PickerTestResult picked;
    if (!search_test_picker(pos, hints, MAX_MOVES + 1, &picked))
        return false;
    for (int i = 0; i < picked.count; ++i) {
        const Move m = picked.moves[i].m;
        if (m == hints.excluded || occurrences(picked.moves, picked.count, m) != 1 ||
            occurrences(all, n, m) != 1 ||
            (i > 0 && picked.moves[i - 1].score < picked.moves[i].score))
            return false;
    }
    for (int i = 0; i < n; ++i)
        if (all[i].m != hints.excluded && movegen_is_legal(pos, all[i].m) &&
            occurrences(picked.moves, picked.count, all[i].m) != 1)
            return false;
    return true;
}

static void test_laziness(void) {
    Position pos;
    memset(&pos, 0, sizeof(pos));
    board_set_startpos(&pos);
    const Move tt         = make_move(SQ_E2, SQ_E4);
    const Move k0         = make_move(SQ_D2, SQ_D4);
    const Move k1         = make_move(SQ_G1, SQ_F3);
    const Move counter    = make_move(SQ_B1, SQ_C3);
    PickerTestMoves hints = {tt, k0, k1, counter, MOVE_NONE};
    PickerTestResult out;
    bool ok = search_test_picker(&pos, hints, 1, &out) && out.count == 1 && out.moves[0].m == tt;
#ifndef MOVE_PICKER_EAGER
    ok &= out.tacticalCount == -1 && out.quietCount == -1;
#endif
    report("TT prefix does not generate either main-search batch", ok);
    ok = search_test_picker(&pos, hints, 4, &out) && out.count == 4 && out.moves[1].m == k0 &&
         out.moves[2].m == k1 && out.moves[3].m == counter;
#ifndef MOVE_PICKER_EAGER
    ok &= out.tacticalCount == 0 && out.quietCount == -1;
#endif
    report("refutations are delivered before quiet generation", ok);

    hints = (PickerTestMoves){tt, tt, tt, tt, MOVE_NONE};
    report("TT/killer/counter duplicates are returned exactly once",
           search_test_picker(&pos, hints, MAX_MOVES + 1, &out) && out.count == 20 &&
               occurrences(out.moves, out.count, tt) == 1);
    hints.tt      = (Move)(tt | (1 << 12));
    hints.killer0 = MOVE_NULL;
    hints.killer1 = make_move(SQ_A3, SQ_A4);
    hints.counter = hints.tt;
    report("invalid direct moves cannot enter the generated move set",
           search_test_picker(&pos, hints, MAX_MOVES + 1, &out) && out.count == 20 &&
               occurrences(out.moves, out.count, hints.tt) == 0);

    ok    = board_set_fen(&pos, "1r2k3/P7/8/8/8/8/8/4K3 w - - 0 1");
    hints = (PickerTestMoves){0};
    ok &= search_test_picker(&pos, hints, 8, &out) && out.count == 8;
    for (int i = 0; i < out.count; ++i)
        ok &= type_of_move(out.moves[i].m) == MT_PROMOTION;
#ifndef MOVE_PICKER_EAGER
    ok &= out.tacticalCount == 8 && out.quietCount == -1;
#endif
    report("all eight capturing/pushing promotions precede quiet generation", ok);

    /* Qxd5 loses to ...exd5; unlike a good tactical it must survive in the
     * retained tail until AFTER the king's quiet moves. */
    ok             = board_set_fen(&pos, "4k3/8/4p3/3p4/8/8/8/3QK3 w - - 0 1");
    const Move bad = make_move(SQ_D1, SQ_D5);
    hints.killer0  = bad;
    ok &= search_test_picker(&pos, hints, MAX_MOVES + 1, &out) && out.count > 1 &&
          out.moves[out.count - 1].m == bad && occurrences(out.moves, out.count, bad) == 1;
    for (int i = 1; i < out.count; ++i)
        ok &= out.moves[i - 1].score >= out.moves[i].score;
    report("a tactical killer is not suppressed and losing captures stay last", ok);
}

static void test_suites(void) {
    const char *paths[] = {"tests/perft/standard.epd", "tests/perft/tricky.epd",
                           "tests/perft/chess960.epd", "tests/perft/chess960-startpos.epd"};
    for (size_t p = 0; p < sizeof(paths) / sizeof(paths[0]); ++p) {
        FILE *f = fopen(paths[p], "r");
        if (!f) {
            report(paths[p], false);
            continue;
        }
        char line[1024];
        int positions = 0;
        bool ok       = true;
        while (fgets(line, sizeof(line), f)) {
            if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
                continue;
            char *semi = strchr(line, ';');
            if (!semi)
                continue;
            *semi = '\0';
            Position pos;
            memset(&pos, 0, sizeof(pos));
            ++positions;
            if (!board_set_fen(&pos, line) || !check_position(&pos, p != 3)) {
                printf("     position: %s\n", line);
                ok = false;
                break;
            }
            /* Deeper than a set check: recurse through the actual picker, with
             * no search pruning. Standard perft does not call it at all. */
            const int depth         = p == 3 ? 2 : 3;
            const uint64_t expected = perft(&pos, depth);
            char before[FEN_MAX_LEN], after[FEN_MAX_LEN];
            board_to_fen(&pos, before);
            const uint64_t got = search_test_picker_perft(&pos, depth);
            board_to_fen(&pos, after);
            if (got != expected || strcmp(before, after) || !board_is_consistent(&pos)) {
                printf("     picker perft: got %llu expected %llu, %s\n", (unsigned long long)got,
                       (unsigned long long)expected, line);
                ok = false;
                break;
            }
        }
        fclose(f);
        report(paths[p], ok && positions > 0);
    }
}

static void test_special_positions(void) {
    const char *fens[] = {
        "4r1k1/8/8/8/8/8/4R3/4K3 w - - 0 1", /* pinned rook */
        "4r1k1/8/8/8/1b6/8/8/4K3 w - - 0 1", /* double check */
        "4k3/8/8/r4pPK/8/8/8/8 w - f6 0 1",  /* illegal EP x-ray */
        "4k3/8/8/3pP3/4K3/8/8/8 w - d6 0 1", /* EP takes checker */
        "7k/6Q1/5K2/8/8/8/8/8 b - - 0 1",    /* checkmate */
        "7k/5K2/6Q1/8/8/8/8/8 b - - 0 1",    /* stalemate */
        "7k/8/5KQ1/8/8/8/8/8 b - - 0 1",     /* only h8-g8 */
    };
    bool ok = true;
    Position pos;
    memset(&pos, 0, sizeof(pos));
    for (size_t i = 0; i < sizeof(fens) / sizeof(fens[0]); ++i) {
        if (!board_set_fen(&pos, fens[i]) || !check_position(&pos, true)) {
            printf("     special position: %s\n", fens[i]);
            ok = false;
        }
    }
    report("pins, double check, EP and terminal positions preserve the legal set", ok);
    PickerTestResult out;
    const Move only             = make_move(SQ_H8, SQ_G8);
    const PickerTestMoves hints = {only, only, only, only, only};
    ok                          = search_test_picker(&pos, hints, MAX_MOVES + 1, &out);
    for (int i = 0; i < out.count; ++i)
        ok &= !movegen_is_legal(&pos, out.moves[i].m);
    report("excluding the only legal move leaves no legal picker candidate", ok);
}

static void test_walks(void) {
    bool ok = true;
    for (int game = 0; game < 24 && ok; ++game) {
        Position pos;
        memset(&pos, 0, sizeof(pos));
        if (game & 1)
            board_set_chess960_start(&pos, (int)(next_random() % 960));
        else
            board_set_startpos(&pos);
        for (int ply = 0; ply < 48 && ok; ++ply) {
            ok = check_position(&pos, false);
            ScoredMove list[MAX_MOVES];
            const int n = movegen_generate(&pos, GEN_ALL, list);
            Move legal[MAX_MOVES];
            int count = 0;
            for (int i = 0; i < n; ++i)
                if (movegen_is_legal(&pos, list[i].m))
                    legal[count++] = list[i].m;
            if (!count)
                break;
            if (!ok) {
                char fen[FEN_MAX_LEN];
                board_to_fen(&pos, fen);
                printf("     walk game %d ply %d: %s\n", game, ply, fen);
                break;
            }
            board_do_move(&pos, legal[next_random() % (unsigned)count]);
        }
    }
    report("deterministic standard/Chess960 walks preserve every legal move", ok);
}

int movepick_selftest(void) {
    Checks = Failures = 0;
    Rng               = UINT64_C(0x9e3779b97f4a7c15);
    test_laziness();
    report("same-ply lifetime, snapshot timing and score-band contracts",
           search_test_picker_contracts() == 0);
    test_suites();
    test_special_positions();
    test_walks();
    printf("movepick: %d checks, %d failures\n", Checks, Failures);
    return Failures || Checks == 0;
}