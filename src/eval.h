/*
 * eval.h - static position evaluation.
 *
 * Scores are from the point of view of the SIDE TO MOVE, which is what lets the
 * search run plain negamax with no sign juggling.
 */
#ifndef EVAL_H
#define EVAL_H

#include <stdbool.h>
#include <stddef.h>

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
 *
 * One stack per THREAD, held in thread-local storage. eval_state_alloc() claims the
 * calling thread's, and every searching thread must call it: without one the
 * evaluation still answers correctly, from a full accumulation at every node, which
 * is several times slower. It is allocated lazily by the first evaluation on a
 * thread that skipped it, so a tool that only wants a score need not know about any
 * of this.
 */
#ifdef EVAL_NNUE

bool eval_state_alloc(void);
void eval_state_free(void);

/* What one thread's stack costs, so a `Threads` setting can be reported before it is
 * multiplied by a hundred. */
size_t eval_state_bytes(void);

void eval_state_clear(void);
void eval_state_push(const Position *pos, Move m);
void eval_state_push_null(const Position *pos);
void eval_state_pop(void);

#else

static inline bool eval_state_alloc(void) { return true; }
static inline void eval_state_free(void) {}
static inline size_t eval_state_bytes(void) { return 0; }

static inline void eval_state_clear(void) {}
static inline void eval_state_push(const Position *pos, Move m) {
    (void)pos;
    (void)m;
}
static inline void eval_state_push_null(const Position *pos) { (void)pos; }
static inline void eval_state_pop(void) {}

#endif

#endif
