/* syzygytest.h - the tablebase gate, `make syzygy-test`. */
#ifndef SYZYGYTEST_H
#define SYZYGYTEST_H

/* Returns the number of failures; nonzero also when the tables are missing. */
int syzygy_verify_suite(const char *path);

/* Checks the prober against the sealed manifest, one checksum per material configuration
 * from the campaign against Fathom (E24). Returns the configurations that differ; nonzero
 * also when a file cannot be read. */
int syzygy_verify_manifest(const char *tbPath, const char *manifestPath);

#endif
