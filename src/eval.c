/*
 * eval.c - static position evaluation.
 *
 * Material plus piece-square tables, tapered between a midgame and an endgame
 * set. That is step 2 of the roadmap in eval.h.
 *
 * The tapering is the part that is easy to underrate. A single set of tables
 * cannot be right for both phases, because the same square means opposite
 * things at different points in the game: a king on g1 behind three pawns is
 * exactly where it belongs on move 20 and a liability on move 60, and a pawn
 * on the seventh is nearly a queen in an endgame and a weakness in a
 * middlegame. Interpolating between two sets by how much material is left
 * gives a score that moves continuously as pieces come off, instead of one
 * that jumps the moment some threshold is crossed.
 */
#include "eval.h"

#include <stdio.h>

#include "bitboard.h"

/*
 * Deliberately the plain textbook values rather than anything tuned. A tuned
 * set is a behavioural change and belongs behind its own SPRT; starting from
 * the obvious numbers keeps that measurement honest.
 */
const Value PieceValues[PIECE_TYPE_NB] = {
    [NO_PIECE_TYPE] = 0, [PAWN] = 100,  [KNIGHT] = 320, [BISHOP] = 330,
    [ROOK] = 500,        [QUEEN] = 900, [KING] = 0,
};

/*
 * Endgame material values. PieceValues above doubles as the midgame set, which
 * keeps it honest as the exchange table the search orders captures with - the
 * two can never disagree about what a piece is worth in a middlegame trade.
 *
 * Pawns are worth more here because they promote, and the pieces that stop
 * them gain a little with them. Every one of these numbers is a guess until it
 * is tuned; the point of step 4 of the roadmap is to replace them with numbers
 * fitted to real games.
 */
static const Value EgPieceValues[PIECE_TYPE_NB] = {
    [NO_PIECE_TYPE] = 0, [PAWN] = 120,  [KNIGHT] = 320, [BISHOP] = 340,
    [ROOK] = 550,        [QUEEN] = 980, [KING] = 0,
};

/*
 * Piece-square tables, written from white's point of view with RANK 8 ON THE
 * FIRST LINE so they read like a chessboard. eval_init() flips them into board
 * order, so what you see here is what white sees looking up the board.
 */
/* clang-format off */
static const Value PstMg[PIECE_TYPE_NB][SQUARE_NB] = {
    [PAWN] = {
          0,   0,   0,   0,   0,   0,   0,   0,
         50,  50,  50,  50,  50,  50,  50,  50,
         10,  10,  20,  30,  30,  20,  10,  10,
          5,   5,  10,  25,  25,  10,   5,   5,
          0,   0,   0,  20,  20,   0,   0,   0,
          5,  -5, -10,   0,   0, -10,  -5,   5,
          5,  10,  10, -20, -20,  10,  10,   5,
          0,   0,   0,   0,   0,   0,   0,   0,
    },
    [KNIGHT] = {
        -50, -40, -30, -30, -30, -30, -40, -50,
        -40, -20,   0,   0,   0,   0, -20, -40,
        -30,   0,  10,  15,  15,  10,   0, -30,
        -30,   5,  15,  20,  20,  15,   5, -30,
        -30,   0,  15,  20,  20,  15,   0, -30,
        -30,   5,  10,  15,  15,  10,   5, -30,
        -40, -20,   0,   5,   5,   0, -20, -40,
        -50, -40, -30, -30, -30, -30, -40, -50,
    },
    [BISHOP] = {
        -20, -10, -10, -10, -10, -10, -10, -20,
        -10,   0,   0,   0,   0,   0,   0, -10,
        -10,   0,   5,  10,  10,   5,   0, -10,
        -10,   5,   5,  10,  10,   5,   5, -10,
        -10,   0,  10,  10,  10,  10,   0, -10,
        -10,  10,  10,  10,  10,  10,  10, -10,
        -10,   5,   0,   0,   0,   0,   5, -10,
        -20, -10, -10, -10, -10, -10, -10, -20,
    },
    [ROOK] = {
          0,   0,   0,   0,   0,   0,   0,   0,
          5,  10,  10,  10,  10,  10,  10,   5,
         -5,   0,   0,   0,   0,   0,   0,  -5,
         -5,   0,   0,   0,   0,   0,   0,  -5,
         -5,   0,   0,   0,   0,   0,   0,  -5,
         -5,   0,   0,   0,   0,   0,   0,  -5,
         -5,   0,   0,   0,   0,   0,   0,  -5,
          0,   0,   0,   5,   5,   0,   0,   0,
    },
    [QUEEN] = {
        -20, -10, -10,  -5,  -5, -10, -10, -20,
        -10,   0,   0,   0,   0,   0,   0, -10,
        -10,   0,   5,   5,   5,   5,   0, -10,
         -5,   0,   5,   5,   5,   5,   0,  -5,
          0,   0,   5,   5,   5,   5,   0,  -5,
        -10,   5,   5,   5,   5,   5,   0, -10,
        -10,   0,   5,   0,   0,   0,   0, -10,
        -20, -10, -10,  -5,  -5, -10, -10, -20,
    },
    /* The midgame king wants a corner behind unmoved pawns, and the table says
     * so bluntly: the centre is worth half a pawn less than g1. */
    [KING] = {
        -30, -40, -40, -50, -50, -40, -40, -30,
        -30, -40, -40, -50, -50, -40, -40, -30,
        -30, -40, -40, -50, -50, -40, -40, -30,
        -30, -40, -40, -50, -50, -40, -40, -30,
        -20, -30, -30, -40, -40, -30, -30, -20,
        -10, -20, -20, -20, -20, -20, -20, -10,
         20,  20,   0,   0,   0,   0,  20,  20,
         20,  30,  10,   0,   0,  10,  30,  20,
    },
};

