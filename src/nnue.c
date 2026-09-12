/*
 * nnue.c - loading and evaluating the network. See nnue.h for the file format.
 *
 * The arithmetic here is the C half of a two-implementation contract: every number this
 * file produces is produced independently by tools/export_net.py in numpy, and
 * `make nnue-test` requires the two to agree EXACTLY on ten thousand positions. That is
 * the only reason to trust it - a quantisation that is subtly wrong loses about 30 Elo
 * and looks healthy from every other angle.
 *
 * An evaluation is almost entirely two sums of at most 32 rows of `hidden` int16
 * weights, so the accumulator is int16 rather than int32 (half the traffic, twice the
 * lanes) and there is one activation and one feature set, hence no branch per unit or
 * per piece. AVX2 and plain C must produce IDENTICAL integers, which they do because
 * integer addition is associative and nothing in either path overflows.
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

/* How many int32 lanes of SCReLU may accumulate before being widened to int64. One
 * madd result is bounded by 2 * 32767 * 255 = 16,711,170, so 64 of them is 1.07e9,
 * comfortably inside int32. Flushing per 64 vectors costs one horizontal add and makes
 * the bound independent of the hidden width. */
#define NNUE_SCRELU_FLUSH 64
#endif

/* The exporter packs the header with an explicit struct format string, so its size is a
 * contract rather than an implementation detail: a field that made the compiler insert
 * padding would shift every weight by a few bytes, and the failure would look like a net
 * that trained badly rather than one that loaded wrong. */
_Static_assert(sizeof(NnueHeader) == 112, "NnueHeader must stay 112 bytes; see HEADER_FMT in "
                                          "tools/export_net.py");

/* Nothing structural stops a network from emitting an enormous number, and a static
 * evaluation that wanders into mate territory makes the search report forced mates that
 * do not exist. Far above any real evaluation, far below VALUE_MATE_IN_MAX_PLY. */
#define NNUE_EVAL_LIMIT 20000

/* A net is 50 MB of gitignored data and the bench node count depends on which one is
 * embedded, so a build has to be able to say which net it carries. FIPS 180-4 in ninety
 * lines, because the engine links against nothing but libc. */
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

/*
 * The net is embedded with .incbin so the shipped binary is self-contained: OpenBench
 * builds one file and runs it, and a bench node count has to be reproducible from the
 * binary alone. NNUE_EVALFILE is relative to the directory make ran in, which is the
 * assembler's working directory too.
 *
 * Mach-O spells both halves differently to ELF - __TEXT,__const rather than .rodata, and
 * an underscore prefix on every C identifier - and emitting the ELF spelling there fails
 * at the .section directive, which is where the arm64 release build lost its net.
 */
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
        ".balign 64\n"
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
 * Everything an evaluation reads, and nothing else, packed into two cache lines.
 *
 * This is a layout decision with a measurement behind it. The fields below used to be
 * read out of `hdr` and out of a pointer block sitting behind it, and growing the header
 * from 96 to 112 bytes for the stack's four fields pushed the weight pointers onto
 * another cache line - which cost ~3.7% of nps on a net that evaluates identically. The
 * hot path never reads the header, so the header should not be in its way.
 *
 * DERIVED AT LOAD, in nnue_adopt(), from `hdr`. The header stays the only thing a net
 * file is parsed into and the only thing nnue_validate() checks, so this is a cache and
 * not a second source of truth.
 *
 * outWeight/outBias are the FINAL layer in both architectures: the flat output layer
 * without a stack, L3 with one.
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
    uint32_t inputs; /* what L1 reads: 2 * hidden, one activation per accumulator unit */
    uint32_t buckets;
    uint32_t trunkWidth; /* what both output heads read */
    uint32_t l1Size;
    uint32_t l2Size;

    /* Shift counts, all under 31 and so a byte each. Narrow on purpose: the four of them
     * as words would push this block past the two cache lines it is meant to be, and
     * bucketShift is here to retire two integer divisions from the hot path. */
    uint8_t l1Shift;
    uint8_t l2Shift;
    uint8_t actShift;    /* log2(qa) - why a stacked net's qa must be a power of two */
    uint8_t bucketShift; /* log2(32 / buckets), which nnue_validate() makes a power of two */

    int32_t qa;
    int32_t qb;
    int32_t scale;
} NnueHot;

_Static_assert(sizeof(NnueHot) <= 128, "the hot block is meant to be two cache lines");

typedef struct {
    _Alignas(64) NnueHot hot;

    NnueHeader hdr;
    unsigned char *owned;
    char hash[65];
    char source[512];
    bool loaded;
} Net;

static Net Loaded;

/* How many king slots a feature set folds the board onto, or 0 if this build has never
 * heard of it. The count is the feature set's shape, so the loader derives the expected
 * feature count from it rather than trusting the file to be self-consistent. */
static uint32_t nnue_king_slots(uint32_t featureSet) {
    switch (featureSet) {
    case NNUE_FEATURES_HALFKA_32SQ: return 32;
    default: return 0;
    }
}

/* What the output heads read: the activated accumulator with no stack, the stack's last
 * hidden layer with one. A header field would be a second place for it to be wrong. */
static uint32_t nnue_trunk_width(const NnueHeader *h) {
    if (h->l1Size == 0)
        return 2u * h->hidden;
    return h->l2Size ? h->l2Size : h->l1Size;
}

/* Bytes the payload must occupy for this header to be self-consistent. The uncertainty
 * head is a second output layer, so its flag adds exactly one more outWeight-and-outBias
 * worth - which is why an engine that never heard of the flag rejects on this count.
 *
 * Every section is a whole number of int32s wide by construction - the widths are
 * multiples of 16 - so nothing here needs padding and the int32 biases land aligned. */
static uint64_t nnue_payload_bytes(const NnueHeader *h) {
    const uint64_t buckets = h->outputBuckets;
    const uint64_t headBytes =
        buckets * nnue_trunk_width(h) * sizeof(int16_t) + buckets * sizeof(int32_t);

    uint64_t stackBytes = 0;
    if (h->l1Size) {
        stackBytes = buckets * h->l1Size * 2u * h->hidden * sizeof(int16_t) +
                     buckets * h->l1Size * sizeof(int32_t);
        if (h->l2Size)
            stackBytes += buckets * h->l2Size * h->l1Size * sizeof(int16_t) +
                          buckets * h->l2Size * sizeof(int32_t);
    }

    return (uint64_t)h->features * h->hidden * sizeof(int16_t) +
           (uint64_t)h->hidden * sizeof(int16_t) + stackBytes +
           headBytes * (h->reserved[0] == 1 ? 2u : 1u);
}

