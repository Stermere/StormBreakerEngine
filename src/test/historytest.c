/* historytest.c - the pawn-context table's indexing and weighting contract. */
#include "historytest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../board.h"
#include "../history.h"
#include "../movegen.h"

static int Checks;
static int Failures;

static void report(const char *what, bool ok) {
    ++Checks;
    if (!ok)
        ++Failures;
    printf("%-4s %s\n", ok ? "ok" : "FAIL", what);
}

static void test_pawn_history(void) {
    PawnHistory *const a = calloc(1, sizeof(*a));
    PawnHistory *const b = calloc(1, sizeof(*b));
    report("pawn history allocates independent thread-owned tables", a != NULL && b != NULL);
    if (!a || !b) {
        free(a);
        free(b);
        return;
    }

    report("pawn history costs exactly one MiB per thread", sizeof(*a) == 1024 * 1024);
    const Key key        = UINT64_C(0xf123456789abcdef);
    int16_t *const entry = history_pawn_entry(a, key, W_KNIGHT, SQ_F3);
    report("a fresh context contributes zero", history_pawn_score(entry, 128) == 0);
    *entry = 7714;
    report("same pawn structure and move share evidence",
           history_pawn_entry(a, key, W_KNIGHT, SQ_F3) == entry);
    report("another pawn bucket does not share evidence",
           *history_pawn_entry(a, key ^ 1, W_KNIGHT, SQ_F3) == 0);
    report("piece color is part of the context", *history_pawn_entry(a, key, B_KNIGHT, SQ_F3) == 0);
    report("piece type is part of the context", *history_pawn_entry(a, key, W_BISHOP, SQ_F3) == 0);
    report("destination is part of the context", *history_pawn_entry(a, key, W_KNIGHT, SQ_H3) == 0);
    report("another thread has independent evidence",
           *history_pawn_entry(b, key, W_KNIGHT, SQ_F3) == 0);
    report("hash collisions share only the intended slot",
           history_pawn_entry(a, key ^ PAWN_HISTORY_SIZE, W_KNIGHT, SQ_F3) == entry);
    report("zero weight contributes nothing", history_pawn_score(entry, 0) == 0);
    report("default weight reads the full history", history_pawn_score(entry, 128) == 7714);
    report("maximum weight doubles the history", history_pawn_score(entry, 256) == 15428);
    *entry = -7714;
    report("failed moves retain a signed penalty", history_pawn_score(entry, 128) == -7714);
    *entry = -16384;
    report("maximum negative contribution fits int", history_pawn_score(entry, 256) == -32768);
    *entry = 16384;
    report("maximum positive contribution fits int", history_pawn_score(entry, 256) == 32768);
    report("all-one pawn hash stays inside the table",
           history_pawn_index(UINT64_MAX) == PAWN_HISTORY_SIZE - 1);

    /* A quiet pawn move is rewarded in the structure it LEFT, not the one it made.
     * Neither castling's rook destination nor promotion may be treated as an array
     * index for a nonexistent piece after undo. Exercise the actual board routines. */
    static const struct {
        const char *name;
        const char *fen;
        bool changesPawns;
    } moves[] = {
        {"knight move preserves pawn context", FEN_STARTPOS, false},
        {"pawn move changes and undo restores context", FEN_STARTPOS, true},
        {"capture changes and undo restores context",
         "rnbqkbnr/ppp1pppp/8/3p4/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2", true},
        {"promotion restores the original pawn context", "4k3/P7/8/8/8/8/8/4K3 w - - 0 1", true},
        {"Chess960 castling preserves pawn context", "4k3/8/8/8/8/8/8/5KR1 w G - 0 1", false},
    };
    const Move actual[] = {make_move(SQ_G1, SQ_F3), make_move(SQ_E2, SQ_E4),
                           make_move(SQ_E4, SQ_D5), make_promotion(SQ_A7, SQ_A8, QUEEN),
                           make_move_typed(SQ_F1, SQ_G1, MT_CASTLING)};
    for (size_t i = 0; i < sizeof(moves) / sizeof(moves[0]); ++i) {
        Position pos;
        memset(&pos, 0, sizeof(pos));
        const Move m = actual[i];
        if (!board_set_fen(&pos, moves[i].fen) || !movegen_is_pseudo_legal(&pos, m) ||
            !movegen_is_legal(&pos, m)) {
            report(moves[i].name, false);
            continue;
        }
        const Key before        = pos.pawnKey;
        const Piece pc          = piece_on(&pos, from_sq(m));
        int16_t *const original = history_pawn_entry(a, before, pc, to_sq(m));
        *original               = 1234;
        board_do_move(&pos, m);
        const bool changedAsExpected = (pos.pawnKey != before) == moves[i].changesPawns;
        board_undo_move(&pos, m);
        report(moves[i].name, changedAsExpected && pos.pawnKey == before &&
                                  piece_on(&pos, from_sq(m)) == pc &&
                                  history_pawn_entry(a, pos.pawnKey, pc, to_sq(m)) == original &&
                                  *original == 1234);
    }

    memset(a, 0, sizeof(*a));
    bool cleared = true;
    for (unsigned i = 0; i < PAWN_HISTORY_SIZE; ++i)
        for (int pc = 0; pc < PIECE_NB; ++pc)
            for (int to = 0; to < SQUARE_NB; ++to)
                cleared &= a->entries[i][pc][to] == 0;
    report("clearing drops every pawn-history entry", cleared);
    free(a);
    free(b);
}

