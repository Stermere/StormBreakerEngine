/*
 * uncprobe.h - measures what unc_scale() reads, to re-centre its constants for a new net.
 * The corpus is searched with the scaling held neutral, so the constants cannot shape the
 * tree they are measured on. Only in `make unc-probe` builds; playing builds get the stub.
 */
#ifndef UNCPROBE_H
#define UNCPROBE_H

#ifdef UNC_PROBE

#include <stdbool.h>

/* Records a node's signal; returns 100 (neutral), or `scale` under `-live`. */
int unc_probe(int signal, int scale);

/* Pairs a node's signal with the error the search found, `searched - staticEval`. */
void unc_probe_residual(int signal, int err, bool exact, bool decisive, int depth);

/* `probe unc` and `probe err`; non-zero on error. */
int unc_probe_command(char *args);
int unc_probe_err_command(char *args);

#else

static inline int unc_probe(int signal, int scale) {
    (void)signal;
    return scale;
}

#endif

#endif
