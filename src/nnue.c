/*
 * nnue.c - loading and evaluating the network. See nnue.h for the file format.
 *
 * `make nnue-test` requires every number here to match tools/export_net.py's numpy
 * reference exactly on 10,000 positions: a subtly wrong quantisation loses Elo silently.
 * The AVX2 and scalar paths must produce identical integers.
 */
#include "nnue.h"

#ifdef EVAL_NNUE

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "eval.h"

#if defined(__AVX2__)
#include <immintrin.h>
#define NNUE_AVX2 1

/* SCReLU madd results summed in int32 before widening: each is at most 2 * 32767 * 255,
 * so 64 of them fit. */
#define NNUE_SCRELU_FLUSH 64
#endif

/* The exporter packs the header with a fixed struct format; padding would shift every
 * weight. */
_Static_assert(sizeof(NnueHeader) == 112, "NnueHeader must stay 112 bytes; see HEADER_FMT in "
                                          "tools/export_net.py");

/* Keeps a runaway output out of the mate range. */
#define NNUE_EVAL_LIMIT 20000

/* SHA-256, to say which net a build carries. Hand-written: the engine links only libc. */
typedef struct {
    uint32_t state[8];
    uint64_t bits;
    uint8_t buf[64];
    size_t used;
} Sha256;

/* clang-format off */
static const uint32_t Sha256K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
/* clang-format on */

static inline uint32_t sha_ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_block(Sha256 *sh, const uint8_t *p) {
    uint32_t w[64];

    for (int i = 0; i < 16; ++i)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];

    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = sha_ror(w[i - 15], 7) ^ sha_ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = sha_ror(w[i - 2], 17) ^ sha_ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i]              = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = sh->state[0], b = sh->state[1], c = sh->state[2], d = sh->state[3];
    uint32_t e = sh->state[4], f = sh->state[5], g = sh->state[6], h = sh->state[7];

    for (int i = 0; i < 64; ++i) {
        const uint32_t s1 = sha_ror(e, 6) ^ sha_ror(e, 11) ^ sha_ror(e, 25);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t t1 = h + s1 + ch + Sha256K[i] + w[i];
        const uint32_t s0 = sha_ror(a, 2) ^ sha_ror(a, 13) ^ sha_ror(a, 22);
        const uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = s0 + mj;

        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    sh->state[0] += a;
    sh->state[1] += b;
    sh->state[2] += c;
    sh->state[3] += d;
    sh->state[4] += e;
    sh->state[5] += f;
    sh->state[6] += g;
    sh->state[7] += h;
}

/* Hex SHA-256 of `len` bytes into `out`, which needs 65 bytes. */
static void sha256_hex(const void *data, size_t len, char *out) {
    static const char Hex[] = "0123456789abcdef";

    Sha256 sh;
    sh.state[0] = 0x6a09e667u;
    sh.state[1] = 0xbb67ae85u;
    sh.state[2] = 0x3c6ef372u;
    sh.state[3] = 0xa54ff53au;
    sh.state[4] = 0x510e527fu;
    sh.state[5] = 0x9b05688cu;
    sh.state[6] = 0x1f83d9abu;
    sh.state[7] = 0x5be0cd19u;
    sh.bits     = (uint64_t)len * 8u;
    sh.used     = 0;

    const uint8_t *p = (const uint8_t *)data;
    while (len >= 64) {
        sha256_block(&sh, p);
        p += 64;
        len -= 64;
    }
    memcpy(sh.buf, p, len);
    sh.used = len;

    sh.buf[sh.used++] = 0x80;
    if (sh.used > 56) {
        memset(sh.buf + sh.used, 0, 64 - sh.used);
        sha256_block(&sh, sh.buf);
        sh.used = 0;
    }
    memset(sh.buf + sh.used, 0, 56 - sh.used);
    for (int i = 0; i < 8; ++i)
        sh.buf[56 + i] = (uint8_t)(sh.bits >> (56 - i * 8));
    sha256_block(&sh, sh.buf);

    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 4; ++j) {
            const uint8_t byte     = (uint8_t)(sh.state[i] >> (24 - j * 8));
            out[i * 8 + j * 2]     = Hex[byte >> 4];
            out[i * 8 + j * 2 + 1] = Hex[byte & 15];
        }
    out[64] = '\0';
}

/* The net is embedded so the binary is self-contained. NNUE_EVALFILE is relative to the
 * directory make runs in. Mach-O has its own section name and symbol prefix. */
#ifdef NNUE_EVALFILE

#if defined(__APPLE__)
#define NNUE_RODATA    ".section __TEXT,__const\n"
#define NNUE_SYM(name) "_" name
#else
#define NNUE_RODATA    ".section .rodata\n"
#define NNUE_SYM(name) name
#endif

/* clang-format off */
__asm__(NNUE_RODATA
        /* 16 bytes past a 64-byte boundary puts the payload, behind the 112-byte header,
         * on a cache line; misaligned rows would split half the vector loads. */
        ".balign 64\n"
        ".space 16\n"
        ".globl " NNUE_SYM("nnueEmbeddedStart") "\n"
        NNUE_SYM("nnueEmbeddedStart") ":\n"
        ".incbin \"" NNUE_EVALFILE "\"\n"
        ".globl " NNUE_SYM("nnueEmbeddedEnd") "\n"
        NNUE_SYM("nnueEmbeddedEnd") ":\n"
        ".balign 4\n"
        ".text\n");
/* clang-format on */
extern const unsigned char nnueEmbeddedStart[];
extern const unsigned char nnueEmbeddedEnd[];
#endif

/*
 * Everything an evaluation reads, in two cache lines (reading through the header cost
 * ~3.7% nps). Derived from `hdr` by nnue_adopt(), so it is a cache, not a second source of
 * truth. out* is the final layer: the flat output, or L3 with a stack. l1/l2 weights are
 * pair-major in an AVX2 build; see nnue_interleave().
 */
typedef struct {
    const int16_t *ftWeight;
    const int16_t *ftBias;
    const int16_t *l1Weight;
    const int32_t *l1Bias;
    const int16_t *l2Weight;
    const int32_t *l2Bias;
    const int16_t *outWeight;
    const int32_t *outBias;
    const int16_t *uncWeight;
    const int32_t *uncBias;

    uint32_t hidden;
    uint32_t inputs; /* what L1 reads: 2 * hidden for SCReLU, hidden for pairwise */
    uint32_t buckets;
    uint32_t trunkWidth; /* what both output heads read */
    uint32_t l1Size;
    uint32_t l2Size;

    /* Bytes, to keep the block in two cache lines. */
    uint8_t l1Shift;
    uint8_t l2Shift;
    uint8_t actShift;    /* log2(qa) - why a stacked net's qa must be a power of two */
    uint8_t bucketShift; /* log2(32 / buckets), which nnue_validate() makes a power of two */
    uint8_t pairwise;    /* NNUE_ACT_PAIRWISE: see nnue_activate() */

    int32_t qa;
    int32_t qb;
    int32_t scale;
} NnueHot;

_Static_assert(sizeof(NnueHot) <= 128, "the hot block is meant to be two cache lines");

typedef struct {
    _Alignas(64) NnueHot hot;

    NnueHeader hdr;
    unsigned char *owned;
    unsigned char *stackOwned; /* the interleaved stack weights, when there are any */
    char hash[65];
    char source[512];
    bool loaded;
} Net;

static Net Loaded;

/* The `UncertaintyHead` option; it survives an `EvalFile` change. Off, the head is never
 * computed, so the option A/Bs a single net file. */
static bool UncertaintyWanted = true;

/* Bumped when a net is adopted and on `ucinewgame`; a thread drops its cached evaluations
 * at the next search when this has moved. */
static uint32_t EvalEpoch = 1;

/* King slots in a feature set, or 0 if this build does not know it. */
static uint32_t nnue_king_slots(uint32_t featureSet) {
    switch (featureSet) {
    case NNUE_FEATURES_HALFKA_32SQ: return 32;
    default: return 0;
    }
}

/* Width the output heads read: the activated accumulators, or the stack's last layer. */
static uint32_t nnue_trunk_width(const NnueHeader *h) {
    if (h->l1Size == 0)
        return 2u * h->hidden;
    return h->l2Size ? h->l2Size : h->l1Size;
}

/* SCReLU passes one input per unit of both accumulators, pairwise one per pair. */
static uint32_t nnue_l1_inputs(const NnueHeader *h) {
    return h->activation == NNUE_ACT_PAIRWISE ? h->hidden : 2u * h->hidden;
}

/* Payload bytes this header implies. The uncertainty flag adds a second output layer. All
 * widths are multiples of 16, so every section stays int32-aligned without padding. */
static uint64_t nnue_payload_bytes(const NnueHeader *h) {
    const uint64_t buckets = h->outputBuckets;
    const uint64_t headBytes =
        buckets * nnue_trunk_width(h) * sizeof(int16_t) + buckets * sizeof(int32_t);

    uint64_t stackBytes = 0;
    if (h->l1Size) {
        stackBytes = buckets * h->l1Size * nnue_l1_inputs(h) * sizeof(int16_t) +
                     buckets * h->l1Size * sizeof(int32_t);
        if (h->l2Size)
            stackBytes += buckets * h->l2Size * h->l1Size * sizeof(int16_t) +
                          buckets * h->l2Size * sizeof(int32_t);
    }

    return (uint64_t)h->features * h->hidden * sizeof(int16_t) +
           (uint64_t)h->hidden * sizeof(int16_t) + stackBytes +
           headBytes * (h->reserved[0] == 1 ? 2u : 1u);
}

