/*
 * timeman.h - clock handling.
 *
 * Time management is worth a surprising amount of Elo: an engine that thinks
 * well but allocates badly loses on time or wastes its advantage. It is also
 * one of the easiest things to test with SPRT, because the change is confined
 * to this module.
 */
#ifndef TIMEMAN_H
#define TIMEMAN_H

#include "search.h"
#include "types.h"

/* Milliseconds from an unspecified monotonic origin. Only differences are
 * meaningful. Monotonic on purpose: a wall clock that jumps backwards (NTP,
 * DST) would otherwise hand the search a negative elapsed time. */
int64_t time_ms(void);

typedef struct {
    int64_t optimum; /* target: stop here unless the position looks unstable */
    int64_t maximum; /* hard ceiling: never exceed this, ever */
} TimeManager;

/*
 * Computes the budget for one move.
 *
 * The current policy is the workable starting point and nothing more: divide
 * the remaining clock by an assumed twenty moves left, bank three quarters of
 * the increment, and cap the whole thing well short of flag fall. `Move
 * Overhead` is held back for GUI and network latency.
 *
 * TODO(engine): refine it - spend longer when the best move keeps changing
 * between iterations and less when the score is stable. Time management is
 * worth real Elo and is confined to this module, so it SPRTs cleanly.
 */
void timeman_init(TimeManager *tm, const SearchLimits *limits, Color us, int gamePly);

#endif /* TIMEMAN_H */