static const Value PstEg[PIECE_TYPE_NB][SQUARE_NB] = {
    /* Endgame pawns are scored almost purely by how close they are to
     * promoting, which is what makes the engine push them instead of shuffling
     * pieces while its passer sits on the fourth rank. */
    [PAWN] = {
          0,   0,   0,   0,   0,   0,   0,   0,
         80,  80,  80,  80,  80,  80,  80,  80,
         50,  50,  50,  50,  50,  50,  50,  50,
         30,  30,  30,  30,  30,  30,  30,  30,
         15,  15,  15,  15,  15,  15,  15,  15,
          5,   5,   5,   5,   5,   5,   5,   5,
          0,   0,   0,   0,   0,   0,   0,   0,
          0,   0,   0,   0,   0,   0,   0,   0,
    },
    [KNIGHT] = {
        -40, -30, -20, -20, -20, -20, -30, -40,
        -30, -15,   0,   0,   0,   0, -15, -30,
        -20,   0,  10,  15,  15,  10,   0, -20,
        -20,   5,  15,  20,  20,  15,   5, -20,
        -20,   0,  15,  20,  20,  15,   0, -20,
        -20,   5,  10,  15,  15,  10,   5, -20,
        -30, -15,   0,   5,   5,   0, -15, -30,
        -40, -30, -20, -20, -20, -20, -30, -40,
    },
    [BISHOP] = {
        -15, -10, -10, -10, -10, -10, -10, -15,
        -10,   0,   0,   0,   0,   0,   0, -10,
        -10,   0,   5,  10,  10,   5,   0, -10,
        -10,   5,   5,  10,  10,   5,   5, -10,
        -10,   0,  10,  10,  10,  10,   0, -10,
        -10,   5,   5,  10,  10,   5,   5, -10,
        -10,   0,   0,   0,   0,   0,   0, -10,
        -15, -10, -10, -10, -10, -10, -10, -15,
    },
    /* No central-file preference left: in an endgame a rook belongs behind a
     * passed pawn or on the seventh, and neither is a property of the file. */
    [ROOK] = {
          0,   0,   0,   0,   0,   0,   0,   0,
         10,  10,  10,  10,  10,  10,  10,  10,
          0,   0,   0,   0,   0,   0,   0,   0,
          0,   0,   0,   0,   0,   0,   0,   0,
          0,   0,   0,   0,   0,   0,   0,   0,
          0,   0,   0,   0,   0,   0,   0,   0,
          0,   0,   0,   0,   0,   0,   0,   0,
          0,   0,   0,   0,   0,   0,   0,   0,
    },
    [QUEEN] = {
        -10,  -5,  -5,  -5,  -5,  -5,  -5, -10,
         -5,   0,   0,   0,   0,   0,   0,  -5,
         -5,   0,   5,   5,   5,   5,   0,  -5,
         -5,   0,   5,  10,  10,   5,   0,  -5,
         -5,   0,   5,  10,  10,   5,   0,  -5,
         -5,   0,   5,   5,   5,   5,   0,  -5,
         -5,   0,   0,   0,   0,   0,   0,  -5,
        -10,  -5,  -5,  -5,  -5,  -5,  -5, -10,
    },
    /* And the endgame king wants the exact opposite of the midgame one: the
     * centre, where it supports pawns and cuts the enemy king off. Sitting on
     * g1 in a pawn endgame loses games that a centralised king draws. */
    [KING] = {
        -50, -40, -30, -20, -20, -30, -40, -50,
        -30, -20, -10,   0,   0, -10, -20, -30,
        -30, -10,  20,  30,  30,  20, -10, -30,
        -30, -10,  30,  40,  40,  30, -10, -30,
        -30, -10,  30,  40,  40,  30, -10, -30,
        -30, -10,  20,  30,  30,  20, -10, -30,
        -30, -30,   0,   0,   0,   0, -30, -30,
        -50, -30, -30, -30, -30, -30, -30, -50,
    },
};
/* clang-format on */

/*
 * Material plus square value, per piece code and square, ready to sum. Built
 * by eval_init() so the hot loop is two array reads and two adds per piece
 * rather than a piece-type switch and a colour-dependent square flip.
 *
 * Black entries are negated and vertically mirrored, so both colours are
 * accumulated into one running white-relative total with no branch.
 */