/* Every rejection names the field and both values (invariant 8). */
static bool nnue_validate(const unsigned char *blob, size_t bytes, const char *what) {
    const NnueHeader *const h = (const NnueHeader *)(const void *)blob;

#define REJECT(...)                       \
    do {                                  \
        printf("info string %s: ", what); \
        printf(__VA_ARGS__);              \
        printf("\n");                     \
        fflush(stdout);                   \
        return false;                     \
    } while (0)

    if (bytes < sizeof(NnueHeader))
        REJECT("truncated - %zu bytes is smaller than the %zu-byte header", bytes,
               sizeof(NnueHeader));

    if (memcmp(h->magic, NNUE_MAGIC, NNUE_MAGIC_LEN) != 0)
        REJECT("not a net file (bad magic)");

    if (h->formatVersion != NNUE_FORMAT_VERSION)
        REJECT("format version %u, this build reads %u - re-export with the current "
               "tools/export_net.py",
               h->formatVersion, NNUE_FORMAT_VERSION);

    const uint32_t slots = nnue_king_slots(h->featureSet);
    if (slots == 0)
        REJECT("feature set %u is not implemented (this build runs %u, halfka-32sq) - add "
               "its king slot count to nnue_king_slots() and its case to nnue_perspective() "
               "in src/nnue.c",
               h->featureSet, (unsigned)NNUE_FEATURES_HALFKA_32SQ);

    if (h->activation != NNUE_ACT_SCRELU && h->activation != NNUE_ACT_PAIRWISE)
        REJECT("activation %u is not implemented (this build runs %u, screlu, and %u, "
               "pairwise) - add its case to nnue_activate() in src/nnue.c",
               h->activation, (unsigned)NNUE_ACT_SCRELU, (unsigned)NNUE_ACT_PAIRWISE);

    if (h->features != slots * 12u * 64u)
        REJECT("feature set %u is %u features, not %u", h->featureSet, slots * 12u * 64u,
               h->features);

    if (h->hidden == 0 || h->hidden > NNUE_MAX_HIDDEN)
        REJECT("hidden width %u, this build holds at most %u - raise NNUE_MAX_HIDDEN in "
               "src/nnue.h and rebuild",
               h->hidden, (unsigned)NNUE_MAX_HIDDEN);

    if (h->hidden % NNUE_WIDTH_MULTIPLE != 0)
        REJECT("hidden width %u is not a multiple of %u, which the vectorised accumulator "
               "requires - retrain at a width that is",
               h->hidden, (unsigned)NNUE_WIDTH_MULTIPLE);

    /* Indexed (pieceCount - 2) / (32 / buckets), which covers every bucket only when the
     * count divides 32. */
    if (h->outputBuckets == 0 || h->outputBuckets > NNUE_MAX_OUTPUT_BUCKETS ||
        32u % h->outputBuckets != 0)
        REJECT("%u output buckets - must be a divisor of 32, at most %u", h->outputBuckets,
               (unsigned)NNUE_MAX_OUTPUT_BUCKETS);

    /* The layer stack; l1Size 0 is a flat net. */
    if (h->l1Size > NNUE_MAX_STACK_WIDTH)
        REJECT("l1 width %u, this build holds at most %u - raise NNUE_MAX_STACK_WIDTH in "
               "src/nnue.h and rebuild",
               h->l1Size, (unsigned)NNUE_MAX_STACK_WIDTH);

    if (h->l2Size > NNUE_MAX_STACK_WIDTH)
        REJECT("l2 width %u, this build holds at most %u - raise NNUE_MAX_STACK_WIDTH in "
               "src/nnue.h and rebuild",
               h->l2Size, (unsigned)NNUE_MAX_STACK_WIDTH);

    if (h->l1Size % NNUE_WIDTH_MULTIPLE != 0 || h->l2Size % NNUE_WIDTH_MULTIPLE != 0)
        REJECT("stack widths %u/%u must be multiples of %u, which the vectorised dot "
               "product requires - retrain at widths that are",
               h->l1Size, h->l2Size, (unsigned)NNUE_WIDTH_MULTIPLE);

    /* Pairwise feeds L1, and the flat head fuses SCReLU into its own dot product. */
    if (h->activation == NNUE_ACT_PAIRWISE && h->l1Size == 0)
        REJECT("activation pairwise with no layer stack - it feeds L1, and there is none");

    if (h->activation == NNUE_ACT_PAIRWISE && h->hidden % (2u * NNUE_WIDTH_MULTIPLE) != 0)
        REJECT("pairwise hidden width %u is not a multiple of %u - each half is walked %u "
               "lanes at a time",
               h->hidden, 2u * NNUE_WIDTH_MULTIPLE, (unsigned)NNUE_WIDTH_MULTIPLE);

    if (h->l2Size && !h->l1Size)
        REJECT("l2 width %u with no l1 - L2 reads L1's output and there is none", h->l2Size);

    if ((h->l1Size == 0 && (h->l1Shift || h->l2Shift)) || (h->l2Size == 0 && h->l2Shift))
        REJECT("shifts %u/%u are set for stack widths %u/%u - one of the two is wrong", h->l1Shift,
               h->l2Shift, h->l1Size, h->l2Size);

    if (h->l1Size && (h->l1Shift == 0 || h->l1Shift > 30))
        REJECT("l1 shift %u is outside 1..30; it requantises an int32 sum", h->l1Shift);

    if (h->l2Size && (h->l2Shift == 0 || h->l2Shift > 30))
        REJECT("l2 shift %u is outside 1..30; it requantises an int32 sum", h->l2Shift);

    if (h->qa == 0 || h->qb == 0 || h->scale == 0)
        REJECT("degenerate quantisation (qa %u, qb %u, scale %d)", h->qa, h->qb, h->scale);

    /* The stack's activation is (x * x) >> log2(qa); a flat net divides by qa instead. */
    if (h->l1Size && (h->qa & (h->qa - 1u)) != 0)
        REJECT("qa %u is not a power of two, which a net with a layer stack requires - its "
               "activation shifts by log2(qa) rather than dividing",
               h->qa);

    /* The AVX2 clamp is an int16 broadcast; a larger qa would truncate only there. */
    if (h->qa > INT16_MAX)
        REJECT("qa %u exceeds the int16 clamp ceiling %d that the vectorised accumulator "
               "requires",
               h->qa, (int)INT16_MAX);

    if (h->scale < 0)
        REJECT("scale %d is negative, which would invert every evaluation", h->scale);

    /* reserved[0] is the uncertainty flag. The rest must be zero, so a field a newer
     * exporter sets is never silently ignored. */
    if (h->reserved[0] > 1)
        REJECT("uncertainty flag %u, this build reads 0 or 1 - re-export with the current "
               "tools/export_net.py",
               h->reserved[0]);
    for (int i = 1; i < 16; ++i)
        if (h->reserved[i] != 0)
            REJECT("reserved byte %d is %u; this build understands only the uncertainty "
                   "flag in byte 0 - it is too old for whatever wrote this net",
                   i, h->reserved[i]);

    /* Weight ranges are not re-checked: tools/export_net.py refuses to write a net whose
     * int16 sums could overflow. */
    const uint64_t need = nnue_payload_bytes(h);
    if (h->payloadBytes != need)
        REJECT("header claims %u payload bytes, its own shape needs %llu", h->payloadBytes,
               (unsigned long long)need);

    if ((uint64_t)bytes != sizeof(NnueHeader) + need)
        REJECT("file is %zu bytes, header describes %llu", bytes,
               (unsigned long long)(sizeof(NnueHeader) + need));

#undef REJECT
    return true;
}

#ifdef NNUE_AVX2

/* The set-bit positions of each 8-bit mask, to compact a movemask into pair indices without
 * a branch: all eight are stored and the count advances by popcount. Filled at load. */
static uint8_t NnzLut[256][8];

static void nnue_nnz_lut_init(void) {
    for (unsigned mask = 0; mask < 256; ++mask) {
        unsigned k = 0;
        for (unsigned bit = 0; bit < 8; ++bit)
            if (mask & (1u << bit))
                NnzLut[mask][k++] = (uint8_t)bit;
    }
}

/* Re-lays one stack layer [bucket][unit][input] as [bucket][pair][unit][2], the order the
 * AVX2 layers read: one broadcast input pair against every unit, so L1 can skip zero pairs.
 * Bucket offsets are unchanged. */
static void nnue_interleave(const int16_t *w, uint32_t buckets, uint32_t units, uint32_t inputs,
                            int16_t *dst) {
    for (uint32_t b = 0; b < buckets; ++b) {
        const int16_t *const src = w + (size_t)b * units * inputs;
        int16_t *const out       = dst + (size_t)b * units * inputs;

        for (uint32_t p = 0; p < inputs / 2u; ++p)
            for (uint32_t u = 0; u < units; ++u) {
                out[((size_t)p * units + u) * 2u]      = src[(size_t)u * inputs + 2u * p];
                out[((size_t)p * units + u) * 2u + 1u] = src[(size_t)u * inputs + 2u * p + 1u];
            }
    }
}
#endif

/* Makes a validated blob the current net. `owned` is NULL for the embedded net. Allocates
 * first, so on failure the previous net is left untouched. */
static bool nnue_adopt(const unsigned char *blob, size_t bytes, unsigned char *owned,
                       const char *source) {
    unsigned char *stackOwned = NULL;

#ifdef NNUE_AVX2
    {
        const NnueHeader *const nh = (const NnueHeader *)(const void *)blob;
        const size_t weights       = (size_t)nh->outputBuckets * nh->l1Size * nnue_l1_inputs(nh) +
                               (size_t)nh->outputBuckets * nh->l2Size * nh->l1Size;

        /* +64 to align the copy to a cache line. */
        if (nh->l1Size) {
            stackOwned = (unsigned char *)malloc(weights * sizeof(int16_t) + 64u);
            if (!stackOwned) {
                printf("info string %s: out of memory for the layer stack's interleaved weights "
                       "(%zu bytes)\n",
                       source, weights * sizeof(int16_t));
                fflush(stdout);
                return false;
            }
        }
        nnue_nnz_lut_init();
    }
#endif

    if (Loaded.owned)
        free(Loaded.owned);
    free(Loaded.stackOwned);

    memcpy(&Loaded.hdr, blob, sizeof(NnueHeader));
    Loaded.owned      = owned;
    Loaded.stackOwned = stackOwned;

    const NnueHeader *const h = &Loaded.hdr;
    const size_t buckets      = h->outputBuckets;
    NnueHot *const hot        = &Loaded.hot;

    memset(hot, 0, sizeof(*hot));
    hot->hidden     = h->hidden;
    hot->buckets    = h->outputBuckets;
    hot->trunkWidth = nnue_trunk_width(h);
    hot->l1Size     = h->l1Size;
    hot->l2Size     = h->l2Size;
    hot->l1Shift    = (uint8_t)h->l1Shift;
    hot->l2Shift    = (uint8_t)h->l2Shift;
    hot->qa         = (int32_t)h->qa;
    hot->qb         = (int32_t)h->qb;
    hot->scale      = h->scale;

    hot->inputs   = nnue_l1_inputs(h);
    hot->pairwise = h->activation == NNUE_ACT_PAIRWISE;

    for (uint32_t q = h->qa; q > 1u; q >>= 1)
        ++hot->actShift;

    for (uint32_t d = 32u / (uint32_t)buckets; d > 1u; d >>= 1)
        ++hot->bucketShift;

    const int16_t *p = (const int16_t *)(const void *)(blob + sizeof(NnueHeader));
    hot->ftWeight    = p;
    p += (size_t)h->features * h->hidden;
    hot->ftBias = p;
    p += h->hidden;

    if (h->l1Size) {
        hot->l1Weight = p;
        p += buckets * h->l1Size * hot->inputs;
        hot->l1Bias = (const int32_t *)(const void *)p;
        p           = (const int16_t *)(const void *)(hot->l1Bias + buckets * h->l1Size);

        if (h->l2Size) {
            hot->l2Weight = p;
            p += buckets * h->l2Size * h->l1Size;
            hot->l2Bias = (const int32_t *)(const void *)p;
            p           = (const int16_t *)(const void *)(hot->l2Bias + buckets * h->l2Size);
        }
    }

    hot->outWeight = p;
    p += buckets * hot->trunkWidth;
    hot->outBias = (const int32_t *)(const void *)p;

    if (h->reserved[0] == 1) {
        p              = (const int16_t *)(const void *)(hot->outBias + buckets);
        hot->uncWeight = p;
        p += buckets * hot->trunkWidth;
        hot->uncBias = (const int32_t *)(const void *)p;
    }

#ifdef NNUE_AVX2
    /* Every layer block is a multiple of 64 bytes, so aligning the first aligns them all. */
    if (stackOwned) {
        int16_t *const l1 =
            (int16_t *)(void *)(stackOwned + ((64u - ((uintptr_t)stackOwned & 63u)) & 63u));

        nnue_interleave(hot->l1Weight, hot->buckets, h->l1Size, hot->inputs, l1);
        hot->l1Weight = l1;

        if (h->l2Size) {
            int16_t *const l2 = l1 + (size_t)hot->buckets * h->l1Size * hot->inputs;

            nnue_interleave(hot->l2Weight, hot->buckets, h->l2Size, h->l1Size, l2);
            hot->l2Weight = l2;
        }
    }
#endif

    sha256_hex(blob, bytes, Loaded.hash);
    snprintf(Loaded.source, sizeof(Loaded.source), "%s", source);
    Loaded.loaded = true;

    /* Invalidates every thread's cached evaluations. */
    ++EvalEpoch;
    return true;
}

