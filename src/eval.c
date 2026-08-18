/*
 * eval.c - static position evaluation.
 *
 * Material only, which is step 1 of the roadmap in eval.h. That is enough for
 * a legal, playing engine and nothing more: it has no idea about king safety,
 * pawn structure or where a knight belongs, and it will happily shuffle in an
 * equal position. That is the point of freezing it as a baseline - every term
 * added from here is measured against it rather than against a guess.
 */
#include "eval.h"

#include <stdio.h>

/*
 * Deliberately the plain textbook values rather than anything tuned. A tuned
 * set is a behavioural change and belongs behind its own SPRT; starting from
 * the obvious numbers keeps that measurement honest.
 */
const Value PieceValues[PIECE_TYPE_NB] = {
    [NO_PIECE_TYPE] = 0, [PAWN] = 100,  [KNIGHT] = 320, [BISHOP] = 330,
    [ROOK] = 500,        [QUEEN] = 900, [KING] = 0,
};

void eval_init(void) { /* TODO(engine): build piece-square tables and any derived constants. */ }

Value eval_evaluate(const Position *pos) {
    Value score = VALUE_ZERO;

    for (PieceType pt = PAWN; pt <= QUEEN; ++pt)
        score += PieceValues[pt] * (piece_count(pos, WHITE, pt) - piece_count(pos, BLACK, pt));

    /* Scores are side-to-move-relative so the search can stay plain negamax. */
    return pos->sideToMove == WHITE ? score : -score;
}

void eval_trace(const Position *pos) {
    static const char *const Names[PIECE_TYPE_NB] = {
        [PAWN] = "Pawn", [KNIGHT] = "Knight", [BISHOP] = "Bishop",
        [ROOK] = "Rook", [QUEEN] = "Queen",
    };
    Value total = VALUE_ZERO;

    printf("           white  black  total\n");
    for (PieceType pt = PAWN; pt <= QUEEN; ++pt) {
        const int w     = piece_count(pos, WHITE, pt);
        const int b     = piece_count(pos, BLACK, pt);
        const Value net = PieceValues[pt] * (w - b);
        total += net;
        printf("%-9s  %4d   %4d  %+6.2f\n", Names[pt], w, b, (double)net / 100.0);
    }

    printf("\nMaterial (white side): %+.2f\n", (double)total / 100.0);
    printf("Evaluation (side to move): %+.2f\n",
           (double)(pos->sideToMove == WHITE ? total : -total) / 100.0);
    printf("\nNOTE: evaluation is material only - see the roadmap in eval.h.\n");
}
