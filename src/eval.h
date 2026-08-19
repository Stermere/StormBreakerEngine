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
 * Centipawn piece values, indexed by PieceType.
 *
 * Shared with the search, which orders captures by them, so ordering and
 * evaluation can never disagree about what a piece is worth. The king is zero:
 * it is never captured, and giving it a value would corrupt every exchange
 * calculation it appears in.
 */
extern const Value PieceValues[PIECE_TYPE_NB];

/*
 * Currently step 2 of the progression below. Every step after this one is a
 * behavioural change and needs its own SPRT - see docs/TESTING.md.
 *
 * A pragmatic progression, SPRT-testing each step (see docs/TESTING.md):
 *   1. material only                        - DONE: gets you a playing engine
 *   2. piece-square tables, tapered between midgame and endgame  - DONE
 *   3. pawn structure, mobility, king safety
 *   4. tune the weights on real game data (Texel tuning / gradient descent)
 *   5. replace the lot with an NNUE network once the search is strong
 *
 * Steps 1-4 are worth several hundred Elo and teach you what the search
 * actually needs. Do not jump straight to step 5.
 */
Value eval_evaluate(const Position *pos);

/* Human-readable term-by-term breakdown - the UCI `eval` command. Invaluable
 * when a game is lost to an evaluation blind spot and you need to see why. */
void eval_trace(const Position *pos);

#endif /* EVAL_H */
