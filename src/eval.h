/*
 * eval.h - static position evaluation.
 *
 * Scores are from the point of view of the SIDE TO MOVE, which is what lets the
 * search run plain negamax with no sign juggling.
 */
#ifndef EVAL_H
#define EVAL_H

#include "board.h"
#include "types.h"

void eval_init(void);

/* Textbook exchange values for move ordering and SEE, deliberately not the tuned
 * Material[] weights. The king is zero: valuing it would corrupt every exchange
 * it appears in. */
extern const Value PieceValues[PIECE_TYPE_NB];

Value eval_evaluate(const Position *pos);

/* The classical model by name. eval_evaluate() is the network in a default build,
 * so the tuner and `eval` must ask for this or they measure whatever got linked. */
Value eval_classical(const Position *pos);

/* Term-by-term breakdown - the UCI `eval` command. */
void eval_trace(const Position *pos);

/*
 * The network's accumulator stack: push after a move is played, pop before it is
 * retracted. Correctness does not depend on these being called - each level
 * rebuilds from the board when its stored key does not match - but speed does.
 */
#ifdef EVAL_NNUE

void eval_state_clear(void);
void eval_state_push(const Position *pos, Move m);
void eval_state_push_null(const Position *pos);
void eval_state_pop(void);

#else

static inline void eval_state_clear(void) {}
static inline void eval_state_push(const Position *pos, Move m) {
    (void)pos;
    (void)m;
}
static inline void eval_state_push_null(const Position *pos) { (void)pos; }
static inline void eval_state_pop(void) {}

#endif

#endif
