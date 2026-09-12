/*
 * nnue.h - the network evaluation, and the file format it loads.
 *
 * The architecture is DATA wherever it can be: a net file carries its own shape and
 * the loader believes the file rather than a constant compiled beside it, so
 * retraining wider or with more output buckets is a drop-in. The activation and
 * feature set are not choices - one of each is implemented, the loader rejects an
 * unknown tag by name, and adding one means C code at the `UPGRADE POINT` marks in
 * nnue.c.
 */
#ifndef NNUE_H
#define NNUE_H

#include <stdbool.h>
#include <stdint.h>

#include "board.h"
#include "types.h"

#ifdef EVAL_NNUE

/* The accumulator is a stack array, so the width needs a compile-time bound; 2048
 * covers the roadmap at 8 KB of stack. A wider net is rejected at load with a
 * message naming this number. */
#define NNUE_MAX_HIDDEN 2048

/* The layer stack's widths are stack arrays too, and they are small by design - other
 * engines run 16 and 32. Generous, and a net past it is rejected by name. */
#define NNUE_MAX_STACK_WIDTH 128

/* Bumped whenever the on-disk layout or the meaning of a tag changes, so a net from
 * an older exporter fails on the version before anything can read a tag as the
 * wrong thing. Version 3 grew the header by the four stack fields below. */
#define NNUE_FORMAT_VERSION 3u

#define NNUE_MAGIC     "CKNNUE\0\0"
#define NNUE_MAGIC_LEN 8u
#define NNUE_TAG_LEN   32u

/* SCReLU squares the clamped activation, so a term carries QA^2 rather than QA and
 * the output sum is divided by QA before the bias is added. */
typedef enum { NNUE_ACT_SCRELU = 1 } NnueActivation;

/* A mirrored king stands on one of 32 squares, which the net indexes directly.
 * Deliberately not shared with eval.c's king_bucket(): the net's indexing can move
 * on while the classical evaluation's stays where the tuner fitted it. */
typedef enum { NNUE_FEATURES_HALFKA_32SQ = 1 } NnueFeatureSet;

/* The accumulator and the output layer are both walked 16 int16 lanes at a time, and
 * every width worth training is a multiple of 64 already, so the loader requires
 * this rather than carrying a remainder loop that would never run. */
#define NNUE_WIDTH_MULTIPLE 16

/* Buckets are selected by piece count, so a count must divide 32. The bound belongs
 * here rather than in nnue.c because it is a property of the file format - a header
 * claiming more is malformed, not unsupported. */
#define NNUE_MAX_OUTPUT_BUCKETS 32

/*
 * Little-endian, fixed 112 bytes, immediately followed by the payload:
 *
 *     int16  ftWeight[features][hidden]   feature-major, so one feature's row is
 *                                         contiguous - the span the accumulator adds
 *     int16  ftBias[hidden]
 *     int16  l1Weight[outputBuckets][l1Size][2 * hidden]   only when l1Size > 0
 *     int32  l1Bias[outputBuckets][l1Size]                 only when l1Size > 0
 *     int16  l2Weight[outputBuckets][l2Size][l1Size]       only when l2Size > 0
 *     int32  l2Bias[outputBuckets][l2Size]                 only when l2Size > 0
 *     int16  outWeight[outputBuckets][trunk]
 *     int32  outBias[outputBuckets]
 *     int16  uncWeight[outputBuckets][trunk]        only when reserved[0] == 1
 *     int32  uncBias[outputBuckets]                 only when reserved[0] == 1
 *
 * where `trunk` is the width of whatever the output heads read: `2 * hidden` with no
 * stack, the stack's last hidden layer with one. BOTH HEADS READ THE SAME TRUNK, which
 * is what keeps them one pass over one vector.
 *
 * l1Size == 0 IS THE FLAT ARCHITECTURE, and its payload is byte-identical to the one
 * version 2 described - `trunk` is then `2 * hidden` and there is no stack to skip. That
 * is deliberate: it makes a version-2 net's re-export under version 3 a change that
 * cannot alter an evaluation, which is a thing a bench node count can prove.
 *
 * reserved[0] flags a second output head, trained to predict the value head's own
 * |error|. It is a flag rather than a version bump because a headless net is still
 * a complete net, and an engine too old for it rejects a flagged net on the payload
 * size rather than misreading it.
 */
typedef struct {
    char magic[NNUE_MAGIC_LEN];
    uint32_t formatVersion;
    uint32_t featureSet;
    uint32_t activation;
    uint32_t features;
    uint32_t hidden;
    uint32_t outputBuckets;
    uint32_t qa;
    uint32_t qb;
    int32_t scale;
    uint32_t payloadBytes;

    /* The stack. Zero means absent, and absent for l1Size means the flat output layer
     * above. The shifts are the requantisation each stage applies to its int32 sum,
     * carried in the file rather than compiled in for the same reason the widths are:
     * retraining at a different scale must not need a C change. */
    uint32_t l1Size;
    uint32_t l2Size;
    uint32_t l1Shift;
    uint32_t l2Shift;

    char tag[NNUE_TAG_LEN];
    uint8_t reserved[16];
} NnueHeader;

/* Loads the embedded net, or EvalFile if one was set. Fatal on failure: an engine
 * whose evaluation did not load has nothing useful to do. */
void nnue_init(void);

/* The `EvalFile` option. Keeps the current net when the file is missing or
 * malformed, so a typo in a GUI config cannot leave the engine without one. */
bool nnue_load_file(const char *path);

/* Side-to-move-relative centipawns, clamped to the range eval_evaluate() promises -
 * see NNUE_EVAL_LIMIT in nnue.c. */
Value nnue_evaluate(const Position *pos);

/* Constant between loads, so callers may branch on it per node without paying for
 * the nets that lack the head. */
bool nnue_has_uncertainty(void);

/* The head's prediction of the evaluation's own |error| in centipawns, >= 0, for one
 * extra output pass over an accumulator the evaluation already keeps. Asserted
 * against on a net without the head - check nnue_has_uncertainty() first. */
Value nnue_uncertainty(const Position *pos);

/* Short hex prefix of the loaded net's SHA-256, for the bench header: a node count
 * that cannot be attributed to a specific net is not a measurement. */
const char *nnue_hash(void);

/* One line naming the net: architecture, width, provenance tag, hash. */
void nnue_print_info(void);

/* Recomputes the `<raw> <cp> <fen>` vectors written by tools/export_net.py and
 * requires EXACT equality on every line; 0 on success. Integer arithmetic is
 * reproducible, so a tolerance would only hide a rounding difference that will be
 * worth 20 Elo in a position that matters. */
int nnue_verify_vectors(const char *path);

#endif
#endif