bool nnue_load_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("info string EvalFile: cannot open %s\n", path);
        fflush(stdout);
        return false;
    }

    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        printf("info string EvalFile: %s is empty\n", path);
        fflush(stdout);
        fclose(f);
        return false;
    }

    /* +64 so the payload, behind the header, can be placed on a cache line. */
    unsigned char *const owned = (unsigned char *)malloc((size_t)size + 64u);
    if (!owned) {
        printf("info string EvalFile: out of memory reading %s\n", path);
        fflush(stdout);
        fclose(f);
        return false;
    }

    unsigned char *const blob =
        owned + ((64u - (((uintptr_t)owned + sizeof(NnueHeader)) & 63u)) & 63u);

    const size_t got = fread(blob, 1, (size_t)size, f);
    fclose(f);

    if (got != (size_t)size || !nnue_validate(blob, got, path) ||
        !nnue_adopt(blob, got, owned, path)) {
        free(owned);
        return false;
    }
    return true;
}

void nnue_init(void) {
    if (Loaded.loaded)
        return;

#ifdef NNUE_EVALFILE
    const size_t bytes = (size_t)(nnueEmbeddedEnd - nnueEmbeddedStart);
    if (!nnue_validate(nnueEmbeddedStart, bytes, "embedded net")) {
        printf("info string the embedded net is unusable; rebuild with a valid EVALFILE\n");
        fflush(stdout);
        exit(1);
    }
    if (!nnue_adopt(nnueEmbeddedStart, bytes, NULL, NNUE_EVALFILE " (embedded)"))
        exit(1);
#else
    printf("info string this build embeds no net: rebuild with "
           "'make EVALFILE=<path>'\n");
    fflush(stdout);
    exit(1);
#endif
}

/* The mirrored king square, files 0-3: 32 slots, one per square. */
static inline int nnue_king_square(Square normalisedKing) {
    return (int)rank_of(normalisedKing) * 4 + (int)file_of(normalisedKing);
}

typedef struct {
    Color side;
    bool mirror;
    int slot;
} Perspective;

/* One side's view: rank-flipped for black, file-mirrored when that side's king is on the
 * kingside. Takes the king square so an update can ask whether a king move changes it. */
static Perspective nnue_perspective_of(Color side, Square king) {
    Perspective p;

    if (side == BLACK)
        king = flip_rank(king);

    p.side   = side;
    p.mirror = file_of(king) >= FILE_E;
    if (p.mirror)
        king = (Square)(king ^ 7);
    p.slot = nnue_king_square(king);
    return p;
}

static inline Perspective nnue_perspective(const Position *pos, Color side) {
    return nnue_perspective_of(side, king_square(pos, side));
}

/* The feature set: king slot x 12 planes x 64 squares. Planes 0-5 are the perspective's
 * own pieces and 6-11 the enemy's, PAWN..KING. A new feature set needs its own version. */
static inline int nnue_feature_index(const Perspective *p, Square sq, Piece pc) {
    Square s = (p->side == BLACK) ? flip_rank(sq) : sq;
    if (p->mirror)
        s = (Square)(s ^ 7);

    const int plane = (color_of(pc) == p->side ? 0 : 6) + (int)type_of(pc) - 1;
    return p->slot * (12 * 64) + plane * 64 + (int)s;
}

/* One perspective's accumulator, from scratch. */
static void nnue_accumulate(const Position *pos, const Perspective *p, int16_t *acc) {
    const uint32_t hidden = Loaded.hot.hidden;

    /* Rows are resolved before summing so the next one can be prefetched (~4% nps). 64, not
     * 32: board_set_fen accepts any diagram. */
    const int16_t *rows[64];
    int count = 0;

    Bitboard occupied = occupied_bb(pos);
    while (occupied) {
        const Square s    = pop_lsb(&occupied);
        const int feature = nnue_feature_index(p, s, piece_on(pos, s));
        rows[count++]     = Loaded.hot.ftWeight + (size_t)feature * hidden;
    }

    memcpy(acc, Loaded.hot.ftBias, hidden * sizeof(int16_t));

    for (int i = 0; i < count; ++i) {
        const int16_t *const row = rows[i];
        if (i + 1 < count)
            __builtin_prefetch(rows[i + 1]);

#ifdef NNUE_AVX2
        for (uint32_t j = 0; j < hidden; j += 16) {
            const __m256i a = _mm256_loadu_si256((const __m256i *)(const void *)(acc + j));
            const __m256i r = _mm256_loadu_si256((const __m256i *)(const void *)(row + j));
            _mm256_storeu_si256((__m256i *)(void *)(acc + j), _mm256_add_epi16(a, r));
        }
#else
        for (uint32_t j = 0; j < hidden; ++j)
            acc[j] = (int16_t)(acc[j] + row[j]);
#endif
    }
}

/* Must match output_bucket() in trainer/nnue/format.py: (pieces - 2) / (32 / buckets),
 * done as a shift since the divisor is a power of two. `n <= 0` matches C division for
 * fewer than two pieces; the clamp keeps a 33+-piece FEN in range. */
static inline int nnue_output_bucket(const Position *pos) {
    const int n      = popcount(occupied_bb(pos)) - 2;
    const int bucket = n <= 0 ? 0 : n >> Loaded.hot.bucketShift;

    return bucket < (int)Loaded.hot.buckets ? bucket : (int)Loaded.hot.buckets - 1;
}

#ifdef NNUE_AVX2

/* Eight int32 lanes into an int64; rare enough that the store round-trip is free. */
static inline int64_t nnue_hsum_epi32(__m256i v) {
    int32_t lanes[8];
    _mm256_storeu_si256((__m256i *)(void *)lanes, v);

    int64_t sum = 0;
    for (int i = 0; i < 8; ++i)
        sum += lanes[i];
    return sum;
}

/* For sums that fit int32. The exporter bounds every layer sum, and any lane's partial
 * sum, inside int32, so reduction order cannot change the result. */
static inline int32_t nnue_hsum32(__m256i v) {
    __m128i s = _mm_add_epi32(_mm256_castsi256_si128(v), _mm256_extracti128_si256(v, 1));

    s = _mm_add_epi32(s, _mm_shuffle_epi32(s, 0x4E));
    s = _mm_add_epi32(s, _mm_shuffle_epi32(s, 0xB1));
    return _mm_cvtsi128_si32(s);
}

/* SCReLU against half the output row. v * w fits int16 (the exporter ensures it), and
 * madd widens (v * w) * v to int32. */
static inline int64_t nnue_screlu_half(const int16_t *acc, const int16_t *w, uint32_t hidden,
                                       int32_t qa) {
    const __m256i zero = _mm256_setzero_si256();
    const __m256i top  = _mm256_set1_epi16((short)qa);

    int64_t total = 0;
    uint32_t j    = 0;

    /* Flushed to int64 per NNUE_SCRELU_FLUSH vectors in an outer loop, keeping the counter
     * out of the inner one; two accumulators for independent dependency chains. */
    while (j < hidden) {
        const uint32_t end =
            hidden - j < NNUE_SCRELU_FLUSH * 16u ? hidden : j + NNUE_SCRELU_FLUSH * 16u;

        __m256i even = zero;
        __m256i odd  = zero;

        for (; j + 32u <= end; j += 32u) {
            const __m256i a0 = _mm256_loadu_si256((const __m256i *)(const void *)(acc + j));
            const __m256i a1 = _mm256_loadu_si256((const __m256i *)(const void *)(acc + j + 16));
            const __m256i k0 = _mm256_loadu_si256((const __m256i *)(const void *)(w + j));
            const __m256i k1 = _mm256_loadu_si256((const __m256i *)(const void *)(w + j + 16));
            const __m256i v0 = _mm256_min_epi16(_mm256_max_epi16(a0, zero), top);
            const __m256i v1 = _mm256_min_epi16(_mm256_max_epi16(a1, zero), top);

            even = _mm256_add_epi32(even, _mm256_madd_epi16(_mm256_mullo_epi16(v0, k0), v0));
            odd  = _mm256_add_epi32(odd, _mm256_madd_epi16(_mm256_mullo_epi16(v1, k1), v1));
        }
        for (; j < end; j += 16u) {
            const __m256i a0 = _mm256_loadu_si256((const __m256i *)(const void *)(acc + j));
            const __m256i k0 = _mm256_loadu_si256((const __m256i *)(const void *)(w + j));
            const __m256i v0 = _mm256_min_epi16(_mm256_max_epi16(a0, zero), top);

            even = _mm256_add_epi32(even, _mm256_madd_epi16(_mm256_mullo_epi16(v0, k0), v0));
        }
        total += nnue_hsum_epi32(_mm256_add_epi32(even, odd));
    }
    return total;
}
#endif

/* The flat output layer, own accumulator first. The squared activation carries qa^2 and the
 * bias qa, so the sum is divided by qa (truncating, like the reference) before the bias is
 * added. */
