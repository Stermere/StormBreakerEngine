/*
 * bench.h - the deterministic node-count benchmark.
 *
 * `bench` searches a fixed list of positions to a fixed depth and prints the
 * total node count and speed. It serves two distinct purposes:
 *
 *   1. SPEED. nps is the headline number when judging an optimisation.
 *
 *   2. IDENTITY. The node count is a fingerprint of the search. Two builds
 *      that produce the same count are searching identically, so a "pure
 *      speedup" patch can be proven not to have changed behaviour. OpenBench
 *      relies on this: it runs bench on every worker and rejects the engine if
 *      the counts disagree.
 *
 * Because of (2), the output must depend on nothing but the code: no clocks,
 * no thread counts, no random seeds, no hash size. Anything that varies
 * between runs makes the engine unusable for distributed testing.
 */
#ifndef BENCH_H
#define BENCH_H

/*
 * Depth used when the caller does not specify one.
 *
 * Tuned to the engine as it stands: plain alpha-beta with a material-only
 * evaluation reaches this in about seven seconds across the position list
 * below. Depth 6 would take under two, which is too short for the nps figure
 * to settle.
 *
 * RAISE THIS, NEVER THE POSITION LIST. Adding or changing a position
 * invalidates every historical node count at a stroke; raising the depth also
 * changes the count, but leaves old numbers meaningful for the depth they were
 * measured at. Expect to raise it as soon as the transposition table lands -
 * that alone typically cuts the node count several-fold.
 */
#define BENCH_DEFAULT_DEPTH 7

/* Runs the benchmark and prints, as the final line:
 *     <nodes> nodes <nps> nps
 * Pass depth <= 0 for BENCH_DEFAULT_DEPTH. */
void bench_run(int depth);

#endif /* BENCH_H */
