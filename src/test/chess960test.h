/*
 * chess960test.h - the Chess960 structural gate.
 *
 * Not part of playing chess: this is `make chess960-test`, checking the things
 * perft cannot see.
 */
#ifndef CHESS960TEST_H
#define CHESS960TEST_H

/* Runs the whole suite, a line per section. Returns the number of failures -
 * nonzero also when nothing ran, so a Makefile gate cannot pass by testing
 * nothing. */
int chess960_selftest(void);

/* The `chess960 sp` command: what the numbering actually produces, for a human to
 * compare against a published table. A negative `idx` prints all 960. */
int chess960_print_startpos(int idx);

#endif
