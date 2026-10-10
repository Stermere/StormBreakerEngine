/* timeman.h - clock handling. */
#ifndef TIMEMAN_H
#define TIMEMAN_H

#include "search.h"
#include "types.h"

/* Monotonic milliseconds from an unspecified origin. */
int64_t time_ms(void);

typedef struct {
    int64_t optimum;
    int64_t maximum;
} TimeManager;

void timeman_init(TimeManager *tm, const SearchLimits *limits, Color us, int gamePly);

/* The soft target, scaled by best-move stability (iterations agreeing) and by the share
 * of nodes spent under the best move. Never above `maximum`. */
int64_t timeman_optimum(const TimeManager *tm, int stability, int bestNodesPermille);

/* False for depth, nodes, infinite and bench. Anything that cuts a search short to save
 * time must check this, or fixed-depth searches and bench change. */
bool timeman_has_clock(const TimeManager *tm);

#endif
