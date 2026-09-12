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

/*
 * The network's per-thread evaluation state: the accumulator stack, the refresh cache,
 * and the layer stack a stacked net needs. Opaque here - nnue.c owns the layout.
 *
 * It is PASSED rather than reached for, and that is a speed decision as much as a
 * clarity one. Either way it is per-thread state under invariant 11, but the Windows
 * toolchain this engine is built with has no native thread-local storage: GCC is
 * configured `--enable-threads=posix`, so every reference to a `_Thread_local` compiles
 * into a call to __emutls_get_address(). At four of those per node - push, pop, and the
 * two output heads - that was about a tenth of the whole search. A pointer the caller
 * already has in hand costs nothing, here or on any other platform.
 */
typedef struct EvalState EvalState;

Value eval_evaluate(EvalState *es, const Position *pos);

/* The classical model by name. eval_evaluate() is the network in a default build,
 * so the tuner and `eval` must ask for this or they measure whatever got linked. */
Value eval_classical(const Position *pos);

/* Term-by-term breakdown - the UCI `eval` command. */
void eval_trace(const Position *pos);

/*
 * The accumulator stack inside that state: push after a move is played, pop before it
 * is retracted. Correctness does not depend on these being called - each level rebuilds
 * from the board when its stored key does not match - but speed does. A NULL state is
 * accepted everywhere and means exactly that: correct, and slow.
 */
#ifdef EVAL_NNUE

/*
 * The calling thread's state, claimed on first use. Every searching thread should call
 * this once and keep what it returns.
 *
 * NULL means the allocation failed, and is not an error: every consumer below accepts it
 * and answers correctly from a full accumulation at each node, which is several times
 * slower. That is also what a caller that never asks gets, which is why a tool wanting
 * one score need not know any of this exists.
 */
EvalState *eval_state(void);

void eval_state_free(void);

/* What one thread's stack costs, so a `Threads` setting can be reported before it is
 * multiplied by a hundred. */
size_t eval_state_bytes(void);

void eval_state_clear(EvalState *es);
void eval_state_push(EvalState *es, const Position *pos, Move m);
void eval_state_push_null(EvalState *es, const Position *pos);
void eval_state_pop(EvalState *es);

#else

/* No state to keep: the classical evaluation reads the board and nothing else. */
static inline EvalState *eval_state(void) { return NULL; }
static inline void eval_state_free(void) {}
static inline size_t eval_state_bytes(void) { return 0; }

static inline void eval_state_clear(EvalState *es) { (void)es; }
static inline void eval_state_push(EvalState *es, const Position *pos, Move m) {
    (void)es;
    (void)pos;
    (void)m;
}
static inline void eval_state_push_null(EvalState *es, const Position *pos) {
    (void)es;
    (void)pos;
}
static inline void eval_state_pop(EvalState *es) { (void)es; }

#endif

#endif
