/*
 * bench.h - the deterministic node-count benchmark. The count must depend on nothing but
 * the code: no clocks, thread counts, seeds or hash size (invariants 1 and 2).
 */
#ifndef BENCH_H
#define BENCH_H

/* Change the depth if needed, never the positions: those invalidate every node count. */
#define BENCH_DEFAULT_DEPTH 7

/* Prints "<nodes> nodes <nps> nps" as its final line; depth <= 0 means the default. */
void bench_run(int depth);

/* The frozen positions, shared with `probe unc`. NULL past the end. */
int bench_position_count(void);
const char *bench_position(int i);

#endif
