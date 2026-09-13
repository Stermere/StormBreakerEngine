/* movepicktest.h - acceptance gate and narrow bridge to the private search picker. */
#ifndef MOVEPICKTEST_H
#define MOVEPICKTEST_H

#include "../board.h"

typedef struct {
    Move tt, killer0, killer1, counter, excluded;
} PickerTestMoves;

typedef struct {
    int count;
    int tacticalCount, quietCount;
    ScoredMove moves[MAX_MOVES];
} PickerTestResult;

/* These bridges allocate isolated history state, never access the playing threads,
 * and exercise the same picker functions negamax calls. */
bool search_test_picker(const Position *pos, PickerTestMoves hints, int limit,
                        PickerTestResult *out);
int search_test_picker_contracts(void);
uint64_t search_test_picker_perft(Position *pos, int depth);
int movepick_selftest(void);

#endif