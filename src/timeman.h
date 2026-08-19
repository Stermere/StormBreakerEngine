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
 * Divide the remaining clock by an assumed twenty moves left, bank three
 * quarters of the increment, scale by where the game is, and cap the whole
 * thing well short of flag fall. `Move Overhead` is held back for GUI and
 * network latency.
 *
 * Every constant in here is a guess that has not been through an SPRT yet.
 * Time management is worth real Elo and is confined to this module, so it
 * tests cleanly - see docs/TESTING.md.
 */
void timeman_init(TimeManager *tm, const SearchLimits *limits, Color us, int gamePly);

/*
 * The optimum allocation, adjusted for how settled the search looks.
 *
 * `stability` is the number of consecutive completed iterations that agreed on
 * the best move. A search whose best move keeps changing has not found the
 * point of the position yet and is exactly where an extra iteration pays; one
 * that has returned the same move six times running is not about to change its
 * mind, and the time is worth more on a later move.
 *
 * This only ever moves the soft target. `maximum` is a hard ceiling and is
 * enforced separately, so an unstable position cannot talk the search into
 * flagging.
 */
int64_t timeman_optimum(const TimeManager *tm, int stability);

#endif /* TIMEMAN_H */
