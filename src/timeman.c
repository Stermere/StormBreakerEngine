/*
 * timeman.c - monotonic clock and time allocation.
 *
 * time_ms() is implemented (bench needs it to report nps); the allocation
 * policy is TODO - see timeman.h.
 */
#include "timeman.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <time.h>
#endif

int64_t time_ms(void) {
#if defined(_WIN32)
    /* QueryPerformanceCounter rather than GetTickCount64: the latter has a
     * ~15ms granularity, which is coarse enough to distort nps readings and to
     * matter at the ultra-fast time controls used for SPRT testing. */
    static LARGE_INTEGER frequency;
    LARGE_INTEGER counter;

    if (frequency.QuadPart == 0)
        QueryPerformanceFrequency(&frequency);

    QueryPerformanceCounter(&counter);
    return (int64_t)((counter.QuadPart * 1000) / frequency.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

void timeman_init(TimeManager *tm, const SearchLimits *limits, Color us, int gamePly) {
    (void)us;
    (void)gamePly;

    /* TODO(engine): replace with a real allocation policy. Until then, honour
     * only the unambiguous limits so a fixed-movetime search behaves. */
    if (limits->movetime > 0) {
        tm->optimum = limits->movetime;
        tm->maximum = limits->movetime;
    } else {
        tm->optimum = INT64_MAX;
        tm->maximum = INT64_MAX;
    }
}