static void test_rescue_credit(void) {
    static const struct {
        const char *name;
        int bonus;
        Value eval, beta, cutoff;
        int error;
        bool excluded;
        int weight, floor, expected;
    } cases[] = {
        {"rescue off adds no credit", 800, -64, 0, 10, 32, false, 0, 32, 0},
        {"half-scale rescue adds half the allowed credit", 800, -32, 0, 10, 32, false, 25, 32, 100},
        {"full-scale rescue caps at the allowed credit", 800, -1000, 0, 10, 32, false, 25, 32, 200},
        {"an easy cutoff keeps its ordinary reward", 800, 64, 0, 10, 32, false, 25, 32, 0},
        {"equal eval and beta gets no rescue credit", 800, 0, 0, 10, 32, false, 25, 32, 0},
        {"failed moves get no rescue credit", 800, -64, 0, -1, 32, false, 25, 32, 0},
        {"larger predicted error dampens rescue credit", 800, -32, 0, 10, 96, false, 25, 32, 50},
        {"larger rescue floor dampens credit", 800, -32, 0, 10, 32, false, 25, 96, 50},
        {"zero predicted error is safe", 800, -16, 0, 10, 0, false, 25, 32, 100},
        {"fail-soft overshoot adds no extra credit", 800, -32, 0, 1000, 32, false, 25, 32, 100},
        {"headless rescue is neutral", 800, -64, 0, 10, -1, false, 25, 32, 0},
        {"in-check rescue is neutral", 800, VALUE_NONE, 0, 10, 32, false, 25, 32, 0},
        {"excluded-search rescue is neutral", 800, -64, 0, 10, 32, true, 25, 32, 0},
        {"decisive eval cannot teach rescue credit", 800, -VALUE_TB_WIN, 0, 10, 32, false, 25, 32,
         0},
        {"decisive window cannot teach rescue credit", 800, 0, VALUE_TB_WIN, VALUE_TB_WIN, 32,
         false, 25, 32, 0},
        {"decisive cutoff cannot teach rescue credit", 800, -64, 0, VALUE_MATE, 32, false, 25, 32,
         0},
        {"small rescue credit truncates toward zero", 9, -32, 0, 10, 32, false, 25, 32, 1},
        {"zero reward gets no extra credit", 0, -64, 0, 10, 32, false, 25, 32, 0},
        {"wide rescue products at tuning bounds", 24576, -20000, 30000, 30000, 20000, false, 100,
         256, 24576},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const int got = history_pawn_rescue_credit(
            cases[i].bonus, cases[i].eval, cases[i].beta, cases[i].cutoff, cases[i].error,
            cases[i].excluded, cases[i].weight, cases[i].floor);
        report(cases[i].name, got == cases[i].expected);
        if (got != cases[i].expected)
            printf("     got %d, expected %d\n", got, cases[i].expected);
    }

    bool bounded = true, monotone = true;
    const int errors[] = {0, 32, 20000};
    for (int weight = 0; weight <= 100; ++weight)
        for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
            int previous = 0;
            for (int gap = -64; gap <= 256; ++gap) {
                const int credit =
                    history_pawn_rescue_credit(997, -gap, 0, 1, errors[i], false, weight, 32);
                bounded &= credit >= 0 && credit <= 997 * weight / 100;
                monotone &= credit >= previous;
                previous = credit;
            }
        }
    report("rescue never reduces or exceeds the bounded extra reward", bounded);
    report("harder cutoffs never receive less rescue credit", monotone);
}

