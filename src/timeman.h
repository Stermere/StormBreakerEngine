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

/* `stability` is how many consecutive iterations agreed on the best move; a
 * settled search hands its time to later moves. Only the soft target moves -
 * `maximum` is enforced separately, so an unstable position cannot flag. */
int64_t timeman_optimum(const TimeManager *tm, int stability);

#endif
