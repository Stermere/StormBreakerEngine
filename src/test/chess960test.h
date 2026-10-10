/* chess960test.h - the Chess960 structural gate, `make chess960-test`. */
#ifndef CHESS960TEST_H
#define CHESS960TEST_H

/* Returns the number of failures; nonzero also when nothing ran. */
int chess960_selftest(void);

/* `chess960 sp`: the start position for `idx`, or all 960 when negative. */
int chess960_print_startpos(int idx);

#endif