static void test_evidence_malus(void) {
    static const struct {
        const char *name;
        Depth depth, childDepth, reduction;
        bool fullDepthSearch;
        Depth expected;
    } depths[] = {
        {"unreduced search retains nominal evidence", 8, 7, 0, false, 8},
        {"reduced-only failure records shallower evidence", 8, 7, 3, false, 5},
        {"normal-depth retry restores ordinary evidence", 8, 7, 3, true, 8},
        {"an extension contributes to requested evidence", 8, 8, 3, false, 6},
        {"extensions cannot exceed ordinary evidence", 8, 10, 1, false, 8},
        {"nonpositive child depth is not negative evidence", 1, 0, 1, false, 0},
        {"negative extension cannot make negative evidence", 3, -2, 1, false, 0},
        {"no reduction leaves negative extensions unchanged", 3, 0, 0, false, 3},
    };
    for (size_t i = 0; i < sizeof(depths) / sizeof(depths[0]); ++i)
        report(depths[i].name, history_pawn_evidence_depth(
                                   depths[i].depth, depths[i].childDepth, depths[i].reduction,
                                   depths[i].fullDepthSearch) == depths[i].expected);

    static const struct {
        const char *name;
        int ordinary, shallow, weight, expected;
    } maluses[] = {
        {"evidence weight zero preserves the old penalty", 576, 225, 0, 576},
        {"half blend retains conservative integer rounding", 576, 225, 50, 401},
        {"full blend uses the shallower penalty", 576, 225, 100, 225},
        {"normal-depth evidence preserves the penalty", 576, 576, 50, 576},
        {"deeper evidence cannot strengthen the penalty", 576, 729, 100, 576},
        {"zero evidence retains half the ordinary penalty", 576, 0, 50, 288},
        {"zero penalty remains zero", 0, 0, 100, 0},
        {"wide evidence products remain valid", 32768, 0, 100, 0},
    };
    for (size_t i = 0; i < sizeof(maluses) / sizeof(maluses[0]); ++i)
        report(maluses[i].name,
               history_pawn_evidence_malus(maluses[i].ordinary, maluses[i].shallow,
                                           maluses[i].weight) == maluses[i].expected);

    /* A reduced fail-high is not the final observation. A retry that fails still
     * received the full request; a later move's cutoff must not discount its malus. */
    bool fullDepthSearch = false;
    const Depth reduced  = history_pawn_evidence_depth(8, 7, 3, fullDepthSearch);
    fullDepthSearch      = true;
    const Depth retried  = history_pawn_evidence_depth(8, 7, 3, fullDepthSearch);
    report("retry replaces the reduced-only observation",
           history_pawn_evidence_malus(8 * 8 * 9, reduced * reduced * 9, 50) == 401 &&
               history_pawn_evidence_malus(8 * 8 * 9, retried * retried * 9, 50) == 576);

    bool bounded = true, monotone = true;
    for (int ordinary = 0; ordinary <= 1024; ordinary += 16)
        for (int shallow = 0; shallow <= ordinary; shallow += 16) {
            int previous = ordinary;
            for (int weight = 0; weight <= 100; ++weight) {
                const int penalty = history_pawn_evidence_malus(ordinary, shallow, weight);
                bounded &= penalty >= shallow && penalty <= ordinary;
                monotone &= penalty <= previous;
                previous = penalty;
            }
        }
    report("evidence blend never exceeds either penalty bound", bounded);
    report("higher evidence weight never increases the penalty", monotone);
}

int history_selftest(void) {
    Checks   = 0;
    Failures = 0;

    test_pawn_history();
    test_rescue_credit();
    test_evidence_malus();

    printf("history: %d checks, %d failures\n", Checks, Failures);
    fflush(stdout);
    return Checks == 0 ? 1 : Failures;
}