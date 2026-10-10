/*
 * nnue.h - the network evaluation and its file format. A net file carries its own shape
 * (invariant 8), so a wider net is a drop-in; a new activation or feature set needs code
 * in nnue.c and a case in the loader.
 */
#ifndef NNUE_H
#define NNUE_H

#include <stdbool.h>
#include <stdint.h>

#include "board.h"
#include "types.h"

typedef struct EvalState EvalState;

#ifdef EVAL_NNUE

/* Compile-time bounds for stack arrays; a larger net is rejected at load by name. */
#define NNUE_MAX_HIDDEN      2048
#define NNUE_MAX_STACK_WIDTH 128

/* Bumped whenever the layout or a tag's meaning changes. */
#define NNUE_FORMAT_VERSION 3u

#define NNUE_MAGIC     "CKNNUE\0\0"
#define NNUE_MAGIC_LEN 8u
#define NNUE_TAG_LEN   32u

/* SCReLU squares the clamped activation. PAIRWISE (layer stack only) multiplies the two
 * clamped halves of each accumulator, `(x * y) >> log2(QA)`, halving L1's input. */
typedef enum { NNUE_ACT_SCRELU = 1, NNUE_ACT_PAIRWISE = 2 } NnueActivation;

/* HalfKA with the king mirrored onto 32 squares, each its own slot. */
typedef enum { NNUE_FEATURES_HALFKA_32SQ = 1 } NnueFeatureSet;

/* Every width must be a multiple of the 16-lane vectors, so no tail loops. */
#define NNUE_WIDTH_MULTIPLE 16

/* Output buckets by piece count; the count must divide 32. */
#define NNUE_MAX_OUTPUT_BUCKETS 32

/*
 * Little-endian, a fixed 112-byte header followed by the payload:
 *
 *     int16  ftWeight[features][hidden]   feature-major: one row per feature
 *     int16  ftBias[hidden]
 *     int16  l1Weight[outputBuckets][l1Size][l1Inputs]     only when l1Size > 0;
 *                                         l1Inputs is 2 * hidden, or hidden if pairwise
 *     int32  l1Bias[outputBuckets][l1Size]                 only when l1Size > 0
 *     int16  l2Weight[outputBuckets][l2Size][l1Size]       only when l2Size > 0
 *     int32  l2Bias[outputBuckets][l2Size]                 only when l2Size > 0
 *     int16  outWeight[outputBuckets][trunk]
 *     int32  outBias[outputBuckets]
 *     int16  uncWeight[outputBuckets][trunk]        only when reserved[0] == 1
 *     int32  uncBias[outputBuckets]                 only when reserved[0] == 1
 *
 * where `trunk`, which both heads read, is `2 * hidden` with no stack and the stack's
 * last layer with one. l1Size == 0 is the flat architecture, byte-identical to version 2.
 * reserved[0] flags the uncertainty head, which predicts the value head's |error|; an
 * older engine rejects a flagged net on its payload size.
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

    /* The layer stack; zero means absent. Each shift requantises a layer's int32 sum. */
    uint32_t l1Size;
    uint32_t l2Size;
    uint32_t l1Shift;
    uint32_t l2Shift;

    char tag[NNUE_TAG_LEN];
    uint8_t reserved[16];
} NnueHeader;

/* Loads the embedded net. Fatal on failure. */
void nnue_init(void);

/* The `EvalFile` option. Keeps the current net on failure. */
bool nnue_load_file(const char *path);

/* From scratch, side to move, clamped like eval_evaluate(). */
Value nnue_evaluate(const Position *pos);

/* Whether the net has the uncertainty head and the `UncertaintyHead` option is on. */
bool nnue_has_uncertainty(void);

/* The `UncertaintyHead` option. Off, the head is never computed: margins fall back to
 * correction history and z-LMR is off. Not during a search. Returns whether the net has
 * the head at all. */
bool nnue_set_uncertainty(bool on);

/* The head's predicted |error| of the evaluation, in centipawns. Only when
 * nnue_has_uncertainty(). */
Value nnue_uncertainty(EvalState *es, const Position *pos);

/* Hex SHA-256 of the loaded net, for the bench header. */
const char *nnue_hash(void);

void nnue_print_info(void);

/* Checks the vectors from tools/export_net.py for exact equality; 0 on success. */
int nnue_verify_vectors(const char *path);

#endif
#endif
