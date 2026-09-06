/*
 * bench.h - the deterministic node-count benchmark.
 *
 * The node count is a fingerprint of the search: two builds that print the same
 * count are searching identically, so it must depend on nothing but the code -
 * no clocks, thread counts, seeds or hash size.
 */
#ifndef BENCH_H
#define BENCH_H

/* Raise this, never the position list: a changed position invalidates every
 * historical node count, where a deeper search only re-bases them. */
#define BENCH_DEFAULT_DEPTH 7

/* Prints "<nodes> nodes <nps> nps" as its final line; depth <= 0 means the default. */
void bench_run(int depth);

/* The frozen list, shared with `probe unc` so it searches exactly this tree - a
 * probe taken on a drifted corpus is not comparable. bench_position returns NULL
 * past the end. */
int bench_position_count(void);
const char *bench_position(int i);

#endif
