/*
 * smptest.h - the parallel-search gate.
 *
 * Not part of playing chess: this is `make smp-test`, checking the things a node
 * count cannot. Bench proves a one-thread search is unchanged; nothing it can
 * measure says whether the pool starts, stops, parks and leaves nothing behind.
 */
#ifndef SMPTEST_H
#define SMPTEST_H

/* Runs the whole suite, a line per check, up to `maxThreads` (clamped to what the
 * machine has when it is 0). Returns the number of failures - nonzero also when
 * nothing ran, so a Makefile gate cannot pass by testing nothing. */
int smp_selftest(int maxThreads);

#endif