/* Every rejection names the field and both values. A net that fails to load is almost
 * always someone mid-upgrade, and "hidden width 1024, this build holds at most 512" is a
 * fix where "bad net file" is a morning. */
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

    if (h->activation != NNUE_ACT_SCRELU)
        REJECT("activation %u is not implemented (this build runs %u, screlu) - add its "
               "case to nnue_activate() in src/nnue.c",
               h->activation, (unsigned)NNUE_ACT_SCRELU);

    /* The feature set's tag defines its own shape, so a file that disagrees was written by
     * an exporter with a different idea of what the tag means. */
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

    /* Buckets are indexed (pieceCount - 2) / (32 / buckets), which covers every row only
     * when the count divides 32. Otherwise some piece counts read a bucket that was never
     * trained. */
    if (h->outputBuckets == 0 || h->outputBuckets > NNUE_MAX_OUTPUT_BUCKETS ||
        32u % h->outputBuckets != 0)
        REJECT("%u output buckets - must be a divisor of 32, at most %u", h->outputBuckets,
               (unsigned)NNUE_MAX_OUTPUT_BUCKETS);

    /* The stack. Zero is the flat output layer, which is a complete net and not a
     * degenerate one, so every check here is conditional on there being a stack at all. */
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

    /* L2 reads L1's output, so a file claiming one without the other describes a shape
     * that has no arithmetic. It is a malformed header rather than an unsupported one. */
    if (h->l2Size && !h->l1Size)
        REJECT("l2 width %u with no l1 - L2 reads L1's output and there is none", h->l2Size);

    /* A shift that belongs to a layer the file does not carry is the signature of an
     * exporter that meant something this build does not implement. */
    if ((h->l1Size == 0 && (h->l1Shift || h->l2Shift)) || (h->l2Size == 0 && h->l2Shift))
        REJECT("shifts %u/%u are set for stack widths %u/%u - one of the two is wrong", h->l1Shift,
               h->l2Shift, h->l1Size, h->l2Size);

    if (h->l1Size && (h->l1Shift == 0 || h->l1Shift > 30))
        REJECT("l1 shift %u is outside 1..30; it requantises an int32 sum", h->l1Shift);

    if (h->l2Size && (h->l2Shift == 0 || h->l2Shift > 30))
        REJECT("l2 shift %u is outside 1..30; it requantises an int32 sum", h->l2Shift);

    if (h->qa == 0 || h->qb == 0 || h->scale == 0)
        REJECT("degenerate quantisation (qa %u, qb %u, scale %d)", h->qa, h->qb, h->scale);

    /* The stack's activation is `(x * x) >> log2(qa)`, which lands in [0, qa] exactly
     * when qa is a power of two and only then. The flat architecture divides by qa once
     * at the end instead, where any qa works - so this is a constraint the stack adds
     * rather than one the format always had. */
    if (h->l1Size && (h->qa & (h->qa - 1u)) != 0)
        REJECT("qa %u is not a power of two, which a net with a layer stack requires - its "
               "activation shifts by log2(qa) rather than dividing",
               h->qa);

    /* qa is the SCReLU clamp ceiling, and the vectorised path materialises it with
     * _mm256_set1_epi16 - so a qa above INT16_MAX truncates THERE and not in the scalar
     * path, and the same net evaluates differently on two builds of this engine. */
    if (h->qa > INT16_MAX)
        REJECT("qa %u exceeds the int16 clamp ceiling %d that the vectorised accumulator "
               "requires",
               h->qa, (int)INT16_MAX);

    /* Only the magnitude is ever the intent. A negative scale loads cleanly, prints a
     * normal-looking info line, and negates every evaluation. */
    if (h->scale < 0)
        REJECT("scale %d is negative, which would invert every evaluation", h->scale);

    /* reserved[0] is the uncertainty flag; the rest must still be zero. A future exporter
     * that used one would otherwise have its field silently ignored, which is the exact
     * failure the reserved block exists to make loud. */
    if (h->reserved[0] > 1)
        REJECT("uncertainty flag %u, this build reads 0 or 1 - re-export with the current "
               "tools/export_net.py",
               h->reserved[0]);
    for (int i = 1; i < 16; ++i)
        if (h->reserved[i] != 0)
            REJECT("reserved byte %d is %u; this build understands only the uncertainty "
                   "flag in byte 0 - it is too old for whatever wrote this net",
                   i, h->reserved[i]);

    /* Not checked: that the weights keep the int16 accumulator and the int16 SCReLU
     * product in range. tools/export_net.py refuses to WRITE a net that does not, and it
     * is the only thing that writes one; re-deriving a looser bound here would cost a
     * pass over 50 MB and could only reject a net the exporter already blessed. */
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

/* Points the net at a validated blob and hashes it. `owned` is NULL for the embedded
 * blob, which lives in rodata and must not be freed. */
static void nnue_adopt(const unsigned char *blob, size_t bytes, unsigned char *owned,
                       const char *source) {
    if (Loaded.owned)
        free(Loaded.owned);

    memcpy(&Loaded.hdr, blob, sizeof(NnueHeader));
    Loaded.owned = owned;

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

    hot->inputs = 2u * h->hidden;

    /* log2(qa), which validate() has already established exists. Counted rather than
     * reached for by a builtin so ARCH=popcnt and a compiler without one both build. */
    for (uint32_t q = h->qa; q > 1u; q >>= 1)
        ++hot->actShift;

    /* log2(32 / buckets). Validate() required the count to divide 32, and every divisor
     * of 32 is a power of two, so nnue_output_bucket() can shift where it divided. */
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

    sha256_hex(blob, bytes, Loaded.hash);
    snprintf(Loaded.source, sizeof(Loaded.source), "%s", source);
    Loaded.loaded = true;
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

    unsigned char *blob = (unsigned char *)malloc((size_t)size);
    if (!blob) {
        printf("info string EvalFile: out of memory reading %s\n", path);
        fflush(stdout);
        fclose(f);
        return false;
    }

    const size_t got = fread(blob, 1, (size_t)size, f);
    fclose(f);

    if (got != (size_t)size || !nnue_validate(blob, got, path)) {
        free(blob);
        return false;
    }

    nnue_adopt(blob, got, blob, path);
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
    nnue_adopt(nnueEmbeddedStart, bytes, NULL, NNUE_EVALFILE " (embedded)");
#else
    printf("info string this build embeds no net: rebuild with "
           "'make EVALFILE=<path>'\n");
    fflush(stdout);
    exit(1);
#endif
}

/* The mirrored king square: file 0-3 after the mirror, so 32 slots. The net indexes this
 * directly rather than a bucketing of it, which is what lets it tell a king on g1 from
 * one on h1. */