static int32_t nnue_flat_head(const int16_t *own, const int16_t *other, const int16_t *weights,
                              const int32_t *biases, int bucket) {
    const uint32_t hidden  = Loaded.hot.hidden;
    const int32_t qa       = Loaded.hot.qa;
    const int16_t *const w = weights + (size_t)bucket * 2u * hidden;
    const int32_t bias     = biases[bucket];

#ifdef NNUE_AVX2
    const int64_t sum =
        nnue_screlu_half(own, w, hidden, qa) + nnue_screlu_half(other, w + hidden, hidden, qa);
#else

    int64_t sum = 0;

    for (uint32_t j = 0; j < hidden; ++j) {
        const int32_t x = own[j] < 0 ? 0 : (own[j] > qa ? qa : own[j]);
        sum += (int64_t)(x * x) * (int64_t)w[j];
    }
    for (uint32_t j = 0; j < hidden; ++j) {
        const int32_t x = other[j] < 0 ? 0 : (other[j] > qa ? qa : other[j]);
        sum += (int64_t)(x * x) * (int64_t)w[hidden + j];
    }
#endif

    return (int32_t)(sum / qa) + bias;
}

/* ---------------------------------------------------------------- the layer stack --
 * Only when the net has one (l1Size > 0); see Task 6 in docs/NNUE.md. */

/* int16 x int16 into int32; the exporter guarantees no lane can overflow. */
static inline int32_t nnue_dot(const int16_t *in, const int16_t *w, uint32_t n) {
#ifdef NNUE_AVX2
    __m256i lanes = _mm256_setzero_si256();

    for (uint32_t j = 0; j < n; j += 16) {
        const __m256i v = _mm256_loadu_si256((const __m256i *)(const void *)(in + j));
        const __m256i k = _mm256_loadu_si256((const __m256i *)(const void *)(w + j));
        lanes           = _mm256_add_epi32(lanes, _mm256_madd_epi16(v, k));
    }
    return nnue_hsum32(lanes);
#else
    int32_t sum = 0;

    for (uint32_t j = 0; j < n; ++j)
        sum += (int32_t)in[j] * (int32_t)w[j];
    return sum;
#endif
}

/* Activation for L1: clamp to [0, qa], multiply, shift down by log2(qa). SCReLU multiplies
 * a unit by itself, pairwise unit j by unit j + hidden / 2. Shifting per element keeps the
 * vector int16, and is why a stacked net's qa must be a power of two. */
#ifdef NNUE_AVX2

/* The activation, and the indices of its nonzero int16 pairs for sparse L1. (Compacting
 * the values instead measured slower in a real search.) No store passes the end: each
 * row of eight lands at the running count, never past that row's own first pair. */
typedef struct {
    _Alignas(64) int16_t act[2 * NNUE_MAX_HIDDEN];
    uint16_t idx[NNUE_MAX_HIDDEN];
} NnzList;

/* `firstPair` is this half's offset in the whole vector; returns the list's new length.
 * Always inlined so SCReLU loads each vector once, and a NULL `idxOut` compiles out. */
static inline __attribute__((always_inline)) uint32_t
nnue_activate_half(const int16_t *lhs, const int16_t *rhs, int16_t *out, uint32_t n, int32_t qa,
                   uint32_t shift, uint16_t *idxOut, uint32_t count, uint32_t firstPair) {
    const __m256i zero  = _mm256_setzero_si256();
    const __m256i top   = _mm256_set1_epi16((short)qa);
    const __m128i k1    = _mm_cvtsi32_si128((int)((17u - shift) / 2u));
    const __m128i k2    = _mm_cvtsi32_si128((int)((16u - shift) / 2u));
    const __m128i eight = _mm_set1_epi16(8);
    __m128i base        = _mm_set1_epi16((short)firstPair);

    for (uint32_t j = 0; j < n; j += 16) {
        const __m256i a = _mm256_loadu_si256((const __m256i *)(const void *)(lhs + j));
        const __m256i b = _mm256_loadu_si256((const __m256i *)(const void *)(rhs + j));
        const __m256i u = _mm256_min_epi16(_mm256_max_epi16(a, zero), top);
        const __m256i v = _mm256_min_epi16(_mm256_max_epi16(b, zero), top);

        /* u * v >> shift as one mulhi: (u << k1) * (v << k2) >> 16 with k1 + k2 = 16 - shift.
         * Exact, and neither operand overflows since u, v <= qa <= 2^14. */
        const __m256i x = _mm256_mulhi_epu16(_mm256_sll_epi16(u, k1), _mm256_sll_epi16(v, k2));

        _mm256_storeu_si256((__m256i *)(void *)(out + j), x);

        if (!idxOut)
            continue;

        /* Both lanes of a pair are >= 0, so as an int32 it is positive iff either is set. */
        const unsigned mask =
            (unsigned)_mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpgt_epi32(x, zero)));
        const __m128i idx =
            _mm_cvtepu8_epi16(_mm_loadl_epi64((const __m128i *)(const void *)NnzLut[mask]));

        _mm_storeu_si128((__m128i *)(void *)(idxOut + count), _mm_add_epi16(base, idx));
        count += (uint32_t)__builtin_popcount(mask);
        base = _mm_add_epi16(base, eight);
    }
    return count;
}

/* L1's whole input, own perspective first. For SCReLU also lists the nonzero pairs and
 * returns their count. Pairwise gets no list: 71% of its pairs are nonzero, too dense for
 * sparse L1 to pay. */
static inline uint32_t nnue_activate(const NnueHot *hot, const int16_t *own, const int16_t *other,
                                     NnzList *list) {
    const uint32_t h = hot->hidden;

    if (hot->pairwise) {
        const uint32_t q = h / 2u;

        nnue_activate_half(own, own + q, list->act, q, hot->qa, hot->actShift, NULL, 0, 0);
        nnue_activate_half(other, other + q, list->act + q, q, hot->qa, hot->actShift, NULL, 0, 0);
        return h / 2u;
    }

    const uint32_t count =
        nnue_activate_half(own, own, list->act, h, hot->qa, hot->actShift, list->idx, 0, 0);

    return nnue_activate_half(other, other, list->act + h, h, hot->qa, hot->actShift, list->idx,
                              count, h / 2u);
}
#else
static inline void nnue_activate_half(const int16_t *lhs, const int16_t *rhs, int16_t *out,
                                      uint32_t n, int32_t qa, uint32_t shift) {
    for (uint32_t j = 0; j < n; ++j) {
        const int32_t x = lhs[j] < 0 ? 0 : (lhs[j] > qa ? qa : lhs[j]);
        const int32_t y = rhs[j] < 0 ? 0 : (rhs[j] > qa ? qa : rhs[j]);

        out[j] = (int16_t)((x * y) >> shift);
    }
}

static inline void nnue_activate(const NnueHot *hot, const int16_t *own, const int16_t *other,
                                 int16_t *out) {
    const uint32_t h = hot->hidden;

    if (hot->pairwise) {
        const uint32_t q = h / 2u;

        nnue_activate_half(own, own + q, out, q, hot->qa, hot->actShift);
        nnue_activate_half(other, other + q, out + q, q, hot->qa, hot->actShift);
        return;
    }

    nnue_activate_half(own, own, out, h, hot->qa, hot->actShift);
    nnue_activate_half(other, other, out + h, h, hot->qa, hot->actShift);
}
#endif

/* A layer's int32 sum back into [0, ceiling], rounded to nearest (truncation would bias
 * every layer downward). Clamped before the shift: `>>` on a negative int32 is
 * implementation-defined, and numpy must agree exactly. */
static inline int16_t nnue_requantise(int32_t sum, uint32_t shift, int32_t ceiling) {
    const int32_t half = (int32_t)1 << (shift - 1);
    const int32_t r    = ((sum < 0 ? 0 : sum) + half) >> shift;

    return (int16_t)(r > ceiling ? ceiling : r);
}

#ifdef NNUE_AVX2

/*
 * The AVX2 layers walk the inputs a pair at a time (madd consumes pairs) and broadcast
 * each pair against every unit's weights, one int32 lane per unit, using the pair-major
 * weights from nnue_interleave(). That order lets sparse L1 skip all-zero pairs: about
 * half of SCReLU's pairs are zero. Results are exact because the exporter bounds every
 * partial sum inside int32, and widths are multiples of 16 so no tail is needed.
 */

/* One input pair broadcast for madd. memcpy avoids an aliasing violation. */
static inline __m256i nnue_pair(const int16_t *in, size_t pair) {
    int32_t v;
    memcpy(&v, in + 2u * pair, sizeof(v));
    return _mm256_set1_epi32(v);
}

/* nnue_requantise() for sixteen units, packed back to int16. */
static inline void nnue_requantise16(__m256i lo, __m256i hi, uint32_t shift, int32_t ceiling,
                                     int16_t *out) {
    const __m256i zero = _mm256_setzero_si256();
    const __m256i half = _mm256_set1_epi32((int32_t)1 << (shift - 1));
    const __m256i cap  = _mm256_set1_epi32(ceiling);
    const __m128i down = _mm_cvtsi32_si128((int)shift);

    lo = _mm256_min_epi32(
        _mm256_srl_epi32(_mm256_add_epi32(_mm256_max_epi32(lo, zero), half), down), cap);
    hi = _mm256_min_epi32(
        _mm256_srl_epi32(_mm256_add_epi32(_mm256_max_epi32(hi, zero), half), down), cap);

    /* packs works within each 128-bit lane, leaving units 0-3, 8-11, 4-7, 12-15; the
     * permute puts the middle two quarters back in order. */
    _mm256_storeu_si256((__m256i *)(void *)out,
                        _mm256_permute4x64_epi64(_mm256_packs_epi32(lo, hi), 0xD8));
}

static inline __m256i nnue_load_at(const char *p) {
    return _mm256_loadu_si256((const __m256i *)(const void *)p);
}

