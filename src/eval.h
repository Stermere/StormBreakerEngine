/*
 * eval.h - static position evaluation.
 *
 * Returns a score in centipawn-like units from the point of view of the SIDE
 * TO MOVE. Keeping every score side-to-move-relative (rather than always
 * white-relative) is what lets the search use plain negamax with no sign
 * juggling, so do not change this convention casually.
 */
#ifndef EVAL_H
#define EVAL_H

#include "board.h"
#include "types.h"

void eval_init(void);

/*
 * Centipawn exchange values, indexed by PieceType.
 *
 * Used by the search to order captures and to run static exchange evaluation.
 * These are fixed textbook numbers and are deliberately NOT the tuned
 * Material[] weights the evaluation itself uses - see the note at the top of
 * eval.c. The king is zero: it is never captured, and giving it a value would
 * corrupt every exchange calculation it appears in.
 */
extern const Value PieceValues[PIECE_TYPE_NB];

/*
 * The progression this evaluation has followed (see docs/TESTING.md; every
 * step after the first is a behavioural change needing its own SPRT):
 *
 *   1. material only                        - DONE: gets you a playing engine
 *   2. piece-square tables, tapered between midgame and endgame  - DONE
 *   3. pawn structure, mobility, king safety, threats            - DONE
 *   4. weights fitted to real game data (logistic regression on
 *      win/draw/loss labels)                                     - DONE
 *   5. replace the lot with an NNUE network once the search is strong
 *
 * Steps 1-4 are worth several hundred Elo and teach you what the search
 * actually needs. Do not jump straight to step 5.
 *
 * Every weight lives in evalparams.c and is enumerated by evalparams.h, which
 * is what lets `make tuner` refit all of them. docs/TUNING.md is the procedure.
 */
Value eval_evaluate(const Position *pos);

/* Human-readable term-by-term breakdown - the UCI `eval` command. Invaluable
 * when a game is lost to an evaluation blind spot and you need to see why. */
void eval_trace(const Position *pos);

#endif /* EVAL_H */
