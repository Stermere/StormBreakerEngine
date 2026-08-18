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
 * TODO(engine): implement.
 *
 * A workable starting point for `movestogo == 0` (sudden death + increment):
 *     optimum = remaining / 20 + increment * 3 / 4
 *     maximum = min(remaining * 0.8, optimum * 5)
 * Then refine: spend longer when the best move keeps changing between
 * iterations, less when the score is stable, and always keep `Move Overhead`
 * in reserve for network and GUI latency.
 */
void timeman_init(TimeManager *tm, const SearchLimits *limits, Color us, int gamePly);

#endif /* TIMEMAN_H */