/* L1 over the listed pairs only, two per step to halve the loop-carried chain. */
static inline __attribute__((always_inline)) void
nnue_l1_sparse_units(const NnzList *list, uint32_t count, const int16_t *w, const int32_t *bias,
                     uint32_t units, uint32_t shift, int32_t ceiling, int16_t *out) {
    const size_t stride = (size_t)units * 4u; /* bytes per pair: two int16 per unit */

    for (uint32_t g = 0; g < units; g += 16) {
        const char *const wg = (const char *)(const void *)w + 4u * g;

        __m256i s0 = _mm256_loadu_si256((const __m256i *)(const void *)(bias + g));
        __m256i s1 = _mm256_loadu_si256((const __m256i *)(const void *)(bias + g + 8));
        uint32_t i = 0;

        for (; i + 2u <= count; i += 2u) {
            const char *const p0 = wg + list->idx[i] * stride;
            const char *const p1 = wg + list->idx[i + 1] * stride;
            const __m256i x0     = nnue_pair(list->act, list->idx[i]);
            const __m256i x1     = nnue_pair(list->act, list->idx[i + 1]);

            s0 = _mm256_add_epi32(s0, _mm256_add_epi32(_mm256_madd_epi16(x0, nnue_load_at(p0)),
                                                       _mm256_madd_epi16(x1, nnue_load_at(p1))));
            s1 = _mm256_add_epi32(s1,
                                  _mm256_add_epi32(_mm256_madd_epi16(x0, nnue_load_at(p0 + 32)),
                                                   _mm256_madd_epi16(x1, nnue_load_at(p1 + 32))));
        }
        if (i < count) {
            const char *const p0 = wg + list->idx[i] * stride;
            const __m256i x0     = nnue_pair(list->act, list->idx[i]);

            s0 = _mm256_add_epi32(s0, _mm256_madd_epi16(x0, nnue_load_at(p0)));
            s1 = _mm256_add_epi32(s1, _mm256_madd_epi16(x0, nnue_load_at(p0 + 32)));
        }

        nnue_requantise16(s0, s1, shift, ceiling, out + g);
    }
}

/* A constant 16 units (every net so far) turns the per-pair address into a shift. */
static void nnue_l1_sparse(const NnzList *list, uint32_t count, const int16_t *w,
                           const int32_t *bias, uint32_t units, uint32_t shift, int32_t ceiling,
                           int16_t *out) {
    if (units == 16)
        nnue_l1_sparse_units(list, count, w, bias, 16, shift, ceiling, out);
    else
        nnue_l1_sparse_units(list, count, w, bias, units, shift, ceiling, out);
}

/* A dense layer in pair-major order: L2, whose input is too dense (77% nonzero pairs) to
 * be worth listing. The tricks in nnue_l1_dense_units() measured slower here. */
static void nnue_layer(const int16_t *in, uint32_t n, const int16_t *w, const int32_t *bias,
                       uint32_t units, uint32_t shift, int32_t ceiling, int16_t *out) {
    const size_t stride = (size_t)units * 2u;

    for (uint32_t g = 0; g < units; g += 16) {
        const int16_t *const wg = w + 2u * g;

        __m256i s0 = _mm256_loadu_si256((const __m256i *)(const void *)(bias + g));
        __m256i s1 = _mm256_loadu_si256((const __m256i *)(const void *)(bias + g + 8));

        for (uint32_t p = 0; p < n / 2u; ++p) {
            const int16_t *const wp = wg + p * stride;
            const __m256i x         = nnue_pair(in, p);

            s0 = _mm256_add_epi32(
                s0, _mm256_madd_epi16(x, _mm256_loadu_si256((const __m256i *)(const void *)wp)));
            s1 = _mm256_add_epi32(
                s1,
                _mm256_madd_epi16(x, _mm256_loadu_si256((const __m256i *)(const void *)(wp + 16))));
        }

        nnue_requantise16(s0, s1, shift, ceiling, out + g);
    }
}

static inline __m256i nnue_madd_at(__m256i x, const int16_t *w) {
    return _mm256_madd_epi16(x, _mm256_loadu_si256((const __m256i *)(const void *)w));
}

/* Dense L1 for a pairwise net. Load-bound, so four pairs' inputs come from one load and are
 * split by shuffles (90 -> 83 ns), and the four products are summed as a tree. The pair
 * count is a multiple of four since a pairwise hidden width is a multiple of 32. */
static inline __attribute__((always_inline)) void
nnue_l1_dense_units(const int16_t *in, uint32_t n, const int16_t *w, const int32_t *bias,
                    uint32_t units, uint32_t shift, int32_t ceiling, int16_t *out) {
    const size_t stride = (size_t)units * 2u;

    for (uint32_t g = 0; g < units; g += 16) {
        const int16_t *wp = w + 2u * g;

        __m256i s0 = _mm256_loadu_si256((const __m256i *)(const void *)(bias + g));
        __m256i s1 = _mm256_loadu_si256((const __m256i *)(const void *)(bias + g + 8));

        for (uint32_t p = 0; p < n / 2u; p += 4u, wp += 4u * stride) {
            const __m256i v = _mm256_broadcastsi128_si256(
                _mm_loadu_si128((const __m128i *)(const void *)(in + 2u * p)));
            const __m256i x0 = _mm256_shuffle_epi32(v, 0x00);
            const __m256i x1 = _mm256_shuffle_epi32(v, 0x55);
            const __m256i x2 = _mm256_shuffle_epi32(v, 0xAA);
            const __m256i x3 = _mm256_shuffle_epi32(v, 0xFF);

            const __m256i lo = _mm256_add_epi32(
                _mm256_add_epi32(nnue_madd_at(x0, wp), nnue_madd_at(x1, wp + stride)),
                _mm256_add_epi32(nnue_madd_at(x2, wp + 2u * stride),
                                 nnue_madd_at(x3, wp + 3u * stride)));
            const __m256i hi = _mm256_add_epi32(
                _mm256_add_epi32(nnue_madd_at(x0, wp + 16), nnue_madd_at(x1, wp + stride + 16)),
                _mm256_add_epi32(nnue_madd_at(x2, wp + 2u * stride + 16),
                                 nnue_madd_at(x3, wp + 3u * stride + 16)));

            s0 = _mm256_add_epi32(s0, lo);
            s1 = _mm256_add_epi32(s1, hi);
        }

        nnue_requantise16(s0, s1, shift, ceiling, out + g);
    }
}

static void nnue_l1_dense(const int16_t *in, uint32_t n, const int16_t *w, const int32_t *bias,
                          uint32_t units, uint32_t shift, int32_t ceiling, int16_t *out) {
    if (units == 16)
        nnue_l1_dense_units(in, n, w, bias, 16, shift, ceiling, out);
    else
        nnue_l1_dense_units(in, n, w, bias, units, shift, ceiling, out);
}
#else

/* The scalar reference the AVX2 layers must match. */
static void nnue_layer(const int16_t *in, uint32_t n, const int16_t *w, const int32_t *bias,
                       uint32_t units, uint32_t shift, int32_t ceiling, int16_t *out) {
    for (uint32_t u = 0; u < units; ++u)
        out[u] = nnue_requantise(nnue_dot(in, w + (size_t)u * n, n) + bias[u], shift, ceiling);
}
#endif

/* Passed in rather than a local: a 12 KB frame makes Windows probe the stack every call. */
#ifdef NNUE_AVX2
typedef NnzList TrunkScratch;
#else
typedef struct {
    _Alignas(64) int16_t act[2 * NNUE_MAX_HIDDEN];
} TrunkScratch;
#endif

/* The vector both output heads read. */
static void nnue_stack_trunk(const int16_t *own, const int16_t *other, int bucket, int16_t *trunk,
                             TrunkScratch *scratch) {
    const NnueHot *const hot = &Loaded.hot;
    const uint32_t inputs    = hot->inputs;
    const int32_t qa         = hot->qa;
    const size_t b           = (size_t)bucket;

    _Alignas(64) int16_t first[NNUE_MAX_STACK_WIDTH];

    /* Without L2, L1 is the trunk. */
    int16_t *const l1Out     = hot->l2Size ? first : trunk;
    const int32_t *const l1b = hot->l1Bias + b * hot->l1Size;

#ifdef NNUE_AVX2
    const uint32_t count     = nnue_activate(hot, own, other, scratch);
    const int16_t *const l1w = hot->l1Weight + b * hot->l1Size * inputs;

    if (hot->pairwise)
        nnue_l1_dense(scratch->act, inputs, l1w, l1b, hot->l1Size, hot->l1Shift, qa, l1Out);
    else
        nnue_l1_sparse(scratch, count, l1w, l1b, hot->l1Size, hot->l1Shift, qa, l1Out);
#else
    const int16_t *const l1w = hot->l1Weight + b * hot->l1Size * inputs;

    nnue_activate(hot, own, other, scratch->act);
    nnue_layer(scratch->act, inputs, l1w, l1b, hot->l1Size, hot->l1Shift, qa, l1Out);
#endif

    if (!hot->l2Size)
        return;

    nnue_layer(first, hot->l1Size, hot->l2Weight + b * hot->l2Size * hot->l1Size,
               hot->l2Bias + b * hot->l2Size, hot->l2Size, hot->l2Shift, qa, trunk);
}

static inline int32_t nnue_trunk_head(const int16_t *trunk, const int16_t *weights,
                                      const int32_t *biases, int bucket) {
    const uint32_t width = Loaded.hot.trunkWidth;

    return nnue_dot(trunk, weights + (size_t)bucket * width, width) + biases[bucket];
}

/* One head from scratch, caching nothing: the verifier and the debug check use this. */
static int32_t nnue_head(const int16_t *own, const int16_t *other, const int16_t *weights,
                         const int32_t *biases, int bucket) {
    if (!Loaded.hot.l1Size)
        return nnue_flat_head(own, other, weights, biases, bucket);

    _Alignas(64) int16_t trunk[NNUE_MAX_STACK_WIDTH];
    TrunkScratch scratch;

    nnue_stack_trunk(own, other, bucket, trunk, &scratch);
    return nnue_trunk_head(trunk, weights, biases, bucket);
}

static inline int32_t nnue_output(const int16_t *own, const int16_t *other, int bucket) {
    return nnue_head(own, other, Loaded.hot.outWeight, Loaded.hot.outBias, bucket);
}

/* Truncates toward zero, which the exporter reproduces explicitly (numpy floors). */
static Value nnue_centipawns(int32_t raw) {
    const int64_t num = (int64_t)raw * (int64_t)Loaded.hot.scale;
    const int64_t den = (int64_t)Loaded.hot.qa * (int64_t)Loaded.hot.qb;
    const int64_t cp  = num / den;

    if (cp > NNUE_EVAL_LIMIT)
        return NNUE_EVAL_LIMIT;
    if (cp < -NNUE_EVAL_LIMIT)
        return -NNUE_EVAL_LIMIT;
    return (Value)cp;
}

/* From scratch; shared by nnue_evaluate() and the verifier. */
static int32_t nnue_raw(const Position *pos) {
    _Alignas(64) int16_t acc[COLOR_NB][NNUE_MAX_HIDDEN];

    for (Color c = WHITE; c <= BLACK; ++c) {
        const Perspective p = nnue_perspective(pos, c);
        nnue_accumulate(pos, &p, acc[c]);
    }

    const Color stm = pos->sideToMove;
    return nnue_output(acc[stm], acc[stm ^ 1], nnue_output_bucket(pos));
}

