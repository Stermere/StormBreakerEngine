/* eval.h - static evaluation, from the side to move's point of view. */
#ifndef EVAL_H
#define EVAL_H

#include <stdbool.h>
#include <stddef.h>

#include "board.h"
#include "types.h"

void eval_init(void);

/* Fixed exchange values for move ordering and SEE; the king is zero. */
extern const Value PieceValues[PIECE_TYPE_NB];

/* The network's per-thread state (accumulator stack and caches), owned by nnue.c. Passed
 * down rather than read from a thread-local: on this toolchain every TLS access is a call
 * to __emutls_get_address(), which cost a tenth of the search. */
typedef struct EvalState EvalState;

Value eval_evaluate(EvalState *es, const Position *pos);

/* The classical model by name: eval_evaluate() is the network in a default build. */
Value eval_classical(const Position *pos);

/* The UCI `eval` command. */
void eval_trace(const Position *pos);

/* Every function taking an EvalState accepts NULL: correct, just slower. Push after a move
 * is played and pop before it is undone; a missed push costs speed, never correctness. */
#ifdef EVAL_NNUE

/* The calling thread's state, allocated on first use; NULL if that failed. */
EvalState *eval_state(void);

void eval_state_free(void);

/* Per-thread memory, for reporting a `Threads` setting. */
size_t eval_state_bytes(void);

/* Called at the start of every search. Drops cached evaluations if the net changed. */
void eval_state_clear(EvalState *es);

/* Marks every thread's cached evaluations stale (`ucinewgame`, invariant 7). */
void eval_state_retire(void);
void eval_state_push(EvalState *es, const Position *pos, Move m);
void eval_state_push_null(EvalState *es, const Position *pos);
void eval_state_pop(EvalState *es);

#else

static inline EvalState *eval_state(void) { return NULL; }
static inline void eval_state_free(void) {}
static inline size_t eval_state_bytes(void) { return 0; }

static inline void eval_state_clear(EvalState *es) { (void)es; }
static inline void eval_state_retire(void) {}
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
