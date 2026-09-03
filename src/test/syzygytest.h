/*
 * syzygytest.h - the tablebase acceptance gate.
 *
 * Not part of playing chess: this is `make syzygy-test`, checking that the
 * loaded tables answer known endgames with the known result. See syzygytest.c
 * for why the suite holds what it holds.
 */
#ifndef SYZYGYTEST_H
#define SYZYGYTEST_H

#include <stdint.h>

#include "board.h"

/* Loads tables from `path`, probes the built-in suite, prints a line per case
 * and returns the number of failures (nonzero also when the tables are
 * missing, so a Makefile gate cannot pass by not testing anything). */
int syzygy_verify_suite(const char *path);

/* ------------------------------------------------------ position generator --
 *
 * Enumerates every material configuration up to a piece count and produces
 * seeded random legal positions of each, so that a probe can be checked
 * against an oracle over the whole space rather than over a handful of
 * positions somebody thought of.
 *
 * Material coverage is exhaustive BY CONSTRUCTION - the configurations are
 * enumerated, not sampled - and only the placement within a configuration is
 * random. That split is the point: a prober that is wrong for one endgame is
 * wrong for a whole table, and sampling positions uniformly would find such a
 * table only in proportion to its size.
 *
 * Everything is a pure function of (config, seed), so a run is reproducible
 * from two integers. That is what lets an oracle's verdict be frozen as a
 * checksum and re-checked long after the oracle itself is gone.
 */

/* Configurations with at most `maxMen` pieces, kings included. */
int tbgen_config_count(int maxMen);

/* "KQvKR", into a buffer of at least 16 bytes. */
void tbgen_config_name(int maxMen, int config, char *buf, size_t cap);

/* Men in this configuration, kings included. */
int tbgen_config_men(int maxMen, int config);

/* A legal position of that exact material, with no castling rights, no en
 * passant square and a zero halfmove clock - the conditions a WDL probe needs.
 * False when `seed` produced nothing legal, which is normal for cramped
 * material; ask again with the next seed. */
bool tbgen_position(int maxMen, int config, uint64_t seed, Position *pos);

/* ------------------------------------------------------- checksum manifest --
 *
 * The differential campaign against Fathom (docs/EXPERIMENTS.md E24) proved
 * this prober correct over millions of positions, and then Fathom was deleted.
 * The manifest is what keeps that proof: Fathom's own answers, reduced to one
 * checksum per material configuration, in a file small enough to check in.
 *
 * So a change to the prober is still checked against an independent
 * implementation's verdict long after that implementation is gone - and a
 * failure names the endgame rather than just saying no.
 */

/* Folds one probe result into a configuration's checksum. Both the tool that
 * seals a manifest and the gate that verifies one call this, so there is
 * exactly one definition of what is being compared. */
uint64_t tbgen_checksum(uint64_t acc, int wdl, int dtz);

/* One configuration's checksum, using THIS build's prober. The sealer that
 * produced the committed manifest computed the same thing against Fathom;
 * see the note in syzygytest.c and docs/EXPERIMENTS.md E24. */
uint64_t tbgen_config_checksum(int maxMen, int config, uint64_t seed, long per);

/* Re-derives every configuration's checksum with THIS prober and compares
 * against `manifestPath`. Returns the number of configurations that differ;
 * non-zero also when the manifest or the tables cannot be read, so a gate
 * cannot pass by checking nothing. */
int syzygy_verify_manifest(const char *tbPath, const char *manifestPath);

#endif /* SYZYGYTEST_H */
