/* history.h - pawn-structure context for quiet-move history. */
#ifndef HISTORY_H
#define HISTORY_H

#include <assert.h>

#include "types.h"

/* One MiB per thread, not a full position cache. Moves from different transpositions
 * should share evidence when the pawn structure survives. Collisions mix ordering
 * hints only; no score, bound or legality decision is stored here. */
#define PAWN_HISTORY_SIZE 512
#define PAWN_HISTORY_UNIT 128

typedef struct {
    int16_t entries[PAWN_HISTORY_SIZE][PIECE_NB][SQUARE_NB];
} PawnHistory;

_Static_assert((PAWN_HISTORY_SIZE & (PAWN_HISTORY_SIZE - 1)) == 0,
               "pawn history size must be a power of two");

static inline unsigned history_pawn_index(Key key) {
    return (unsigned)(key & (PAWN_HISTORY_SIZE - 1));
}

/* The key and moving piece are read BEFORE making the move, including a pawn move
 * that changes its own context. Piece includes color; a separate side key is redundant. */
static inline int16_t *history_pawn_entry(PawnHistory *history, Key key, Piece pc, Square to) {
    assert(pc > NO_PIECE && (int)pc < PIECE_NB && is_ok_square(to));
    return &history->entries[history_pawn_index(key)][pc][to];
}

static inline int history_pawn_score(const int16_t *entry, int weight) {
    assert(weight >= 0 && weight <= 256);
    return (int)*entry * weight / PAWN_HISTORY_UNIT;
}

/* Extra credit only: an ordinary success must never lose its reward. The caller
 * applies this to a quiet winner's pawn context, not the global histories. Error is
 * the head's predicted median absolute residual, not a cutoff probability; -1 means
 * no head. VALUE_NONE also excludes in-check evals through the decisive-score guard. */
static inline int history_pawn_rescue_credit(int bonus, Value eval, Value beta, Value cutoff,
                                             int error, bool excluded, int weight, int floor) {
    assert(bonus >= 0 && weight >= 0 && weight <= 100 && floor >= 1 && floor <= 256);
    if (weight == 0 || error < 0 || excluded || is_decisive_score(eval) ||
        is_decisive_score(beta) || is_decisive_score(cutoff) || cutoff < beta || eval >= beta)
        return 0;

    const int64_t scale = (int64_t)error + floor;
    const int64_t gap   = (int64_t)beta - eval;
    /* Use beta, not the fail-soft overshoot; only that bound was established. */
    return (int)((int64_t)bonus * weight * (gap < scale ? gap : scale) / (100 * scale));
}

/* Convert a reduced-only attempt to parent-equivalent requested depth. This is
 * evidence requested, not plies actually visited: TT hits and internal pruning may
 * return early. A normal-depth retry restores ordinary credit even if it fails. */
static inline Depth history_pawn_evidence_depth(Depth depth, Depth childDepth, Depth reduction,
                                                bool fullDepthSearch) {
    assert(depth >= 0 && reduction >= 0);
    if (reduction == 0 || fullDepthSearch)
        return depth;
    const Depth evidence = childDepth - reduction + 1;
    return evidence < 0 ? 0 : evidence > depth ? depth : evidence;
}

/* Positive magnitudes in and out; only the pawn table receives the negated result.
 * Blend rather than trusting shallow evidence outright. Rounding retains more of
 * the original penalty, and neither an extension nor tuning may strengthen it. */
static inline int history_pawn_evidence_malus(int ordinary, int shallow, int weight) {
    assert(ordinary >= 0 && shallow >= 0 && weight >= 0 && weight <= 100);
    if (weight == 0 || shallow >= ordinary)
        return ordinary;
    return ordinary - (int)((int64_t)(ordinary - shallow) * weight / 100);
}

#endif