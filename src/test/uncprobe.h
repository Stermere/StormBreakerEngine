/*
 * uncprobe.h - what unc_scale() is actually looking at, measured on a real tree.
 *
 * The mapping's constants are centred on a signal distribution that belongs to one
 * net, so retraining de-centres them silently - which has already happened twice.
 * This searches a corpus with the scaling held NEUTRAL, so the constants cannot
 * shape the tree they are measured on, and reports what re-centres them.
 */
#ifndef UNCPROBE_H
#define UNCPROBE_H

/* Compiled in by `make unc-probe` and nothing else. The hook sits inside unc_scale(),
 * which runs at every node, so a playing build gets the identity stub below: no flag
 * to load, no branch to predict, nothing in the binary at all. */
#ifdef UNC_PROBE

#include <stdbool.h>

/* Records one node's signal in centipawns and returns the scale the search should
 * use: 100, neutral, by default and `scale` under `-live`. `scale` is passed in
 * rather than recomputed so the mapping stays written exactly once. */
int unc_probe(int signal, int scale);

/* Pairs the signal a node read with the error the search then found there,
 * `searched - staticEval`. Asks whether the signal tells the truth, and in particular
 * whether its confident band has a thin enough tail to justify pruning harder. */
void unc_probe_residual(int signal, int err, bool exact, bool decisive, int depth);

/* The `probe unc` / `probe err` commands; `args` is everything after the subcommand.
 * Non-zero on a usage, corpus or file error, which becomes the exit code. */
int unc_probe_command(char *args);
int unc_probe_err_command(char *args);

#else

static inline int unc_probe(int signal, int scale) {
    (void)signal;
    return scale;
}

#endif

#endif