Value nnue_evaluate(const Position *pos) { return nnue_centipawns(nnue_raw(pos)); }

/*
 * The accumulator stack: a move changes at most four features per perspective, so it is
 * carried across make/unmake instead of re-summing 32 rows. Each level records its
 * position's key and rebuilds when the key does not match, so a missed push costs time,
 * never a wrong score; debug builds check every incremental value against a full one.
 */
typedef struct {
    _Alignas(64) int16_t acc[COLOR_NB][NNUE_MAX_HIDDEN];

    Key key; /* 0 until the level describes a position */

    /* Per perspective: a king move can reindex its own side without touching the other. */
    bool computed[COLOR_NB];
} Accumulator;

/*
 * Both heads' outputs for one position (`unc` is 0 without the head). They share the
 * trunk, so the first head asked computes both. Kept per accumulator level, and in a
 * per-thread direct-mapped table for positions evaluated before (a quarter of trunk builds
 * at bench 13). Validated by the full key; `gen` lets a new search invalidate both without
 * a memset. A parallel stack rather than an Accumulator field: widening the levels cost
 * ~2% nps even on flat nets.
 */
typedef struct {
    Key key;
    int16_t value;
    int16_t unc;
    uint16_t gen;
} StackOutputs;

/* 4 MB per thread; pays only together with the prefetch in eval_state_push(). */
#define OUTPUT_CACHE_ENTRIES (1u << 18)

/* Deeper than any push: the search returns at ply >= MAX_PLY - 1 before moving. */
#define ACC_LEVELS (MAX_PLY + 2)

/*
 * The refresh cache. A king move that reindexes its side forces a rebuild, ~10x the cost
 * of a delta. Instead the rebuild starts from the last accumulator seen with the king on
 * that square and applies the diff of the stored piece bitboards against the board. A stale
 * entry just means a bigger diff; only a net swap invalidates it (eval_state_clear()).
 */
typedef struct {
    Bitboard pieces[COLOR_NB][PIECE_TYPE_NB];
    bool valid;
} RefreshEntry;

/* One per king square per side: exactly one per distinct perspective. */
#define REFRESH_SLOTS (COLOR_NB * SQUARE_NB)

/* Beyond this many rows a full rebuild (32 rows) is cheaper than the diff. */
#define REFRESH_MAX_ROWS 24

/*
 * A thread's evaluation state, in one object. On this toolchain every thread-local access
 * is a call to __emutls_get_address(), so there is one thread-local and the search passes
 * the pointer down (`nt`) instead of re-reading it.
 */
struct EvalState {
    Accumulator *accStack;
    int accTop;

    /* `outGen` starts at 1, so a calloc'd record is never current. */
    StackOutputs *outLevels;
    StackOutputs *outCache;
    uint16_t outGen;

    /* Aligned by hand (no aligned_alloc here); `trunkBlock` is what free() is owed. */
    TrunkScratch *trunkScratch;
    void *trunkBlock;

    /* The EvalEpoch the caches were filled under; 0 matches none. */
    uint32_t epoch;

    /* Sized by the loaded net; `refreshWidth` notices a swap to another width. */
    RefreshEntry *refreshCache;
    int16_t *refreshAcc;
    uint32_t refreshWidth;
    bool refreshFailed;

    /* Set after a failed allocation so it is not retried every evaluation. */
    bool accFailed;
};

static _Thread_local EvalState NnueTls;

EvalState *eval_state(void) {
    EvalState *const nt = &NnueTls;

    if (nt->accStack)
        return nt;
    if (nt->accFailed)
        return NULL;

    nt->accStack = (Accumulator *)calloc(ACC_LEVELS, sizeof(Accumulator));
    if (!nt->accStack) {
        nt->accFailed = true;
        return NULL;
    }

    nt->accTop = 0;
    return nt;
}

size_t eval_state_bytes(void) {
    const size_t outputs =
        Loaded.hot.l1Size ? (ACC_LEVELS + OUTPUT_CACHE_ENTRIES) * sizeof(StackOutputs) : 0;

    return ACC_LEVELS * sizeof(Accumulator) + outputs +
           REFRESH_SLOTS * (sizeof(RefreshEntry) + Loaded.hot.hidden * sizeof(int16_t));
}

void eval_state_free(void) {
    EvalState *const nt = &NnueTls;

    free(nt->accStack);
    free(nt->outLevels);
    free(nt->outCache);
    free(nt->trunkBlock);
    free(nt->refreshCache);
    free(nt->refreshAcc);
    nt->accStack      = NULL;
    nt->outLevels     = NULL;
    nt->outCache      = NULL;
    nt->trunkScratch  = NULL;
    nt->trunkBlock    = NULL;
    nt->outGen        = 0;
    nt->epoch         = 0;
    nt->refreshCache  = NULL;
    nt->refreshAcc    = NULL;
    nt->refreshWidth  = 0;
    nt->accFailed     = false;
    nt->refreshFailed = false;
    nt->accTop        = 0;
}

typedef struct {
    Piece pc;
    Square sq;
} NnueFeature;

/* A move's feature changes, and which perspectives need a refresh instead. */
typedef struct {
    NnueFeature added[2];
    NnueFeature removed[2];
    int addedCount;
    int removedCount;
    bool refresh[COLOR_NB];
} NnueDelta;

/* Whether a king move changes its side's slot or mirror (d1-e1 changes only the mirror). */
static inline bool nnue_king_reindexes(Color side, Square kingFrom, Square kingTo) {
    const Perspective before = nnue_perspective_of(side, kingFrom);
    const Perspective after  = nnue_perspective_of(side, kingTo);

    return before.slot != after.slot || before.mirror != after.mirror;
}

/* The features `m` changed, read from the position after it was played. */
static void nnue_delta(const Position *pos, Move m, NnueDelta *d) {
    const Color us    = (Color)(pos->sideToMove ^ 1);
    const Square from = from_sq(m);
    const Square to   = to_sq(m);
    const MoveType mt = type_of_move(m);

    d->addedCount = d->removedCount = 0;
    d->refresh[WHITE] = d->refresh[BLACK] = false;

    if (mt == MT_CASTLING) {
        Square kingTo, rookTo;
        castling_targets(from, to, &kingTo, &rookTo);

        const Piece king = make_piece(us, KING);
        const Piece rook = make_piece(us, ROOK);

        d->removed[d->removedCount++] = (NnueFeature){king, from};
        d->removed[d->removedCount++] = (NnueFeature){rook, to};
        d->added[d->addedCount++]     = (NnueFeature){king, kingTo};
        d->added[d->addedCount++]     = (NnueFeature){rook, rookTo};

        d->refresh[us] = nnue_king_reindexes(us, from, kingTo);
        return;
    }

    /* do_move incremented gamePly, so the record it wrote is one below. */
    const Piece captured = pos->history[pos->gamePly - 1].captured;
    if (captured != NO_PIECE) {
        const Square capsq            = mt == MT_EN_PASSANT ? (Square)(to - pawn_push(us)) : to;
        d->removed[d->removedCount++] = (NnueFeature){captured, capsq};
    }

    const Piece vacated  = mt == MT_PROMOTION ? make_piece(us, PAWN) : piece_on(pos, to);
    const Piece occupied = piece_on(pos, to);

    d->removed[d->removedCount++] = (NnueFeature){vacated, from};
    d->added[d->addedCount++]     = (NnueFeature){occupied, to};

    if (type_of(vacated) == KING)
        d->refresh[us] = nnue_king_reindexes(us, from, to);
}

/* dst = src + added rows - removed rows, in one pass. */
static void nnue_apply_delta(const int16_t *src, int16_t *dst, const int16_t *const *add,
                             int addCount, const int16_t *const *sub, int subCount,
                             uint32_t hidden) {
    /* Quiet moves and captures, nearly every update, get loop-free bodies. */
    if (addCount == 1 && subCount == 1) {
        const int16_t *const a = add[0];
        const int16_t *const b = sub[0];

#ifdef NNUE_AVX2
        for (uint32_t j = 0; j < hidden; j += 16) {
            const __m256i v = _mm256_loadu_si256((const __m256i *)(const void *)(src + j));
            const __m256i x = _mm256_loadu_si256((const __m256i *)(const void *)(a + j));
            const __m256i y = _mm256_loadu_si256((const __m256i *)(const void *)(b + j));
            _mm256_storeu_si256((__m256i *)(void *)(dst + j),
                                _mm256_sub_epi16(_mm256_add_epi16(v, x), y));
        }
#else
        for (uint32_t j = 0; j < hidden; ++j)
            dst[j] = (int16_t)(src[j] + a[j] - b[j]);
#endif
        return;
    }

    if (addCount == 1 && subCount == 2) {
        const int16_t *const a = add[0];
        const int16_t *const b = sub[0];
        const int16_t *const c = sub[1];

#ifdef NNUE_AVX2
        for (uint32_t j = 0; j < hidden; j += 16) {
            const __m256i v = _mm256_loadu_si256((const __m256i *)(const void *)(src + j));
            const __m256i x = _mm256_loadu_si256((const __m256i *)(const void *)(a + j));
            const __m256i y = _mm256_loadu_si256((const __m256i *)(const void *)(b + j));
            const __m256i z = _mm256_loadu_si256((const __m256i *)(const void *)(c + j));
            _mm256_storeu_si256((__m256i *)(void *)(dst + j),
                                _mm256_sub_epi16(_mm256_sub_epi16(_mm256_add_epi16(v, x), y), z));
        }
#else
        for (uint32_t j = 0; j < hidden; ++j)
            dst[j] = (int16_t)(src[j] + a[j] - b[j] - c[j]);
#endif
        return;
    }

#ifdef NNUE_AVX2
    for (uint32_t j = 0; j < hidden; j += 16) {
        __m256i v = _mm256_loadu_si256((const __m256i *)(const void *)(src + j));

        for (int i = 0; i < addCount; ++i)
            v = _mm256_add_epi16(v,
                                 _mm256_loadu_si256((const __m256i *)(const void *)(add[i] + j)));
        for (int i = 0; i < subCount; ++i)
            v = _mm256_sub_epi16(v,
                                 _mm256_loadu_si256((const __m256i *)(const void *)(sub[i] + j)));

        _mm256_storeu_si256((__m256i *)(void *)(dst + j), v);
    }
#else
    for (uint32_t j = 0; j < hidden; ++j) {
        int32_t v = src[j];

        for (int i = 0; i < addCount; ++i)
            v += add[i][j];
        for (int i = 0; i < subCount; ++i)
            v -= sub[i][j];

        dst[j] = (int16_t)v;
    }
#endif
}

