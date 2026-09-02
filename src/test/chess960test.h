/*
 * chess960test.h - the Chess960 structural gate.
 *
 * Not part of playing chess: this is `make chess960-test`, checking the things
 * perft cannot see. See chess960test.c for what those are and why perft cannot
 * see them.
 */
#ifndef CHESS960TEST_H
#define CHESS960TEST_H

/* Runs the whole suite, printing a line per section. Returns the number of
 * failures - nonzero also when nothing ran, so a Makefile gate cannot pass by
 * testing nothing. */
int chess960_selftest(void);

/* Prints Chess960 start position `idx` as a FEN, or every one of them when
 * `idx` is negative. The `chess960 sp` command: what the numbering actually
 * produces, for a human to compare against a published table. */
int chess960_print_startpos(int idx);

#endif /* CHESS960TEST_H */
