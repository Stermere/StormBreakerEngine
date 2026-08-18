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

/* Depth used when the caller does not specify one. Raise it as the engine gets
 * faster; a bench should take a few seconds, not a few milliseconds. */
#define BENCH_DEFAULT_DEPTH 13

/* Runs the benchmark and prints, as the final line:
 *     <nodes> nodes <nps> nps
 * Pass depth <= 0 for BENCH_DEFAULT_DEPTH. */
void bench_run(int depth);

#endif /* BENCH_H */