/* Refresh rows are cold; four lines are enough for the hardware prefetcher to take over. */
static inline void nnue_prefetch_row(const int16_t *row) {
#ifdef NNUE_AVX2
    for (int i = 0; i < 4; ++i)
        _mm_prefetch((const char *)(row + i * 32), _MM_HINT_T0);
#else
    (void)row;
#endif
}

/* Applies a refresh diff to the cache entry and copies it to `dst` in one pass. Not shared
 * with nnue_apply_delta(): a second caller stopped GCC inlining the per-node update. */
static void nnue_refresh_rows(int16_t *acc, int16_t *dst, const int16_t *const *add, int addCount,
                              const int16_t *const *sub, int subCount, uint32_t hidden) {
#ifdef NNUE_AVX2
    for (uint32_t j = 0; j < hidden; j += 16) {
        __m256i v = _mm256_loadu_si256((const __m256i *)(const void *)(acc + j));

        for (int i = 0; i < addCount; ++i)
            v = _mm256_add_epi16(v,
                                 _mm256_loadu_si256((const __m256i *)(const void *)(add[i] + j)));
        for (int i = 0; i < subCount; ++i)
            v = _mm256_sub_epi16(v,
                                 _mm256_loadu_si256((const __m256i *)(const void *)(sub[i] + j)));

        _mm256_storeu_si256((__m256i *)(void *)(acc + j), v);
        _mm256_storeu_si256((__m256i *)(void *)(dst + j), v);
    }
#else
    for (uint32_t j = 0; j < hidden; ++j) {
        int32_t v = acc[j];

        for (int i = 0; i < addCount; ++i)
            v += add[i][j];
        for (int i = 0; i < subCount; ++i)
            v -= sub[i][j];

        acc[j] = (int16_t)v;
        dst[j] = (int16_t)v;
    }
#endif
}

/* Allocated on first use, and again if a new net has a different width. */
static bool nnue_refresh_alloc(EvalState *nt) {
    const uint32_t hidden = Loaded.hot.hidden;

    if (nt->refreshAcc && nt->refreshWidth == hidden)
        return true;
    if (nt->refreshFailed)
        return false;

    free(nt->refreshCache);
    free(nt->refreshAcc);
    nt->refreshCache = (RefreshEntry *)calloc(REFRESH_SLOTS, sizeof(RefreshEntry));
    nt->refreshAcc   = (int16_t *)calloc((size_t)REFRESH_SLOTS * hidden, sizeof(int16_t));

    if (!nt->refreshCache || !nt->refreshAcc) {
        free(nt->refreshCache);
        free(nt->refreshAcc);
        nt->refreshCache  = NULL;
        nt->refreshAcc    = NULL;
        nt->refreshWidth  = 0;
        nt->refreshFailed = true;
        return false;
    }

    nt->refreshWidth = hidden;
    return true;
}

/* Rebuilds one perspective from the refresh cache, falling back to nnue_accumulate() when
 * there is no cache or the diff is too large. */
static void nnue_refresh(EvalState *nt, const Position *pos, Color c, int16_t *dst) {
    const Square king     = king_square(pos, c);
    const Perspective p   = nnue_perspective_of(c, king);
    const uint32_t hidden = Loaded.hot.hidden;

    if (!nnue_refresh_alloc(nt)) {
        nnue_accumulate(pos, &p, dst);
        return;
    }

    const size_t idx      = (size_t)c * SQUARE_NB + (size_t)king;
    RefreshEntry *const e = &nt->refreshCache[idx];
    int16_t *const acc    = nt->refreshAcc + idx * hidden;

    const int16_t *add[REFRESH_MAX_ROWS];
    const int16_t *sub[REFRESH_MAX_ROWS];
    int addCount = 0;
    int subCount = 0;
    bool diff    = e->valid;

    for (Color side = WHITE; side <= BLACK && diff; ++side)
        for (PieceType pt = PAWN; pt <= KING; ++pt) {
            const Bitboard now = pieces_bb(pos, side, pt);
            const Bitboard was = e->pieces[side][pt];

            Bitboard gained = now & ~was;
            Bitboard lost   = was & ~now;

            if (!(gained | lost))
                continue;

            if (addCount + popcount(gained) > REFRESH_MAX_ROWS ||
                subCount + popcount(lost) > REFRESH_MAX_ROWS) {
                diff = false;
                break;
            }

            const Piece pc = make_piece(side, pt);

            while (gained) {
                const int16_t *const row =
                    Loaded.hot.ftWeight +
                    (size_t)nnue_feature_index(&p, pop_lsb(&gained), pc) * hidden;
                nnue_prefetch_row(row);
                add[addCount++] = row;
            }
            while (lost) {
                const int16_t *const row =
                    Loaded.hot.ftWeight +
                    (size_t)nnue_feature_index(&p, pop_lsb(&lost), pc) * hidden;
                nnue_prefetch_row(row);
                sub[subCount++] = row;
            }
        }

    if (diff) {
        nnue_refresh_rows(acc, dst, add, addCount, sub, subCount, hidden);
    } else {
        nnue_accumulate(pos, &p, acc);
        memcpy(dst, acc, hidden * sizeof(int16_t));
    }

    for (Color side = WHITE; side <= BLACK; ++side)
        for (PieceType pt = PAWN; pt <= KING; ++pt)
            e->pieces[side][pt] = pieces_bb(pos, side, pt);
    e->valid = true;
}

void eval_state_retire(void) { ++EvalEpoch; }

/* Called by every thread at the start of every search. The stack always resets; the caches
 * are exact under one net, so they are dropped only when EvalEpoch moved (a new net, or
 * `ucinewgame` via search_clear(), which keeps invariant 7). */
void eval_state_clear(EvalState *nt) {
    if (!nt)
        return;

    nt->accTop = 0;

    if (nt->accStack) {
        nt->accStack[0].key             = 0;
        nt->accStack[0].computed[WHITE] = nt->accStack[0].computed[BLACK] = false;
    }

    if (nt->epoch == EvalEpoch)
        return;
    nt->epoch = EvalEpoch;

    /* A new generation retires every recorded output; the tables are wiped only when the
     * counter wraps. */
    if (nt->refreshCache)
        memset(nt->refreshCache, 0, REFRESH_SLOTS * sizeof(RefreshEntry));
    if (nt->outLevels && ++nt->outGen == 0) {
        memset(nt->outLevels, 0, ACC_LEVELS * sizeof(StackOutputs));
        memset(nt->outCache, 0, OUTPUT_CACHE_ENTRIES * sizeof(StackOutputs));
        nt->outGen = 1;
    }
}

/* Without a stack (failed allocation), push and pop do nothing and every evaluation
 * accumulates from scratch: slow but correct. */
void eval_state_push(EvalState *nt, const Position *pos, Move m) {
    if (!nt || !nt->accStack)
        return;

    assert(nt->accTop + 1 < ACC_LEVELS);

    const Accumulator *const parent = &nt->accStack[nt->accTop];
    Accumulator *const child        = &nt->accStack[++nt->accTop];

    const Key parentKey = pos->history[pos->gamePly - 1].key;

    child->key = pos->key;

    /* The line nnue_stack_outputs() will probe. */
    if (nt->outCache)
        __builtin_prefetch(&nt->outCache[pos->key & (OUTPUT_CACHE_ENTRIES - 1)]);

    NnueDelta d;
    nnue_delta(pos, m, &d);

    const uint32_t hidden = Loaded.hot.hidden;

    for (Color c = WHITE; c <= BLACK; ++c) {
        /* Left for the next evaluation to rebuild. */
        if (d.refresh[c] || !parent->computed[c] || parent->key != parentKey) {
            child->computed[c] = false;
            continue;
        }

        /* `c` did not reindex, so the current board's perspective is valid for both. */
        const Perspective p = nnue_perspective(pos, c);

        const int16_t *add[2];
        const int16_t *sub[2];

        for (int i = 0; i < d.addedCount; ++i)
            add[i] = Loaded.hot.ftWeight +
                     (size_t)nnue_feature_index(&p, d.added[i].sq, d.added[i].pc) * hidden;
        for (int i = 0; i < d.removedCount; ++i)
            sub[i] = Loaded.hot.ftWeight +
                     (size_t)nnue_feature_index(&p, d.removed[i].sq, d.removed[i].pc) * hidden;

        nnue_apply_delta(parent->acc[c], child->acc[c], add, d.addedCount, sub, d.removedCount,
                         hidden);
        child->computed[c] = true;
    }
}

/* Copies the accumulators to a level carrying the new key, or every node under the null
 * move would see a key mismatch and rebuild. */
void eval_state_push_null(EvalState *nt, const Position *pos) {
    if (!nt || !nt->accStack)
        return;

    assert(nt->accTop + 1 < ACC_LEVELS);

    const Accumulator *const parent = &nt->accStack[nt->accTop];
    Accumulator *const child        = &nt->accStack[++nt->accTop];
    const uint32_t hidden           = Loaded.hot.hidden;

    child->key = pos->key;

    if (nt->outCache)
        __builtin_prefetch(&nt->outCache[pos->key & (OUTPUT_CACHE_ENTRIES - 1)]);

    for (Color c = WHITE; c <= BLACK; ++c) {
        child->computed[c] = parent->computed[c];
        if (parent->computed[c])
            memcpy(child->acc[c], parent->acc[c], hidden * sizeof(int16_t));
    }
}

void eval_state_pop(EvalState *nt) {
    if (!nt || !nt->accStack)
        return;

    assert(nt->accTop > 0);
    --nt->accTop;
}

/* The current level, rebuilt where needed; NULL on a thread with no stack. */
static const Accumulator *nnue_current(EvalState *const nt, const Position *pos) {
    if (!nt || !nt->accStack)
        return NULL;

    Accumulator *const a = &nt->accStack[nt->accTop];

    if (a->key != pos->key) {
        a->key             = pos->key;
        a->computed[WHITE] = a->computed[BLACK] = false;
    }

    for (Color c = WHITE; c <= BLACK; ++c)
        if (!a->computed[c]) {
            nnue_refresh(nt, pos, c, a->acc[c]);
            a->computed[c] = true;
        }

    return a;
}

bool nnue_has_uncertainty(void) { return Loaded.hot.uncWeight != NULL && UncertaintyWanted; }

bool nnue_set_uncertainty(bool on) {
    if (on != UncertaintyWanted) {
        UncertaintyWanted = on;
        ++EvalEpoch; /* cached outputs were computed under the other setting */
    }
    return Loaded.hot.uncWeight != NULL;
}

