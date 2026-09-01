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
 *   * Retraining WIDER (1024 -> 1536 -> ...) is a drop-in. Export, point
 *     EVALFILE at it, rebuild. No C changes, up to NNUE_MAX_HIDDEN, provided
 *     the width is a multiple of NNUE_WIDTH_MULTIPLE.
 *   * Retraining with more or fewer OUTPUT BUCKETS is a drop-in, up to
 *     NNUE_MAX_OUTPUT_BUCKETS.
 *   * Retraining the same architecture on better data is a drop-in.
 *   * The ACTIVATION and the FEATURE SET are NOT choices. One of each is
 *     implemented - SCReLU over 32 mirrored king squares - because the
 *     inference path is written for it: no branch per unit, no branch per
 *     piece. Both are still enum-tagged in this header, and the loader rejects
 *     a tag it was not built for BY NAME, which is what makes a stale net a
 *     one-line fix instead of a morning.
 *   * A NEW activation or feature set needs C code, because those are
 *     arithmetic rather than shape. The places that need one are marked
 *     `UPGRADE POINT` in nnue.c. Add it, extend the enum with a NEW number,
 *     and the exporter writes the tag straight out of the checkpoint.
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

/*
 * Bumped whenever the on-disk layout or the meaning of a tag changes. A net
 * written by an older exporter must fail loudly rather than be misread.
 *
 * 2: one architecture. SCReLU over 32 mirrored king squares, output buckets by
 *    piece count. The v1 tags were renumbered rather than retired, which is
 *    exactly what a version bump licenses - every v1 net fails on the version
 *    before anything can read a tag as the wrong thing.
 */
#define NNUE_FORMAT_VERSION 2u

#define NNUE_MAGIC     "CKNNUE\0\0" /* 8 bytes, NUL-padded, never NUL-terminated */
#define NNUE_MAGIC_LEN 8u
#define NNUE_TAG_LEN   32u

/*
 * How the accumulator is squashed before the output layer.
 *
 * SCReLU squares the clamped activation, so a term carries QA^2 rather than QA
 * and the output sum is divided by QA before the bias is added.
 */
typedef enum {
    NNUE_ACT_SCRELU = 1 /* clamp(x, 0, QA)^2, rescaled by QA */
} NnueActivation;

/*
 * Which (king, piece, square) indexing the net was trained with. A mirrored
 * king stands on one of 32 squares, and the net indexes that square directly.
 *
 * UPGRADE POINT: an indexing that is not "king slot x 12 planes x 64 squares"
 * needs its own case in nnue_feature_index() as well as a slot count.
 *
 * Deliberately NOT shared with eval.c's king_bucket(): the point of the tag is
 * that the net's indexing can move on while the classical evaluation's stays
 * where the tuner fitted it.
 */
typedef enum {
    NNUE_FEATURES_HALFKA_32SQ = 1 /* 32 king squares x 12 planes x 64 squares = 24576 */
} NnueFeatureSet;

/*
 * The hidden width must be a multiple of this.
 *
 * The accumulator and the output layer are both walked 16 int16 lanes at a
 * time on AVX2, and a remainder loop is code that runs on no net anyone would
 * train - every width worth using is a multiple of 64 already. So the loader
 * requires it rather than handling it, and says so by name.
 */
#define NNUE_WIDTH_MULTIPLE 16

/*
 * Ceiling on output buckets, which are selected by piece count.
 *
 * A bucket count must divide 32 so that (pieceCount - 2) / (32 / buckets)
 * covers every row: 32 is the most buckets that can differ, one per piece
 * count. The bound is here rather than in nnue.c because it is a property of
 * the file format - a header claiming more is malformed, not unsupported.
 */
#define NNUE_MAX_OUTPUT_BUCKETS 32

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
 *     int16  uncWeight[outputBuckets][2 * hidden]   only when reserved[0] == 1
 *     int32  uncBias[outputBuckets]                 only when reserved[0] == 1
 *
 * reserved[0] is the UNCERTAINTY flag: 1 means the payload carries a second
 * output head, trained to predict the value head's own |error| against the
 * search label, which search.c uses to scale its pruning margins. A flag in
 * the reserved bytes rather than a version bump because a headless net is
 * still a complete net: both engine generations load it identically, and an
 * engine too old for the flag rejects a flagged net on the payload size
 * rather than misreading it.
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
    uint32_t features;      /* 6144 or 24576, per featureSet */
    uint32_t hidden;        /* per-perspective width */
    uint32_t outputBuckets; /* selected by piece count; divides 32 */
    uint32_t qa;            /* accumulator scale, 255 */
    uint32_t qb;            /* output weight scale, 64 */
    int32_t scale;          /* centipawns per unit of float output, 400 */
    uint32_t payloadBytes;  /* weight bytes following this header */
    char tag[NNUE_TAG_LEN]; /* free-form provenance: training run, epoch, commit */
    uint8_t reserved[16];   /* [0] uncertainty flag; the rest zero, room for more */
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

/* Whether the loaded net carries the uncertainty head. Constant between loads,
 * so callers may branch on it once per node without paying for the nets that
 * lack it. */
bool nnue_has_uncertainty(void);

/* The head's prediction of the evaluation's own |error| in centipawns, >= 0.
 * One extra output-row pass over the accumulator the evaluation already keeps.
 * Meaningless (and asserted against) on a net without the head - check
 * nnue_has_uncertainty() first. */
Value nnue_uncertainty(const Position *pos);

/* Short hex prefix of the loaded net's SHA-256, for the bench header. A node
 * count that cannot be attributed to a specific net is not a measurement. */
const char *nnue_hash(void);

/* One line naming the net: architecture, width, provenance tag, hash. */
void nnue_print_info(void);

/*
 * Reads the `<raw> <cp> <fen>` vectors written by tools/export_net.py,
 * recomputes both numbers here, and requires EXACT equality on every line.
 * Returns 0 on success.
 *
 * Exact, not close: the quantised network is integer arithmetic and integer
 * arithmetic is reproducible. A tolerance here is a bug generator - a
 * disagreement of one means something rounds differently, and that something
 * will be worth 20 Elo in a position that matters.
 */
int nnue_verify_vectors(const char *path);

#endif /* EVAL_NNUE */
#endif /* NNUE_H */
