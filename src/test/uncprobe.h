/*
 * uncprobe.h - what unc_scale() is actually looking at, measured on a real
 * tree.
 *
 * The margin scaling in search.c maps a per-node uncertainty signal - the
 * network's sigma head, or |correction| when the loaded net has no head - onto
 * a percentage that multiplies five pruning margins. Its constants are
 * CENTRED on that signal's distribution: E20 and E21 chose them so the
 * node-weighted average scale is ~100, which is what makes the margins the
 * SPSA fit sees the ones it fitted, and the conditioning a conditioning rather
 * than a disguised global shift of every margin at once.
 *
 * A distribution belongs to a net. Retrain, and the mapping is centred on one
 * that no longer exists - silently, because every score it produces is still
 * plausible. That has already happened twice: E20 recorded its constants as
 * centred on the net before gen-4, and E21's were centred on the net before
 * gen-5. Both times the discovery came later than the net did.
 *
 * So the measurement is kept here rather than re-improvised each time: search
 * a corpus with the scaling HELD NEUTRAL - so the constants under measurement
 * cannot shape the tree they are measured on - record the signal at every node
 * that consults the mapping, and report the constants that re-centre it onto
 * what came back.
 *
 * It sits beside the acceptance gates because it is the same kind of thing
 * they are: not engine code. It differs from them in the one way that decides
 * where the hook goes - a gate has to run inside the build that plays, and
 * this cannot, since measuring the mapping means switching it off. So it is
 * compiled in by its own target, out of the same sources at the same commit,
 * carrying the same net.
 */
#ifndef UNCPROBE_H
#define UNCPROBE_H

/*
 * COMPILED IN BY `make unc-probe` AND NOTHING ELSE.
 *
 * The hook this needs sits inside unc_scale(), which runs at every node, so a
 * playing build gets the identity below: no flag to load, no branch to
 * predict, nothing in the binary at all. The measurement gets its own binary,
 * which it wants anyway - it answers every search neutrally, which is the
 * whole point of it and also makes it useless for playing.
 */
#ifdef UNC_PROBE

#include <stdbool.h>

/*
 * Records one node's signal, in centipawns, and returns the scale the search
 * should use: 100 - neutral - by default, and `scale`, the mapping's own
 * answer, under `-live`.
 *
 * `scale` is passed in rather than recomputed here so the mapping stays
 * written exactly once, in unc_scale().
 */
int unc_probe(int signal, int scale);

/*
 * Pairs the signal a node read with the error the search went on to find
 * there, `searched - staticEval`, signed. `probe err` asks the question the
 * distribution above cannot: whether the signal is telling the truth, and in
 * particular whether its confident band has a thin enough TAIL to justify
 * pruning harder there than a straight line would.
 *
 * `decisive` marks a mate or tablebase score - a different kind of fact, whose
 * size means nothing - and it is counted rather than measured.
 */
void unc_probe_residual(int signal, int err, bool exact, bool decisive, int depth);

/* The `probe unc` / `probe err` commands; `args` is everything after the
 * subcommand. Non-zero on a usage, corpus or file error, which becomes the
 * exit code. */
int unc_probe_command(char *args);
int unc_probe_err_command(char *args);

#else

static inline int unc_probe(int signal, int scale) {
    (void)signal;
    return scale;
}

#endif /* UNC_PROBE */

#endif /* UNCPROBE_H */
