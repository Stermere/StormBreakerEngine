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
 * Every weight lives in evalparams.c and is enumerated by evalparams.h, which
 * is what lets `make tuner` refit all of them. docs/TUNING.md is the procedure.
 */
Value eval_evaluate(const Position *pos);

/*
 * The hand-written evaluation above, by name.
 *
 * eval_evaluate() IS this in an `EVAL=classical` build and is the network in
 * the default one, so anything that specifically wants the classical model -
 * tools/tuner.c, which fits it, and `eval`, which traces it - must ask for it
 * by this name or it will quietly measure whichever evaluation got linked.
 */
Value eval_classical(const Position *pos);

/* Human-readable term-by-term breakdown - the UCI `eval` command. Invaluable
 * when a game is lost to an evaluation blind spot and you need to see why. */
void eval_trace(const Position *pos);

/*
 * Evaluation state carried across make/unmake.
 *
 * The classical evaluation has none - it reads the board and nothing else - so
 * in a classical build these are empty inline functions and the search calls
 * them for free. The network has an accumulator whose entire purpose is not
 * being recomputed at every node, so it keeps a per-ply stack: push after the
 * move is played, pop before it is retracted.
 *
 * CORRECTNESS DOES NOT DEPEND ON THESE BEING CALLED. Every level records the
 * Zobrist key it describes and rebuilds itself from the board when that key
 * does not match, so a push that goes missing costs a full recomputation
 * rather than a wrong score. Speed very much does depend on it.
 *
 * `eval_state_clear` belongs to invariant 7 in CLAUDE.md: search_clear() has
 * to reset everything that carries between searches, and this now carries.
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

#endif /* EVAL_H */
