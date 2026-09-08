/* historytest.h - the pawn-history gate, never reached while playing. */
#ifndef HISTORYTEST_H
#define HISTORYTEST_H

/* Returns the number of failures, also nonzero if no checks ran. */
int history_selftest(void);

#endif