static int32_t nnue_unc_output(const int16_t *own, const int16_t *other, int bucket) {
    assert(Loaded.hot.uncWeight != NULL && "uncertainty asked of a net without the head");
    return nnue_head(own, other, Loaded.hot.uncWeight, Loaded.hot.uncBias, bucket);
}

/* A magnitude, so floored at zero; the exporter clamps the same way. */
static Value nnue_unc_centipawns(int32_t raw) {
    const int64_t num = (int64_t)raw * (int64_t)Loaded.hot.scale;
    const int64_t den = (int64_t)Loaded.hot.qa * (int64_t)Loaded.hot.qb;
    const int64_t cp  = num / den;

    if (cp < 0)
        return 0;
    if (cp > NNUE_EVAL_LIMIT)
        return NNUE_EVAL_LIMIT;
    return (Value)cp;
}

/* Both heads' outputs for `a`'s position, from this level, the table, or a fresh trunk.
 * NULL for a flat net or when allocation fails; the caller then computes directly. */
static const StackOutputs *nnue_stack_outputs(EvalState *const nt, const Accumulator *a,
                                              const Position *pos) {
    if (!Loaded.hot.l1Size)
        return NULL;

    /* Lazily: a net with a stack can be loaded after the thread started. */
    if (!nt->outLevels) {
        nt->outLevels  = (StackOutputs *)calloc(ACC_LEVELS, sizeof(StackOutputs));
        nt->outCache   = (StackOutputs *)calloc(OUTPUT_CACHE_ENTRIES, sizeof(StackOutputs));
        nt->trunkBlock = calloc(1, sizeof(TrunkScratch) + 63);
        if (!nt->outLevels || !nt->outCache || !nt->trunkBlock) {
            free(nt->outLevels);
            free(nt->outCache);
            free(nt->trunkBlock);
            nt->outLevels = nt->outCache = NULL;
            nt->trunkBlock               = NULL;
            return NULL;
        }
        nt->trunkScratch = (TrunkScratch *)(((uintptr_t)nt->trunkBlock + 63) & ~(uintptr_t)63);
        nt->outGen       = 1;
    }

    StackOutputs *const level = &nt->outLevels[nt->accTop];
    if (level->gen == nt->outGen && level->key == pos->key)
        return level;

    StackOutputs *const seen = &nt->outCache[pos->key & (OUTPUT_CACHE_ENTRIES - 1)];
    if (seen->gen == nt->outGen && seen->key == pos->key) {
        *level = *seen;
        return level;
    }

    _Alignas(64) int16_t trunk[NNUE_MAX_STACK_WIDTH];
    const Color stm  = pos->sideToMove;
    const int bucket = nnue_output_bucket(pos);

    nnue_stack_trunk(a->acc[stm], a->acc[stm ^ 1], bucket, trunk, nt->trunkScratch);

    level->key   = pos->key;
    level->gen   = nt->outGen;
    level->value = (int16_t)nnue_centipawns(
        nnue_trunk_head(trunk, Loaded.hot.outWeight, Loaded.hot.outBias, bucket));
    level->unc = Loaded.hot.uncWeight && UncertaintyWanted
                     ? (int16_t)nnue_unc_centipawns(
                           nnue_trunk_head(trunk, Loaded.hot.uncWeight, Loaded.hot.uncBias, bucket))
                     : 0;
    *seen      = *level;
    return level;
}

/* eval.c defines the same symbol in classical builds. */
Value eval_evaluate(EvalState *es, const Position *pos) {
    const Accumulator *const a = nnue_current(es, pos);
    if (!a)
        return nnue_centipawns(nnue_raw(pos));

    /* The debug asserts below check the incremental path and the caches against a full
     * recomputation; a drift would otherwise show only as unreproducible blunders. */
    const StackOutputs *const s = nnue_stack_outputs(es, a, pos);
    if (s) {
        assert(s->value == nnue_centipawns(nnue_raw(pos)) &&
               "recorded stack output disagrees with a full recomputation");
        return s->value;
    }

    const Color stm   = pos->sideToMove;
    const int32_t raw = nnue_output(a->acc[stm], a->acc[stm ^ 1], nnue_output_bucket(pos));

    assert(raw == nnue_raw(pos) && "incremental accumulator disagrees with a full recomputation");

    return nnue_centipawns(raw);
}

/* Out of line so its 8 KB frame does not cost nnue_uncertainty() a stack probe per call. */
static __attribute__((noinline)) Value nnue_uncertainty_scratch(const Position *pos) {
    _Alignas(64) int16_t acc[COLOR_NB][NNUE_MAX_HIDDEN];

    for (Color c = WHITE; c <= BLACK; ++c) {
        const Perspective p = nnue_perspective(pos, c);
        nnue_accumulate(pos, &p, acc[c]);
    }

    const Color stm = pos->sideToMove;
    return nnue_unc_centipawns(nnue_unc_output(acc[stm], acc[stm ^ 1], nnue_output_bucket(pos)));
}

Value nnue_uncertainty(EvalState *es, const Position *pos) {
    const Accumulator *const a = nnue_current(es, pos);
    if (!a)
        return nnue_uncertainty_scratch(pos);

    const Color stm  = pos->sideToMove;
    const int bucket = nnue_output_bucket(pos);

    /* Usually already recorded with the value. */
    const StackOutputs *const s = nnue_stack_outputs(es, a, pos);
    if (s) {
        assert(Loaded.hot.uncWeight != NULL && "uncertainty asked of a net without the head");
        assert(s->unc ==
                   nnue_unc_centipawns(nnue_unc_output(a->acc[stm], a->acc[stm ^ 1], bucket)) &&
               "recorded uncertainty disagrees with a full recomputation");
        return s->unc;
    }

    return nnue_unc_centipawns(nnue_unc_output(a->acc[stm], a->acc[stm ^ 1], bucket));
}

const char *nnue_hash(void) { return Loaded.loaded ? Loaded.hash : "no-net"; }

void nnue_print_info(void) {
    if (!Loaded.loaded) {
        printf("info string no net loaded\n");
        fflush(stdout);
        return;
    }

    const NnueHeader *h = &Loaded.hdr;
    char tag[NNUE_TAG_LEN + 1];
    memcpy(tag, h->tag, NNUE_TAG_LEN);
    tag[NNUE_TAG_LEN] = '\0';

    char stack[32] = "";
    if (h->l1Size) {
        if (h->l2Size)
            snprintf(stack, sizeof(stack), "->%u->%u", h->l1Size, h->l2Size);
        else
            snprintf(stack, sizeof(stack), "->%u", h->l1Size);
    }

    /* Stated either way, so an A/B log shows which setting ran. */
    const char *unc = !Loaded.hot.uncWeight ? "" : UncertaintyWanted ? "+unc" : "+unc(off)";

    printf("info string net %.12s  %u->%ux2%s->%u%s  %s halfka-32sq %s  qa %u qb %u "
           "scale %d  tag %s  from %s\n",
           Loaded.hash, h->features, h->hidden, stack, h->outputBuckets, unc,
           h->activation == NNUE_ACT_PAIRWISE ? "pairwise" : "screlu",
#ifdef NNUE_AVX2
           "avx2",
#else
           "scalar",
#endif
           h->qa, h->qb, h->scale, tag, Loaded.source);
    fflush(stdout);
}

int nnue_verify_vectors(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("nnue verify: cannot open %s\n", path);
        printf("             write it with tools/export_net.py --vectors\n");
        return 1;
    }

    char line[512];
    long checked = 0, failed = 0;

    /* Whether the net has the head, not the option: the gate checks what the file carries. */
    const bool wantUnc = Loaded.hot.uncWeight != NULL;

    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
            continue;

        long expectedRaw, expectedCp, expectedUncRaw = 0, expectedUncCp = 0;
        int consumed = 0;

        const int fields = wantUnc
                               ? sscanf(line, "%ld %ld %ld %ld %n", &expectedRaw, &expectedCp,
                                        &expectedUncRaw, &expectedUncCp, &consumed)
                               : sscanf(line, "%ld %ld %n", &expectedRaw, &expectedCp, &consumed);
        if (fields != (wantUnc ? 4 : 2) || !consumed) {
            printf("nnue verify: expected %d columns (net %s the uncertainty head): %s",
                   wantUnc ? 4 : 2, wantUnc ? "carries" : "lacks", line);
            ++failed;
            continue;
        }

        char *const fen = line + consumed;
        for (char *p = fen; *p; ++p)
            if (*p == '\n' || *p == '\r') {
                *p = '\0';
                break;
            }

        Position pos;
        memset(&pos, 0, sizeof(pos));
        if (!board_set_fen(&pos, fen)) {
            printf("nnue verify: unparseable FEN: %s\n", fen);
            ++failed;
            continue;
        }

        const int32_t raw = nnue_raw(&pos);
        const Value cp    = nnue_centipawns(raw);

        bool ok     = (long)raw == expectedRaw && (long)cp == expectedCp;
        long uncRaw = 0, uncCp = 0;
        if (wantUnc) {
            /* From scratch when the accumulator stack could not be allocated. */
            _Alignas(64) int16_t local[COLOR_NB][NNUE_MAX_HIDDEN];
            const Accumulator *const a = nnue_current(eval_state(), &pos);
            const Color stm            = pos.sideToMove;

            if (!a)
                for (Color c = WHITE; c <= BLACK; ++c) {
                    const Perspective p = nnue_perspective(&pos, c);
                    nnue_accumulate(&pos, &p, local[c]);
                }

            const int16_t *const own   = a ? a->acc[stm] : local[stm];
            const int16_t *const other = a ? a->acc[stm ^ 1] : local[stm ^ 1];

            uncRaw = nnue_unc_output(own, other, nnue_output_bucket(&pos));
            uncCp  = nnue_unc_centipawns((int32_t)uncRaw);
            ok     = ok && uncRaw == expectedUncRaw && uncCp == expectedUncCp;
        }

        if (!ok) {
            if (failed < 10) {
                printf("nnue verify: MISMATCH  raw %ld != %ld   cp %ld != %ld", (long)raw,
                       expectedRaw, (long)cp, expectedCp);
                if (wantUnc)
                    printf("   unc %ld != %ld   ucp %ld != %ld", uncRaw, expectedUncRaw, uncCp,
                           expectedUncCp);
                printf("   %s\n", fen);
            }
            ++failed;
        }
        ++checked;
    }
    fclose(f);

    if (checked == 0) {
        printf("FAIL: %s contained no test vectors\n", path);
        return 1;
    }
    if (failed) {
        printf("FAIL: %ld of %ld positions disagree with the Python reference\n", failed, checked);
        return 1;
    }

    printf("PASS: %ld positions, C inference matches the quantised reference exactly\n", checked);
    return 0;
}

#endif