static Value PsqMg[PIECE_NB][SQUARE_NB];
static Value PsqEg[PIECE_NB][SQUARE_NB];

/*
 * Game phase: 24 with all the pieces on, 0 once only kings and pawns remain.
 *
 * Pawns deliberately contribute nothing. Phase is meant to measure how much
 * material is left to attack WITH, and a position with every pawn and no piece
 * is an endgame however many pawns are on the board.
 */
#define PHASE_MAX 24

static const int PhaseWeight[PIECE_TYPE_NB] = {
    [KNIGHT] = 1,
    [BISHOP] = 1,
    [ROOK]   = 2,
    [QUEEN]  = 4,
};

static int game_phase(const Position *pos) {
    int phase = 0;

    for (PieceType pt = KNIGHT; pt <= QUEEN; ++pt)
        phase += PhaseWeight[pt] * (piece_count(pos, WHITE, pt) + piece_count(pos, BLACK, pt));

    /* Promotions can put more material on the board than the game started
     * with, and an unclamped phase would extrapolate past the midgame table. */
    return phase > PHASE_MAX ? PHASE_MAX : phase;
}

void eval_init(void) {
    for (PieceType pt = PAWN; pt <= KING; ++pt) {
        for (Square s = SQ_A1; s <= SQ_H8; ++s) {
            /* The literals are laid out rank 8 first, so the table index for a
             * board square is that square mirrored vertically - which is
             * exactly flip_rank(). */
            const Square t = flip_rank(s);

            const Value mg = PieceValues[pt] + PstMg[pt][t];
            const Value eg = EgPieceValues[pt] + PstEg[pt][t];

            PsqMg[make_piece(WHITE, pt)][s] = mg;
            PsqEg[make_piece(WHITE, pt)][s] = eg;

            /* Black gets the same value at the mirrored square, negated: the
             * whole evaluation is accumulated white-relative and flipped once
             * at the end. */
            PsqMg[make_piece(BLACK, pt)][t] = -mg;
            PsqEg[make_piece(BLACK, pt)][t] = -eg;
        }
    }
}

Value eval_evaluate(const Position *pos) {
    int mg = 0;
    int eg = 0;

    Bitboard occupied = occupied_bb(pos);
    while (occupied) {
        const Square s = pop_lsb(&occupied);
        const Piece pc = piece_on(pos, s);

        mg += PsqMg[pc][s];
        eg += PsqEg[pc][s];
    }

    /* Interpolate. With everything on the board this is purely the midgame
     * score; with nothing but kings and pawns, purely the endgame one. */
    const int phase   = game_phase(pos);
    const Value score = (Value)((mg * phase + eg * (PHASE_MAX - phase)) / PHASE_MAX);

    /* Scores are side-to-move-relative so the search can stay plain negamax. */
    return pos->sideToMove == WHITE ? score : -score;
}

void eval_trace(const Position *pos) {
    static const char *const Names[PIECE_TYPE_NB] = {
        [PAWN] = "Pawn", [KNIGHT] = "Knight", [BISHOP] = "Bishop",
        [ROOK] = "Rook", [QUEEN] = "Queen",   [KING] = "King",
    };

    const int phase = game_phase(pos);
    int totalMg     = 0;
    int totalEg     = 0;

    printf("           count(w/b)   midgame   endgame\n");
    printf("           ----------   -------   -------\n");

    for (PieceType pt = PAWN; pt <= KING; ++pt) {
        int mg = 0;
        int eg = 0;

        for (Color c = WHITE; c <= BLACK; ++c) {
            Bitboard b = pieces_bb(pos, c, pt);
            while (b) {
                const Square s = pop_lsb(&b);
                mg += PsqMg[make_piece(c, pt)][s];
                eg += PsqEg[make_piece(c, pt)][s];
            }
        }

        totalMg += mg;
        totalEg += eg;

        printf("%-9s     %2d/%-2d     %+7.2f   %+7.2f\n", Names[pt], piece_count(pos, WHITE, pt),
               piece_count(pos, BLACK, pt), (double)mg / 100.0, (double)eg / 100.0);
    }

    printf("           ----------   -------   -------\n");
    printf("%-9s                %+7.2f   %+7.2f\n", "Total", (double)totalMg / 100.0,
           (double)totalEg / 100.0);

    const int tapered = (totalMg * phase + totalEg * (PHASE_MAX - phase)) / PHASE_MAX;

    printf("\nPhase: %d/%d (%d%% midgame)\n", phase, PHASE_MAX, phase * 100 / PHASE_MAX);
    printf("Tapered (white side):      %+.2f\n", (double)tapered / 100.0);
    printf("Evaluation (side to move): %+.2f\n",
           (double)(pos->sideToMove == WHITE ? tapered : -tapered) / 100.0);
    printf("\nNOTE: material and piece-square tables only - see the roadmap in eval.h.\n");
}
