/*
 * syzygytest.h - the tablebase acceptance gate.
 *
 * Not part of playing chess: this is `make syzygy-test`, checking that the loaded
 * tables answer known endgames with the known result.
 */
#ifndef SYZYGYTEST_H
#define SYZYGYTEST_H

#include <stddef.h>
#include <stdint.h>

#include "board.h"

/* Probes the built-in suite and returns the number of failures - nonzero also when
 * the tables are missing, so a gate cannot pass by not testing anything. */
int syzygy_verify_suite(const char *path);

/*
 * Enumerates every material configuration up to a piece count and produces seeded
 * random legal positions of each, so a probe can be checked over the whole space
 * rather than a handful of positions somebody thought of. Material coverage is
 * exhaustive by construction and only placement is random: a prober that is wrong is
 * wrong for a whole table, which uniform sampling would find only in proportion to
 * that table's size.
 */
int tbgen_config_count(int maxMen);

/* "KQvKR", into a buffer of at least 16 bytes. */
void tbgen_config_name(int maxMen, int config, char *buf, size_t cap);

/* Kings included. */
int tbgen_config_men(int maxMen, int config);

/* A legal position of exactly that material, with no castling rights, no en passant
 * square and a zero halfmove clock - the conditions a WDL probe needs. False when
 * `seed` produced nothing legal, which is normal for cramped material. */
bool tbgen_position(int maxMen, int config, uint64_t seed, Position *pos);

/*
 * The differential campaign against Fathom (docs/EXPERIMENTS.md E24) proved this
 * prober over millions of positions, and then Fathom was deleted; the manifest keeps
 * that proof as one checksum per material configuration. Every generated position is
 * a pure function of (config, seed), which is what lets an oracle's verdict be frozen
 * and re-checked long after the oracle is gone.
 *
 * Both the tool that seals a manifest and the gate that verifies one fold results in
 * through here, so there is exactly one definition of what is being compared.
 */
uint64_t tbgen_checksum(uint64_t acc, int wdl, int dtz);

/* One configuration's checksum using THIS build's prober; the committed manifest
 * holds the same computation run against Fathom. */
uint64_t tbgen_config_checksum(int maxMen, int config, uint64_t seed, long per);

/* Number of configurations that differ, and non-zero also when the manifest or the
 * tables cannot be read. */
int syzygy_verify_manifest(const char *tbPath, const char *manifestPath);

#endif
