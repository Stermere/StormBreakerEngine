/* movepicktest.h - the move picker gate, `make movepick-test`, and its bridge into search.c. */
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

/* The picker negamax uses, on private state that never touches the thread pool. */
bool search_test_picker(const Position *pos, PickerTestMoves hints, int limit,
                        PickerTestResult *out);
int search_test_picker_contracts(void);
uint64_t search_test_picker_perft(Position *pos, int depth);
int movepick_selftest(void);

#endif