static inline int nnue_king_square(Square normalisedKing) {
    return (int)rank_of(normalisedKing) * 4 + (int)file_of(normalisedKing);
}

typedef struct {
    Color side;
    bool mirror;
    int slot;
} Perspective;

/*
 * One perspective's view of the board: rank-flipped for black, file-mirrored when that
 * side's king sits kingside. The mirror is driven by the KING and applied to every
 * square, exactly as TERM_PSQK does in eval.c.
 *
 * Split from nnue_perspective() so the incremental update can ask what a perspective
 * WOULD be with the king elsewhere: a king move that leaves both `slot` and `mirror`
 * alone is an ordinary two-feature delta rather than a refresh, which is most king
 * moves.
 */
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

/* UPGRADE POINT: the index function IS the feature set. Anything that is not "king slot
 * x 12 planes x 64 squares" gets its own version here, selected on the tag. Planes 0-5
 * are the perspective's own pieces and 6-11 the enemy's, each in PAWN..KING order. */
static inline int nnue_feature_index(const Perspective *p, Square sq, Piece pc) {
    Square s = (p->side == BLACK) ? flip_rank(sq) : sq;
    if (p->mirror)
        s = (Square)(s ^ 7);

    const int plane = (color_of(pc) == p->side ? 0 : 6) + (int)type_of(pc) - 1;
    return p->slot * (12 * 64) + plane * 64 + (int)s;
}

/* One perspective's accumulator, from scratch. This is the evaluation: at 1024 wide it
 * is 32 rows of 1024 int16 additions, and everything else in this file is rounding error
 * beside it. */
