/*
 * nnue.h - the network evaluation, and the file format it loads.
 *
 * The engine is built with exactly ONE evaluation. `make EVAL=nnue` defines
 * EVAL_NNUE and this file's implementation supplies eval_evaluate();
 * `make EVAL=classical` (the default) does not compile any of it, and the
 * resulting binary is byte-for-byte the classical engine. Nothing here is a
 * runtime branch - an evaluation that tested a flag at every node would pay
 * for the flexibility in the only currency that matters.
 *
 * ----------------------------------------------------------------------------
 * UPGRADING THE NETWORK
 * ----------------------------------------------------------------------------
 * The architecture is DATA, not code, wherever it can be. A net file carries
 * its own shape - feature count, hidden width, quantisation scales, activation
 * and feature-set identifiers - and the loader believes the file rather than a
 * constant compiled beside it. The practical consequence:
 *
 *   * Retraining WIDER (512 -> 1024 -> ...) is a drop-in. Export, point
 *     EVALFILE at it, rebuild. No C changes, up to NNUE_MAX_HIDDEN.
 *   * Retraining the same architecture on better data is a drop-in.
 *   * Changing the ACTIVATION or the FEATURE SET needs C code, because those
 *     are arithmetic rather than shape. Both are enum-tagged in the header
 *     (NnueActivation, NnueFeatureSet), the loader rejects a tag it was not
 *     built to handle by name, and the switch statements that need a new case
 *     are marked `UPGRADE POINT` in nnue.c. Add the case, bump the enum, and
 *     the exporter writes the new tag straight out of the checkpoint.
 *   * Changing the RECORD or FILE layout bumps NNUE_FORMAT_VERSION, which
 *     makes every stale net fail loudly at load instead of being misread.
 *
 * tools/export_net.py is the other half of this contract and derives every
 * field above from the PyTorch checkpoint, so there is no pair of constants to
 * keep in sync by hand. `make nnue-test` proves the two agree exactly.
 */
#ifndef NNUE_H
#define NNUE_H

#include <stdbool.h>
#include <stdint.h>

#include "board.h"
#include "types.h"

#ifdef EVAL_NNUE

/*
 * Ceiling on the hidden width this build can hold.
 *
 * The accumulator is a stack array in the evaluation, so it needs a bound
 * known at compile time; a heap allocation on the hot path would cost more
 * than the flexibility is worth. 2048 covers every width on the roadmap at
 * 8 KB of stack. A net wider than this is rejected at load with a message
 * saying to raise exactly this number.
 */
#define NNUE_MAX_HIDDEN 2048

/* Bumped whenever the on-disk layout changes shape. A net written by an older
 * exporter must fail loudly rather than be misread as the new layout. */
#define NNUE_FORMAT_VERSION 1u

#define NNUE_MAGIC     "CKNNUE\0\0" /* 8 bytes, NUL-padded, never NUL-terminated */
#define NNUE_MAGIC_LEN 8u
#define NNUE_TAG_LEN   32u

/*
 * How the accumulator is squashed before the output layer.
 *
 * UPGRADE POINT: SCReLU is worth Elo and is the first planned change, but it
 * needs int32 intermediates in the output sum. Add the case in nnue_output().
 */
typedef enum {
    NNUE_ACT_CRELU  = 0, /* clamp(x, 0, QA) */
    NNUE_ACT_SCRELU = 1  /* clamp(x, 0, QA)^2 - NOT IMPLEMENTED */
} NnueActivation;

/*
 * Which (king, piece, square) indexing the net was trained with.
 *
 * UPGRADE POINT: v1 folds the 64 king squares onto 8 buckets, reusing the
 * classical evaluation's normalisation. Moving to 32 mirrored king squares is
 * a different index function, not a different shape, so it gets a new tag and
 * a new case in nnue_feature_index().
 *
 * Deliberately NOT shared with eval.c's king_bucket(), even though v1 computes
 * the same thing: the whole point of the tag is that the net's indexing can
 * move on while the classical evaluation's stays put.
 */
typedef enum {
    NNUE_FEATURES_HALFKA_8BUCKET = 1 /* 8 king buckets x 12 planes x 64 squares */
} NnueFeatureSet;

/*
 * The file header. Little-endian, fixed 96 bytes, immediately followed by the
 * payload:
 *
 *     int16  ftWeight[features][hidden]   feature-major, so one feature's row
 *                                         is contiguous - which is exactly the
 *                                         span the accumulator adds
 *     int16  ftBias[hidden]
 *     int16  outWeight[outputBuckets][2 * hidden]
 *     int32  outBias[outputBuckets]
 *
 * There is no checksum field on purpose. The SHA-256 is of the whole file, so
 * a field holding it could not describe the bytes containing it; the engine
 * hashes what it actually loaded and prints that.
 */
typedef struct {
    char magic[NNUE_MAGIC_LEN];
    uint32_t formatVersion;
    uint32_t featureSet;    /* NnueFeatureSet */
    uint32_t activation;    /* NnueActivation */
    uint32_t features;      /* 6144 for v1 */
    uint32_t hidden;        /* 512 for v1 */
    uint32_t outputBuckets; /* 1 for v1 */
    uint32_t qa;            /* accumulator scale, 255 */
    uint32_t qb;            /* output weight scale, 64 */
    int32_t scale;          /* centipawns per unit of float output, 400 */
    uint32_t payloadBytes;  /* weight bytes following this header */
    char tag[NNUE_TAG_LEN]; /* free-form provenance: training run, epoch, commit */
    uint8_t reserved[16];   /* zero; room to add a field without a version bump */
} NnueHeader;

/* Loads the embedded net (or EvalFile, if one was set). Fatal on failure: an
 * engine whose evaluation did not load has nothing useful to do. */
void nnue_init(void);

/* Replaces the loaded net at runtime - the `EvalFile` UCI option. Returns
 * false and keeps the current net if the file is missing or malformed, so a
 * typo in a GUI config cannot leave the engine without an evaluation. */
bool nnue_load_file(const char *path);

/* Side-to-move-relative centipawns, the same convention eval_evaluate() uses
 * and clamped to the same safe range - see NNUE_EVAL_LIMIT in nnue.c. */
Value nnue_evaluate(const Position *pos);

/* Short hex prefix of the loaded net's SHA-256, for the bench header. A node
 * count that cannot be attributed to a specific net is not a measurement. */
const char *nnue_hash(void);

/* One line naming the net: architecture, width, provenance tag, hash. */
void nnue_print_info(void);

/*
 * The Task 3 acceptance gate. Reads the `<raw> <cp> <fen>` vectors written by
 * tools/export_net.py, recomputes both numbers here, and requires EXACT
 * equality on every line. Returns 0 on success.
 *
 * Exact, not close: the quantised network is integer arithmetic and integer
 * arithmetic is reproducible. A tolerance here is a bug generator - a
 * disagreement of one means something rounds differently, and that something
 * will be worth 20 Elo in a position that matters.
 */
int nnue_verify_vectors(const char *path);

#endif /* EVAL_NNUE */
#endif /* NNUE_H */
