/* timeman.h - clock handling. */
#ifndef TIMEMAN_H
#define TIMEMAN_H

#include "search.h"
#include "types.h"

/* Monotonic, from an unspecified origin: a wall clock that jumps backwards would
 * hand the search a negative elapsed time. */
int64_t time_ms(void);

typedef struct {
    int64_t optimum;
    int64_t maximum;
} TimeManager;

/* Budget for one move. `Move Overhead` is held back once per move still to be
 * played rather than once for this one - with an increment the clock converges,
 * and what it converges on has to cover every later move's latency too. */
void timeman_init(TimeManager *tm, const SearchLimits *limits, Color us, int gamePly);

/* `stability` is how many consecutive iterations agreed on the best move, and
 * `bestNodesPermille` the share of the search's nodes spent under it; a settled
 * search hands its time to later moves. Only the soft target moves - `maximum` is
 * enforced separately, so an unstable position cannot flag. */
int64_t timeman_optimum(const TimeManager *tm, int stability, int bestNodesPermille);

/* False for `go depth`, `go nodes`, `go infinite` and bench, where the budget is a
 * sentinel rather than a deadline. Anything that shortens a search to save time must
 * ask this first: a fixed-depth search was asked for a depth and owes exactly that,
 * and bench node counts are reproducible only because nothing abbreviates them. */
bool timeman_has_clock(const TimeManager *tm);

#endif