static void nnue_accumulate(const Position *pos, const Perspective *p, int16_t *acc) {
    const uint32_t hidden = Loaded.hot.hidden;

    /*
     * Rows are resolved first, in one pass over the occupancy, and only then summed. That
     * unblocks the address arithmetic from the adds and makes the NEXT row known while
     * the current one is being added, which is what the prefetch below needs - worth
     * about 4% of bench nps, small but consistent.
     *
     * One row per occupied square, sized to a full board rather than the 32 of a legal
     * position, because board_set_fen accepts any diagram that fits on the squares.
     */
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

/* Piece count folded onto the net's buckets. The expression is output_bucket() in
 * trainer/nnue/format.py and must stay bit-identical to it, so the divisor stays 32
 * whatever the engine's piece cap is - which is why the clamp is here instead: a 40-man
 * puzzle would read off the end of outWeight. */
static inline int nnue_output_bucket(const Position *pos) {
    /* Both divisions the expression above names are by 32 / buckets, which nnue_validate()
     * has already established is a power of two, so the load-time shift is the same
     * arithmetic and not an approximation of it. `n <= 0` reproduces what C's / does with
     * a count below two, which no legal position has and no FEN needs to be trusted about:
     * truncation toward zero gives 0, where an arithmetic shift would give -1. */
    const int n      = popcount(occupied_bb(pos)) - 2;
    const int bucket = n <= 0 ? 0 : n >> Loaded.hot.bucketShift;

    return bucket < (int)Loaded.hot.buckets ? bucket : (int)Loaded.hot.buckets - 1;
}

#ifdef NNUE_AVX2

/* Eight int32 lanes into an int64. Called once per NNUE_SCRELU_FLUSH vectors, so the
 * store round-trip is free and the clarity is worth having. */
static inline int64_t nnue_hsum_epi32(__m256i v) {
    int32_t lanes[8];
    _mm256_storeu_si256((__m256i *)(void *)lanes, v);

    int64_t sum = 0;
    for (int i = 0; i < 8; ++i)
        sum += lanes[i];
    return sum;
}

/* The same reduction without the round trip, for the sums that fit int32.
 *
 * tools/export_net.py refuses a net whose layer sums could leave int32, and any subset
 * of those terms - which is what a lane holds - is bounded by the same number, so the
 * pairwise order this reduces in is exact and not merely close. That is what lets the
 * scalar path add the same terms left to right and still agree to the bit.
 */
static inline int32_t nnue_hsum32(__m256i v) {
    __m128i s = _mm_add_epi32(_mm256_castsi256_si128(v), _mm256_extracti128_si256(v, 1));

    s = _mm_add_epi32(s, _mm_shuffle_epi32(s, 0x4E));
    s = _mm_add_epi32(s, _mm_shuffle_epi32(s, 0xB1));
    return _mm_cvtsi128_si32(s);
}

/* Four accumulators reduced together, into the four sums they hold. Two hadd rounds
 * leave each accumulator's halves in the two 128-bit lanes, and the fold across adds
 * them. */
static inline __m128i nnue_hsum32x4(__m256i a, __m256i b, __m256i c, __m256i d) {
    const __m256i ab   = _mm256_hadd_epi32(a, b);
    const __m256i cd   = _mm256_hadd_epi32(c, d);
    const __m256i abcd = _mm256_hadd_epi32(ab, cd);

    return _mm_add_epi32(_mm256_castsi256_si128(abcd), _mm256_extracti128_si256(abcd, 1));
}

/* SCReLU against one half of the output row. v * w stays in int16 - the exporter refuses
 * a net where it would not - and madd then widens (v * w) * v into int32 pairs, flushed
 * as the bound at the top of this file describes. */
static inline int64_t nnue_screlu_half(const int16_t *acc, const int16_t *w, uint32_t hidden,
                                       int32_t qa) {
    const __m256i zero = _mm256_setzero_si256();
    const __m256i top  = _mm256_set1_epi16((short)qa);

    int64_t total = 0;
    uint32_t j    = 0;

    /* The flush bound is a property of the WIDTH, not of any one vector, so it belongs in
     * an outer loop. A counter tested inside the inner loop spends three of that loop's
     * seven uops - an increment, a compare and a branch - answering a question the trip
     * count already settled, and the multiplies queue behind them for issue slots.
     *
     * Two accumulators rather than one, for the same reason: consecutive vectors then
     * travel independent dependency chains, and each holds half of what the bound at the
     * top of this file allows rather than all of it. */
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

/*
 * The side to move always reads its own accumulator first. Getting this backwards
 * produces a net that plays reasonably, hates its own position, and trains to a loss
 * curve that looks completely normal.
 *
 * SCReLU's rescale is the part with a wrong answer that looks right: the squared
 * activation carries QA^2 where the bias carries QA, so the sum is divided by QA -
 * truncating toward zero, as C's / does - BEFORE the bias is added. numpy's // would
 * floor instead, and `make nnue-test` is what proves the two agree on the negatives.
 *
 * UPGRADE POINT: a new NnueActivation gets a branch here and a case in the exporter's
 * forward().
 */
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

    /* int64 because a term reaches QA^2 * 32767 and there are 2 * hidden of them. Both
     * orders sum the same integers, so both give the same answer. */
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

/*
 * ---------------------------------------------------------------- the layer stack --
 *
 * Everything below runs only when the net carries one (`l1Size > 0`). See "Task 6" in
 * docs/NNUE.md for why it exists; the short version is that the flat head above forms
 * `v * w` as int16, which caps a quantised output weight at 128 and had the shipped net
 * sitting on 127.
 *
 * The stack's dot products are `madd_epi16`, which forms each product in 32 bits, so
 * nothing between the accumulator and the score can leave int16 any more and the weights
 * get twelve bits instead of seven.
 */

/* One layer's dot product: int16 activations, int16 weights, int32 out.
 *
 * The int32 accumulation is sound rather than hopeful: tools/export_net.py refuses a net
 * whose `|bias| + qa * sum|w|` could leave int32, and any subset of those terms - which
 * is what one SIMD lane holds - is bounded by the same number. That is also why the two
 * paths agree exactly: integer addition is associative, and neither order overflows.
 */
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

/* One perspective's SCReLU output as a vector L1 can read: clamp to [0, qa], square, and
 * shift back down by log2(qa) so the result lands in [0, qa] again.
 *
 * The flat head never materialises this - it fuses the square into its own dot product
 * and divides by qa once at the end. A stack has to hand L1 a real vector, and doing the
 * rescale per element is what keeps that vector int16 and its dot products plain
 * `madd_epi16`. It is also why a stacked net's qa must be a power of two: per element,
 * a shift is a shift and a divide is a divide.
 */
static inline void nnue_activate_half(const int16_t *acc, int16_t *out, uint32_t n, int32_t qa,
                                      uint32_t shift) {
#ifdef NNUE_AVX2
    const __m256i zero = _mm256_setzero_si256();
    const __m256i top  = _mm256_set1_epi16((short)qa);
    const __m128i down = _mm_cvtsi32_si128((int)shift);
    const __m128i up   = _mm_cvtsi32_si128((int)(16u - shift));

    for (uint32_t j = 0; j < n; j += 16) {
        const __m256i a = _mm256_loadu_si256((const __m256i *)(const void *)(acc + j));
        const __m256i v = _mm256_min_epi16(_mm256_max_epi16(a, zero), top);

        /* x * x reaches qa^2, which does not fit int16 for any qa past 181, so the
         * product is taken as its two halves and the shift reassembles them. Exact, and
         * the scalar path below is the same arithmetic in one expression. */
        const __m256i lo = _mm256_mullo_epi16(v, v);
        const __m256i hi = _mm256_mulhi_epu16(v, v);

        _mm256_storeu_si256((__m256i *)(void *)(out + j),
                            _mm256_or_si256(_mm256_sll_epi16(hi, up), _mm256_srl_epi16(lo, down)));
    }
#else
    for (uint32_t j = 0; j < n; ++j) {
        const int32_t x = acc[j] < 0 ? 0 : (acc[j] > qa ? qa : acc[j]);
        out[j]          = (int16_t)((x * x) >> shift);
    }
#endif
}

/*
 * The whole activation vector L1 reads, from both accumulators: SCReLU of every unit,
 * so `2 * hidden` numbers.
 *
 * UPGRADE POINT: a new NnueActivation gets its case here and in the exporter's
 * stack_trunk().
 */
static inline void nnue_activate(const NnueHot *hot, const int16_t *own, const int16_t *other,
                                 int16_t *out) {
    const uint32_t h = hot->hidden;

    nnue_activate_half(own, out, h, hot->qa, hot->actShift);
    nnue_activate_half(other, out + h, h, hot->qa, hot->actShift);
}

/* One stage's int32 sum, back into the [0, qa] range the next stage's weights are scaled
 * against.
 *
 * CLAMP BEFORE THE SHIFT. `>>` on a negative int32 is implementation-defined in C17, and
 * clamping to zero first is what makes this line, numpy's `>>` and a floor division all
 * agree on every value - which is the whole basis of `make nnue-test` being an exact
 * test rather than an approximate one.
 */
static inline int16_t nnue_requantise(int32_t sum, uint32_t shift, int32_t ceiling) {
    /* Round to nearest rather than toward zero. A bare shift truncates, and a truncation
     * at every stage of every unit is not noise - it is a systematic downward bias that
     * compounds across the stack and would make the quantised net uniformly more
     * pessimistic than the one that was trained. The half is exact in both languages and
     * costs an add; tools/export_net.py carries it in its int32 bound. */
    const int32_t half = (int32_t)1 << (shift - 1);
    const int32_t r    = ((sum < 0 ? 0 : sum) + half) >> shift;

    return (int16_t)(r > ceiling ? ceiling : r);
}

/*
 * One whole layer: every unit's dot product against the same input, biased and
 * requantised.
 *
 * The transposition is the point, and it is worth more than the arithmetic. A
 * unit-at-a-time loop reads the entire input vector once PER UNIT - sixteen passes over
 * two kilobytes to do one pass' worth of multiplies - and finishes each unit with a
 * horizontal sum through memory, whose store-to-load round trip the next unit then waits
 * on. Taking four units together reads the input once for the four of them and reduces
 * their partial sums in registers.
 *
 * Four rather than more because more buys almost nothing. The layer is not short of issue
 * slots once the input is being read once per group - it is short of BANDWIDTH: L1 is
 * l1Size * 2 * hidden weights, which at the shape this was written for is 32 KB per
 * bucket per evaluation, and that is the whole of an L1 data cache. Taking eight units at
 * a time measures half a percent, because the weights still have to arrive.
 *
 * Every width here is a multiple of NNUE_WIDTH_MULTIPLE, which nnue_validate() enforces
 * on both the accumulator and the stack, so neither loop needs a tail.
 */
static void nnue_layer(const int16_t *in, uint32_t n, const int16_t *w, const int32_t *bias,
                       uint32_t units, uint32_t shift, int32_t ceiling, int16_t *out) {
#ifdef NNUE_AVX2
    for (uint32_t u = 0; u < units; u += 4) {
        const int16_t *const w0 = w + (size_t)u * n;
        const int16_t *const w1 = w0 + n;
        const int16_t *const w2 = w1 + n;
        const int16_t *const w3 = w2 + n;

        __m256i s0 = _mm256_setzero_si256();
        __m256i s1 = _mm256_setzero_si256();
        __m256i s2 = _mm256_setzero_si256();
        __m256i s3 = _mm256_setzero_si256();

        for (uint32_t j = 0; j < n; j += 16) {
            const __m256i v = _mm256_loadu_si256((const __m256i *)(const void *)(in + j));

            s0 = _mm256_add_epi32(
                s0,
                _mm256_madd_epi16(v, _mm256_loadu_si256((const __m256i *)(const void *)(w0 + j))));
            s1 = _mm256_add_epi32(
                s1,
                _mm256_madd_epi16(v, _mm256_loadu_si256((const __m256i *)(const void *)(w1 + j))));
            s2 = _mm256_add_epi32(
                s2,
                _mm256_madd_epi16(v, _mm256_loadu_si256((const __m256i *)(const void *)(w2 + j))));
            s3 = _mm256_add_epi32(
                s3,
                _mm256_madd_epi16(v, _mm256_loadu_si256((const __m256i *)(const void *)(w3 + j))));
        }

        int32_t sums[4];
        _mm_storeu_si128((__m128i *)(void *)sums, nnue_hsum32x4(s0, s1, s2, s3));

        for (uint32_t k = 0; k < 4; ++k)
            out[u + k] = nnue_requantise(sums[k] + bias[u + k], shift, ceiling);
    }
#else
    for (uint32_t u = 0; u < units; ++u)
        out[u] = nnue_requantise(nnue_dot(in, w + (size_t)u * n, n) + bias[u], shift, ceiling);
#endif
}

/* The vector both output heads read, for one bucket. */
static void nnue_stack_trunk(const int16_t *own, const int16_t *other, int bucket, int16_t *trunk) {
    const NnueHot *const hot = &Loaded.hot;
    const uint32_t inputs    = hot->inputs;
    const int32_t qa         = hot->qa;
    const size_t b           = (size_t)bucket;

    _Alignas(64) int16_t a[2 * NNUE_MAX_HIDDEN];
    _Alignas(64) int16_t first[NNUE_MAX_STACK_WIDTH];

    nnue_activate(hot, own, other, a);

    /* With no L2, L1 IS the trunk and writes straight into the caller's buffer. */
    int16_t *const l1Out = hot->l2Size ? first : trunk;

    nnue_layer(a, inputs, hot->l1Weight + b * hot->l1Size * inputs, hot->l1Bias + b * hot->l1Size,
               hot->l1Size, hot->l1Shift, qa, l1Out);

    if (!hot->l2Size)
        return;

    nnue_layer(first, hot->l1Size, hot->l2Weight + b * hot->l2Size * hot->l1Size,
               hot->l2Bias + b * hot->l2Size, hot->l2Size, hot->l2Shift, qa, trunk);
}

/* One output head over a trunk that has already been built. */
static inline int32_t nnue_trunk_head(const int16_t *trunk, const int16_t *weights,
                                      const int32_t *biases, int bucket) {
    const uint32_t width = Loaded.hot.trunkWidth;

    return nnue_dot(trunk, weights + (size_t)bucket * width, width) + biases[bucket];
}

/*
 * One head's score from the accumulators alone, building whatever the architecture needs
 * on the way. This is the FROM-SCRATCH path - the verifier and the debug assert - and it
 * deliberately caches nothing; eval_evaluate() and nnue_uncertainty() share a trunk
 * through the accumulator stack instead.
 */
static int32_t nnue_head(const int16_t *own, const int16_t *other, const int16_t *weights,
                         const int32_t *biases, int bucket) {
    if (!Loaded.hot.l1Size)
        return nnue_flat_head(own, other, weights, biases, bucket);

    _Alignas(64) int16_t trunk[NNUE_MAX_STACK_WIDTH];

    nnue_stack_trunk(own, other, bucket, trunk);
    return nnue_trunk_head(trunk, weights, biases, bucket);
}

/* The value head. The uncertainty head is the same arithmetic over its own weights. */
static inline int32_t nnue_output(const int16_t *own, const int16_t *other, int bucket) {
    return nnue_head(own, other, Loaded.hot.outWeight, Loaded.hot.outBias, bucket);
}

/* int64 because raw reaches several million and the scale is 400. The division TRUNCATES
 * toward zero, which tools/export_net.py reproduces explicitly: numpy's floor division
 * rounds the other way for negatives, and that asymmetry alone would fail about half the
 * test vectors. */
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

/* The raw integer output, shared by the evaluation and the verifier so the gate tests
 * the arithmetic the engine actually runs. */
static int32_t nnue_raw(const Position *pos) {
    /* Aligned so the vector loads and stores land on cache-line boundaries. 8 KB of stack
     * at the maximum width, which is fine at every depth the search reaches. */
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
 * The accumulator stack. A from-scratch accumulation is up to 32 rows per perspective
 * and is essentially the entire cost of the network, while a move changes at most two
 * features per perspective - three on a capture, four on a castle - so carrying it
 * across make/unmake replaces 64 rows with 4.
 *
 * Two things make that safe rather than merely fast. Every level records the Zobrist key
 * of the position it describes and rebuilds from the board when the key does not match,
 * so a missing push costs a recomputation and never a wrong score; and debug builds
 * assert the incremental value against a full recomputation at every evaluation, which
 * is the only thing that catches the class of bug whose symptom is rare unreproducible
 * blunders.
 *
 * One stack per searching thread, and thread-local rather than passed down because
 * eval_evaluate() is reached from the tuner, datagen and the UCI `eval` command as
 * well as from the search, and none of those has a search thread to hand it. It is
 * a POINTER, not a block: at 2 MB a stack, 128 threads' worth in static thread-local
 * storage is not something a thread creation should have to commit up front.
 */
typedef struct {
    _Alignas(64) int16_t acc[COLOR_NB][NNUE_MAX_HIDDEN];

    /* Key of the position this level describes; 0 until it describes one. */
    Key key;

    /* Per perspective, because a king that changes slot or crosses the mirror line
     * reindexes every feature ITS side sees and none of the other's. */
    bool computed[COLOR_NB];
} Accumulator;

/*
 * The layer stack's trunk, one per accumulator level.
 *
 * Both output heads read the same trunk, and unc_scale() in search.c asks for the
 * uncertainty at very nearly every node the value is asked at - so without this the
 * stack's dominant cost, L1, is paid twice per node.
 *
 * A PARALLEL STACK, not a field on Accumulator, and that is a measurement rather than a
 * preference: 256 bytes a level pushes consecutive accumulators apart and costs the
 * incremental update ~2% of nps on a net WITH NO STACK, which would hand a stacked net
 * that much free Elo in the SPRT meant to judge it. Allocated on first use, so a flat net
 * never allocates it and never touches it.
 *
 * Validity is the position key rather than a flag the push has to clear, which is what
 * keeps eval_state_push() free of this entirely: a level whose key matches was built from
 * accumulators describing that same position, since nnue_current() has already
 * established they describe it. `valid` covers the untouched level, whose key is zero.
 *
 * Per-thread, like the accumulator stack it shadows (invariant 11).
 */
typedef struct {
    _Alignas(64) int16_t trunk[NNUE_MAX_STACK_WIDTH];
    Key key;
    bool valid;
} TrunkLevel;

/* The search returns at ply >= MAX_PLY - 1 before making a move, so the deepest push is
 * shallower than this; the slack is deliberate. */
#define ACC_LEVELS (MAX_PLY + 2)

/*
 * The refresh cache.
 *
 * A king move that changes its side's slot or crosses the mirror line reindexes every
 * feature that side sees, so the incremental update has nothing to say about it and the
 * accumulator is rebuilt from the board. That rebuild is 32 rows drawn out of a 25 MB
 * table at scattered indices, and it measures about ten times what an ordinary two-row
 * delta costs. Six percent of a search's evaluations take it, and they are a fifth of
 * what the evaluation costs.
 *
 * So a rebuild starts from the last accumulator seen with the king on THAT square rather
 * than from the bias. The entry stores the pieces its accumulator was summed from, the
 * rebuild diffs the board against them, and a king stepping around its own corner of the
 * board pays for the few pieces that moved in between instead of for all of them.
 *
 * Correct by construction rather than by invalidation: the entry carries the exact
 * bitboards its accumulator sums, so an entry left over from some other part of the tree
 * is a bigger diff and never a wrong answer. The one thing it cannot see for itself is
 * the net being swapped under a running engine, where the same features carry different
 * weights - eval_state_clear() covers that, which is the same hook and the same reason
 * the accumulator stack is cleared there.
 *
 * Per-thread (invariant 11), and lazily allocated so a tool that evaluates one position
 * does not claim 128 accumulators to do it.
 */
typedef struct {
    Bitboard pieces[COLOR_NB][PIECE_TYPE_NB];
    bool valid;
} RefreshEntry;

/* One entry per king square per side. King square to (slot, mirror) is a bijection, so
 * this is exactly one entry per distinct perspective and never two that disagree. */
#define REFRESH_SLOTS (COLOR_NB * SQUARE_NB)

/* Past this many rows a diff has stopped being the cheaper answer: rebuilding from the
 * bias is 32 rows and bounded, while a diff against a distant position is neither. */
#define REFRESH_MAX_ROWS 24

static _Thread_local Accumulator *AccStack;
static _Thread_local TrunkLevel *TrunkStack;
static _Thread_local int AccTop;

/* Sized by the net rather than by NNUE_MAX_HIDDEN: 128 entries at the maximum width
 * would be half a megabyte a thread to hold a net four times narrower. RefreshWidth is
 * what a net swap to a different width is noticed by. */
static _Thread_local RefreshEntry *RefreshCache;
static _Thread_local int16_t *RefreshAcc;
static _Thread_local uint32_t RefreshWidth;
static _Thread_local bool RefreshFailed;

/* Set once a thread's allocation has failed, so a machine short of memory pays for one
 * failed calloc rather than one per evaluation. */
static _Thread_local bool AccStackFailed;

bool eval_state_alloc(void) {
    if (AccStack)
        return true;
    if (AccStackFailed)
        return false;

    AccStack = (Accumulator *)calloc(ACC_LEVELS, sizeof(Accumulator));
    if (!AccStack) {
        AccStackFailed = true;
        return false;
    }

    AccTop = 0;
    return true;
}

size_t eval_state_bytes(void) {
    return ACC_LEVELS * (sizeof(Accumulator) + (Loaded.hot.l1Size ? sizeof(TrunkLevel) : 0)) +
           REFRESH_SLOTS * (sizeof(RefreshEntry) + Loaded.hot.hidden * sizeof(int16_t));
}

void eval_state_free(void) {
    free(AccStack);
    free(TrunkStack);
    free(RefreshCache);
    free(RefreshAcc);
    AccStack       = NULL;
    TrunkStack     = NULL;
    RefreshCache   = NULL;
    RefreshAcc     = NULL;
    RefreshWidth   = 0;
    AccStackFailed = false;
    RefreshFailed  = false;
    AccTop         = 0;
}

typedef struct {
    Piece pc;
    Square sq;
} NnueFeature;

/* What a move changed, in features, plus which perspectives cannot express it as a delta
 * at all. Two of each is the worst case, and it is castling. */
typedef struct {
    NnueFeature added[2];
    NnueFeature removed[2];
    int addedCount;
    int removedCount;
    bool refresh[COLOR_NB];
} NnueDelta;

/* Whether `side` still indexes the board the same way after its king moves. Both fields,
 * not just the slot: a king stepping d1-e1 keeps slot 3 and gains the mirror, which
 * reindexes every square just as thoroughly. */
static inline bool nnue_king_reindexes(Color side, Square kingFrom, Square kingTo) {
    const Perspective before = nnue_perspective_of(side, kingFrom);
    const Perspective after  = nnue_perspective_of(side, kingTo);

    return before.slot != after.slot || before.mirror != after.mirror;
}

/* The features `m` changed, derived from the position AFTER it was played - the level
 * being pushed records the key of the position it describes, and that key exists only
 * once do_move has folded in the side to move, the castling rights and the en passant
 * square. Everything the delta needs survives the move. */
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
        /* En passant takes the pawn beside the destination, not on it, and the push
         * direction belongs to the mover. */
        const Square capsq            = mt == MT_EN_PASSANT ? (Square)(to - pawn_push(us)) : to;
        d->removed[d->removedCount++] = (NnueFeature){captured, capsq};
    }

    /* A promotion vacates `from` as a pawn and occupies `to` as something else, which is
     * the one move where the two features disagree. */
    const Piece vacated  = mt == MT_PROMOTION ? make_piece(us, PAWN) : piece_on(pos, to);
    const Piece occupied = piece_on(pos, to);

    d->removed[d->removedCount++] = (NnueFeature){vacated, from};
    d->added[d->addedCount++]     = (NnueFeature){occupied, to};

    if (type_of(vacated) == KING)
        d->refresh[us] = nnue_king_reindexes(us, from, to);
}

/* dst = src + the added rows - the removed rows, in one pass over the width. One pass
 * because the accumulator IS the memory traffic: separate add and subtract passes would
 * pay for dst repeatedly. */
static void nnue_apply_delta(const int16_t *src, int16_t *dst, const int16_t *const *add,
                             int addCount, const int16_t *const *sub, int subCount,
                             uint32_t hidden) {
    /* The two shapes that are nearly all of them get bodies with no inner loop at all: a
     * quiet move, one square vacated and one occupied, and a capture, which is that plus
     * the taken piece. The general body below reaches its rows through a pointer array the
     * compiler cannot unroll away, and a capture going through it costs about half again
     * what this does - which matters because move ordering tries captures first and the
     * quiescence search plays almost nothing else. */
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

/*
 * acc += the added rows, -= the removed ones, in place - what a refresh diff applies.
 *
 * Deliberately NOT a call to nnue_apply_delta, which does the same arithmetic. That one is
 * the per-node update, and it is hot enough to need inlining - which the compiler does
 * only while eval_state_push() is its single caller. Adding the refresh as a second
 * caller was measured at a quarter of what the whole update costs: GCC emitted one shared
 * out-of-line copy, and the per-node path lost its specialised one-row-each body to it.
 * The two callers want different code anyway - two rows against as many as
 * REFRESH_MAX_ROWS - so they get different functions.
 */
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

        /* Both destinations in the one pass. The entry has to end up holding what it now
         * describes and the caller has to end up with a copy, and doing that as a sum
         * followed by a memcpy reads the width twice to write it twice. */
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

/*
 * The refresh cache's backing store, claimed on the first rebuild that wants it.
 *
 * Reallocated rather than reused when the width changes, because `setoption EvalFile` can
 * name a net of a different shape and the entries are laid out by the old one.
 */
static bool nnue_refresh_alloc(void) {
    const uint32_t hidden = Loaded.hot.hidden;

    if (RefreshAcc && RefreshWidth == hidden)
        return true;
    if (RefreshFailed)
        return false;

    free(RefreshCache);
    free(RefreshAcc);
    RefreshCache = (RefreshEntry *)calloc(REFRESH_SLOTS, sizeof(RefreshEntry));
    RefreshAcc   = (int16_t *)calloc((size_t)REFRESH_SLOTS * hidden, sizeof(int16_t));

    if (!RefreshCache || !RefreshAcc) {
        free(RefreshCache);
        free(RefreshAcc);
        RefreshCache  = NULL;
        RefreshAcc    = NULL;
        RefreshWidth  = 0;
        RefreshFailed = true;
        return false;
    }

    RefreshWidth = hidden;
    return true;
}

/*
 * One perspective's accumulator, rebuilt from the nearest thing already computed for this
 * king square rather than from the bias.
 *
 * A failed allocation is not fatal, and neither is a diff that has grown past the point
 * of being worth it: both fall through to nnue_accumulate(), which is the same
 * from-scratch path this replaced and still the definition of the right answer. That is
 * also what the debug assert in eval_evaluate() compares against, so a cache that ever
 * disagreed with a full accumulation would fail the gate rather than lose Elo quietly.
 */
static void nnue_refresh(const Position *pos, Color c, int16_t *dst) {
    const Square king     = king_square(pos, c);
    const Perspective p   = nnue_perspective_of(c, king);
    const uint32_t hidden = Loaded.hot.hidden;

    if (!nnue_refresh_alloc()) {
        nnue_accumulate(pos, &p, dst);
        return;
    }

    const size_t idx      = (size_t)c * SQUARE_NB + (size_t)king;
    RefreshEntry *const e = &RefreshCache[idx];
    int16_t *const acc    = RefreshAcc + idx * hidden;

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

            while (gained)
                add[addCount++] = Loaded.hot.ftWeight +
                                  (size_t)nnue_feature_index(&p, pop_lsb(&gained), pc) * hidden;
            while (lost)
                sub[subCount++] = Loaded.hot.ftWeight +
                                  (size_t)nnue_feature_index(&p, pop_lsb(&lost), pc) * hidden;
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

void eval_state_clear(void) {
    AccTop = 0;
    if (AccStack)
        memset(&AccStack[0], 0, sizeof(AccStack[0]));

    /*
     * Everything below holds values the PREVIOUS net produced, and nothing about a key or
     * a bitboard says which net summed it - which is the whole reason this function is
     * called when `setoption EvalFile` swaps one in. A trunk carries the same hazard: its
     * level is validated by position key, and the root of the next search is very often
     * the position the last one ended at.
     */
    if (RefreshCache)
        memset(RefreshCache, 0, REFRESH_SLOTS * sizeof(RefreshEntry));
    if (TrunkStack)
        memset(TrunkStack, 0, ACC_LEVELS * sizeof(TrunkLevel));
}

/*
 * A thread with no stack skips the whole incremental path: push and pop become
 * nothing, and every evaluation accumulates from the board. That is slow and it is
 * CORRECT, which is the right way round for the one case that reaches it - a machine
 * that could not spare the allocation.
 */
void eval_state_push(const Position *pos, Move m) {
    if (!AccStack)
        return;

    assert(AccTop + 1 < ACC_LEVELS);

    const Accumulator *const parent = &AccStack[AccTop];
    Accumulator *const child        = &AccStack[++AccTop];

    /* The key the parent must be describing if its accumulator is to be worth carrying
     * forward: do_move recorded the pre-move key on the Undo. */
    const Key parentKey = pos->history[pos->gamePly - 1].key;

    child->key = pos->key;

    NnueDelta d;
    nnue_delta(pos, m, &d);

    const uint32_t hidden = Loaded.hot.hidden;

    for (Color c = WHITE; c <= BLACK; ++c) {
        /* Nothing to carry forward from a parent that was never computed or describes some
         * other position, and nothing a delta can say to a perspective that reindexed.
         * Either way the level is left for the next evaluation to rebuild. */
        if (d.refresh[c] || !parent->computed[c] || parent->key != parentKey) {
            child->computed[c] = false;
            continue;
        }

        /* Read off the CURRENT board, which is legitimate precisely because this branch has
         * established that `c` did not reindex - the same indices apply on both sides of
         * the move. */
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

/* A null move moves no piece, so both accumulators are already right and only the key
 * changed. The copy exists so the child level can carry that key: without a level of its
 * own, every node under a null move would find a mismatch and rebuild from scratch. */
void eval_state_push_null(const Position *pos) {
    if (!AccStack)
        return;

    assert(AccTop + 1 < ACC_LEVELS);

    const Accumulator *const parent = &AccStack[AccTop];
    Accumulator *const child        = &AccStack[++AccTop];
    const uint32_t hidden           = Loaded.hot.hidden;

    child->key = pos->key;

    for (Color c = WHITE; c <= BLACK; ++c) {
        child->computed[c] = parent->computed[c];
        if (parent->computed[c])
            memcpy(child->acc[c], parent->acc[c], hidden * sizeof(int16_t));
    }
}

void eval_state_pop(void) {
    if (!AccStack)
        return;

    assert(AccTop > 0);
    --AccTop;
}

/* The level describing the board, with any perspective that cannot be trusted rebuilt
 * from it - or NULL on a thread with no stack, which is a full recomputation. */
static const Accumulator *nnue_current(const Position *pos) {
    if (!AccStack && !eval_state_alloc())
        return NULL;

    Accumulator *const a = &AccStack[AccTop];

    if (a->key != pos->key) {
        a->key             = pos->key;
        a->computed[WHITE] = a->computed[BLACK] = false;
    }

    for (Color c = WHITE; c <= BLACK; ++c)
        if (!a->computed[c]) {
            nnue_refresh(pos, c, a->acc[c]);
            a->computed[c] = true;
        }

    return a;
}

/*
 * This level's trunk, built at most once.
 *
 * Returns NULL for a net with no stack, where there is no trunk and the heads read the
 * accumulators directly. The debug assert in eval_evaluate() is what proves the cache
 * agrees with a from-scratch computation, since that is exactly what it compares against.
 */
static const int16_t *nnue_cached_trunk(const Accumulator *a, const Position *pos, int bucket) {
    if (!Loaded.hot.l1Size)
        return NULL;

    /* Lazily, because the net can change under a running engine - `setoption EvalFile` -
     * and a thread that first searched with a flat net has no stack to shadow. A failed
     * allocation is not fatal: the heads fall back to building their own trunk, which is
     * the slow-and-correct path the from-scratch code already is. */
    if (!TrunkStack) {
        TrunkStack = (TrunkLevel *)calloc(ACC_LEVELS, sizeof(TrunkLevel));
        if (!TrunkStack)
            return NULL;
    }

    TrunkLevel *const t = &TrunkStack[AccTop];

    if (!t->valid || t->key != pos->key) {
        const Color stm = pos->sideToMove;

        nnue_stack_trunk(a->acc[stm], a->acc[stm ^ 1], bucket, t->trunk);
        t->key   = pos->key;
        t->valid = true;
    }
    return t->trunk;
}

/* This build's evaluation. eval.c defines the same symbol when EVAL_NNUE is not set, so
 * which one the engine runs costs nothing at runtime. */
Value eval_evaluate(const Position *pos) {
    const Accumulator *const a = nnue_current(pos);
    if (!a)
        return nnue_centipawns(nnue_raw(pos));

    const Color stm            = pos->sideToMove;
    const int bucket           = nnue_output_bucket(pos);
    const int16_t *const trunk = nnue_cached_trunk(a, pos, bucket);

    const int32_t raw =
        trunk ? nnue_trunk_head(trunk, Loaded.hot.outWeight, Loaded.hot.outBias, bucket)
              : nnue_output(a->acc[stm], a->acc[stm ^ 1], bucket);

    /* The gate on the entire incremental path. Cheap to state, expensive to omit: an
     * accumulator that drifts produces a legal-looking evaluation and surfaces only as
     * blunders nobody can reproduce. */
    assert(raw == nnue_raw(pos) && "incremental accumulator disagrees with a full recomputation");

    return nnue_centipawns(raw);
}

bool nnue_has_uncertainty(void) { return Loaded.hot.uncWeight != NULL; }

static int32_t nnue_unc_output(const int16_t *own, const int16_t *other, int bucket) {
    assert(Loaded.hot.uncWeight != NULL && "uncertainty asked of a net without the head");
    return nnue_head(own, other, Loaded.hot.uncWeight, Loaded.hot.uncBias, bucket);
}

/* The head predicts a magnitude, so the floor is zero rather than -NNUE_EVAL_LIMIT: a
 * negative prediction is the head saying "less error than I can express".
 * tools/export_net.py clamps identically. */
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

Value nnue_uncertainty(const Position *pos) {
    const Accumulator *const a = nnue_current(pos);
    if (!a) {
        _Alignas(64) int16_t acc[COLOR_NB][NNUE_MAX_HIDDEN];

        for (Color c = WHITE; c <= BLACK; ++c) {
            const Perspective p = nnue_perspective(pos, c);
            nnue_accumulate(pos, &p, acc[c]);
        }

        const Color stm = pos->sideToMove;
        return nnue_unc_centipawns(
            nnue_unc_output(acc[stm], acc[stm ^ 1], nnue_output_bucket(pos)));
    }

    const Color stm            = pos->sideToMove;
    const int bucket           = nnue_output_bucket(pos);
    const int16_t *const trunk = nnue_cached_trunk(a, pos, bucket);

    /* The trunk the value head just built, almost always: unc_scale() asks at very nearly
     * every node the evaluation is asked at, and building L1 twice for that was the whole
     * of the stack's avoidable cost. */
    if (trunk)
        return nnue_unc_centipawns(
            nnue_trunk_head(trunk, Loaded.hot.uncWeight, Loaded.hot.uncBias, bucket));

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

    /* Which inference path this binary took as well as which net it carries: an nps that
     * cannot be attributed to a build is as useless as a node count that cannot be
     * attributed to a net. */
    char stack[32] = "";
    if (h->l1Size) {
        if (h->l2Size)
            snprintf(stack, sizeof(stack), "->%u->%u", h->l1Size, h->l2Size);
        else
            snprintf(stack, sizeof(stack), "->%u", h->l1Size);
    }

    printf("info string net %.12s  %u->%ux2%s->%u%s  screlu halfka-32sq %s  qa %u qb %u "
           "scale %d  tag %s  from %s\n",
           Loaded.hash, h->features, h->hidden, stack, h->outputBuckets,
           Loaded.hot.uncWeight ? "+unc" : "",
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

    /* The net says whether every line carries the two uncertainty columns. The vectors
     * were written beside the net they describe, so a count that disagrees means these
     * vectors belong to a different net. */
    const bool wantUnc = nnue_has_uncertainty();

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
            /* Through the same accumulators the value just used, exactly as the reference
             * computes both heads from one activation. */
            const Accumulator *const a = nnue_current(&pos);
            const Color stm            = pos.sideToMove;

            uncRaw = nnue_unc_output(a->acc[stm], a->acc[stm ^ 1], nnue_output_bucket(&pos));
            uncCp  = nnue_unc_centipawns((int32_t)uncRaw);
            ok     = ok && uncRaw == expectedUncRaw && uncCp == expectedUncCp;
        }

        if (!ok) {
            /* Print the first handful and then stop counting out loud: a quantisation bug
             * fails every line, and ten thousand of them buries the one worth reading. */
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
