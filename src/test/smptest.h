/* smptest.h - the parallel-search gate, `make smp-test`: what a node count cannot check. */
#ifndef SMPTEST_H
#define SMPTEST_H

/* Up to `maxThreads` (0: what the machine has). Returns the number of failures; nonzero
 * also when nothing ran. */
int smp_selftest(int maxThreads);

#endif
