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
_Static_assert(sizeof(NnueHeader) == 96, "NnueHeader must stay 96 bytes; see HEADER_FMT in "
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

typedef struct {
    NnueHeader hdr;

    /* Pointers into the payload, which is either the embedded blob - read in place, never
     * copied - or `owned` below. */
    const int16_t *ftWeight;
    const int16_t *ftBias;
    const int16_t *outWeight;
    const int32_t *outBias;
    const int16_t *uncWeight;
    const int32_t *uncBias;

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

/* Bytes the payload must occupy for this header to be self-consistent. The uncertainty
 * head is a second output layer, so its flag adds exactly one more outWeight-and-outBias
 * worth - which is why an engine that never heard of the flag rejects on this count. */
static uint64_t nnue_payload_bytes(const NnueHeader *h) {
    const uint64_t headBytes = (uint64_t)h->outputBuckets * 2u * h->hidden * sizeof(int16_t) +
                               (uint64_t)h->outputBuckets * sizeof(int32_t);

    return (uint64_t)h->features * h->hidden * sizeof(int16_t) +
           (uint64_t)h->hidden * sizeof(int16_t) + headBytes * (h->reserved[0] == 1 ? 2u : 1u);
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
               "case to nnue_output() in src/nnue.c",
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

    if (h->qa == 0 || h->qb == 0 || h->scale == 0)
        REJECT("degenerate quantisation (qa %u, qb %u, scale %d)", h->qa, h->qb, h->scale);

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

    const int16_t *p = (const int16_t *)(const void *)(blob + sizeof(NnueHeader));
    Loaded.ftWeight  = p;
    p += (size_t)Loaded.hdr.features * Loaded.hdr.hidden;
    Loaded.ftBias = p;
    p += Loaded.hdr.hidden;
    Loaded.outWeight = p;
    p += (size_t)Loaded.hdr.outputBuckets * 2u * Loaded.hdr.hidden;
    Loaded.outBias = (const int32_t *)(const void *)p;

    if (Loaded.hdr.reserved[0] == 1) {
        p = (const int16_t *)(const void *)(Loaded.outBias + Loaded.hdr.outputBuckets);
        Loaded.uncWeight = p;
        p += (size_t)Loaded.hdr.outputBuckets * 2u * Loaded.hdr.hidden;
        Loaded.uncBias = (const int32_t *)(const void *)p;
    } else {
        Loaded.uncWeight = NULL;
        Loaded.uncBias   = NULL;
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
    const uint32_t hidden = Loaded.hdr.hidden;

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
        rows[count++]     = Loaded.ftWeight + (size_t)feature * hidden;
    }

    memcpy(acc, Loaded.ftBias, hidden * sizeof(int16_t));

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
    const uint32_t buckets = Loaded.hdr.outputBuckets;
    if (buckets == 1)
        return 0;

    const int bucket = (popcount(occupied_bb(pos)) - 2) / (int)(32u / buckets);
    return bucket < (int)buckets ? bucket : (int)buckets - 1;
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

/* SCReLU against one half of the output row. v * w stays in int16 - the exporter refuses
 * a net where it would not - and madd then widens (v * w) * v into int32 pairs, flushed
 * as the bound at the top of this file describes. */
static inline int64_t nnue_screlu_half(const int16_t *acc, const int16_t *w, uint32_t hidden,
                                       int32_t qa) {
    const __m256i zero = _mm256_setzero_si256();
    const __m256i top  = _mm256_set1_epi16((short)qa);

    int64_t total    = 0;
    __m256i lanes    = zero;
    uint32_t pending = 0;

    for (uint32_t j = 0; j < hidden; j += 16) {
        const __m256i a = _mm256_loadu_si256((const __m256i *)(const void *)(acc + j));
        const __m256i v = _mm256_min_epi16(_mm256_max_epi16(a, zero), top);
        const __m256i k = _mm256_loadu_si256((const __m256i *)(const void *)(w + j));

        lanes = _mm256_add_epi32(lanes, _mm256_madd_epi16(_mm256_mullo_epi16(v, k), v));

        if (++pending == NNUE_SCRELU_FLUSH) {
            total += nnue_hsum_epi32(lanes);
            lanes   = zero;
            pending = 0;
        }
    }
    return total + nnue_hsum_epi32(lanes);
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
static int32_t nnue_head(const int16_t *own, const int16_t *other, const int16_t *weights,
                         const int32_t *biases, int bucket) {
    const uint32_t hidden  = Loaded.hdr.hidden;
    const int32_t qa       = (int32_t)Loaded.hdr.qa;
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

/* The value head. The uncertainty head is the same arithmetic over its own weights. */
static inline int32_t nnue_output(const int16_t *own, const int16_t *other, int bucket) {
    return nnue_head(own, other, Loaded.outWeight, Loaded.outBias, bucket);
}

/* int64 because raw reaches several million and the scale is 400. The division TRUNCATES
 * toward zero, which tools/export_net.py reproduces explicitly: numpy's floor division
 * rounds the other way for negatives, and that asymmetry alone would fail about half the
 * test vectors. */
static Value nnue_centipawns(int32_t raw) {
    const int64_t num = (int64_t)raw * (int64_t)Loaded.hdr.scale;
    const int64_t den = (int64_t)Loaded.hdr.qa * (int64_t)Loaded.hdr.qb;
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
 * TODO(engine): Lazy SMP needs one stack per thread. It is file-scope for the same
 * reason search.c's ordering tables are, and it moves when they do.
 */
typedef struct {
    _Alignas(64) int16_t acc[COLOR_NB][NNUE_MAX_HIDDEN];

    /* Key of the position this level describes; 0 until it describes one. */
    Key key;

    /* Per perspective, because a king that changes slot or crosses the mirror line
     * reindexes every feature ITS side sees and none of the other's. */
    bool computed[COLOR_NB];
} Accumulator;

/* The search returns at ply >= MAX_PLY - 1 before making a move, so the deepest push is
 * shallower than this; the slack is deliberate. */
static Accumulator AccStack[MAX_PLY + 2];
static int AccTop;

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
    /* The overwhelmingly common shape - a quiet move, one square vacated and one occupied
     * - gets a body with no inner loop at all. */
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

void eval_state_clear(void) {
    AccTop = 0;
    memset(&AccStack[0], 0, sizeof(AccStack[0]));
}

void eval_state_push(const Position *pos, Move m) {
    assert(AccTop + 1 < (int)(sizeof(AccStack) / sizeof(AccStack[0])));

    const Accumulator *const parent = &AccStack[AccTop];
    Accumulator *const child        = &AccStack[++AccTop];

    /* The key the parent must be describing if its accumulator is to be worth carrying
     * forward: do_move recorded the pre-move key on the Undo. */
    const Key parentKey = pos->history[pos->gamePly - 1].key;

    child->key = pos->key;

    NnueDelta d;
    nnue_delta(pos, m, &d);

    const uint32_t hidden = Loaded.hdr.hidden;

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
            add[i] = Loaded.ftWeight +
                     (size_t)nnue_feature_index(&p, d.added[i].sq, d.added[i].pc) * hidden;
        for (int i = 0; i < d.removedCount; ++i)
            sub[i] = Loaded.ftWeight +
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
    assert(AccTop + 1 < (int)(sizeof(AccStack) / sizeof(AccStack[0])));

    const Accumulator *const parent = &AccStack[AccTop];
    Accumulator *const child        = &AccStack[++AccTop];
    const uint32_t hidden           = Loaded.hdr.hidden;

    child->key = pos->key;

    for (Color c = WHITE; c <= BLACK; ++c) {
        child->computed[c] = parent->computed[c];
        if (parent->computed[c])
            memcpy(child->acc[c], parent->acc[c], hidden * sizeof(int16_t));
    }
}

void eval_state_pop(void) {
    assert(AccTop > 0);
    --AccTop;
}

/* The level describing the board, with any perspective that cannot be trusted rebuilt
 * from it. */
static const Accumulator *nnue_current(const Position *pos) {
    Accumulator *const a = &AccStack[AccTop];

    if (a->key != pos->key) {
        a->key             = pos->key;
        a->computed[WHITE] = a->computed[BLACK] = false;
    }

    for (Color c = WHITE; c <= BLACK; ++c)
        if (!a->computed[c]) {
            const Perspective p = nnue_perspective(pos, c);
            nnue_accumulate(pos, &p, a->acc[c]);
            a->computed[c] = true;
        }

    return a;
}

/* This build's evaluation. eval.c defines the same symbol when EVAL_NNUE is not set, so
 * which one the engine runs costs nothing at runtime. */
Value eval_evaluate(const Position *pos) {
    const Accumulator *const a = nnue_current(pos);
    const Color stm            = pos->sideToMove;

    const int32_t raw = nnue_output(a->acc[stm], a->acc[stm ^ 1], nnue_output_bucket(pos));

    /* The gate on the entire incremental path. Cheap to state, expensive to omit: an
     * accumulator that drifts produces a legal-looking evaluation and surfaces only as
     * blunders nobody can reproduce. */
    assert(raw == nnue_raw(pos) && "incremental accumulator disagrees with a full recomputation");

    return nnue_centipawns(raw);
}

bool nnue_has_uncertainty(void) { return Loaded.uncWeight != NULL; }

static int32_t nnue_unc_output(const int16_t *own, const int16_t *other, int bucket) {
    assert(Loaded.uncWeight != NULL && "uncertainty asked of a net without the head");
    return nnue_head(own, other, Loaded.uncWeight, Loaded.uncBias, bucket);
}

/* The head predicts a magnitude, so the floor is zero rather than -NNUE_EVAL_LIMIT: a
 * negative prediction is the head saying "less error than I can express".
 * tools/export_net.py clamps identically. */
static Value nnue_unc_centipawns(int32_t raw) {
    const int64_t num = (int64_t)raw * (int64_t)Loaded.hdr.scale;
    const int64_t den = (int64_t)Loaded.hdr.qa * (int64_t)Loaded.hdr.qb;
    const int64_t cp  = num / den;

    if (cp < 0)
        return 0;
    if (cp > NNUE_EVAL_LIMIT)
        return NNUE_EVAL_LIMIT;
    return (Value)cp;
}

Value nnue_uncertainty(const Position *pos) {
    const Accumulator *const a = nnue_current(pos);
    const Color stm            = pos->sideToMove;

    return nnue_unc_centipawns(
        nnue_unc_output(a->acc[stm], a->acc[stm ^ 1], nnue_output_bucket(pos)));
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
    printf("info string net %.12s  %u->%ux2->%u%s  screlu halfka-32sq %s  qa %u qb %u "
           "scale %d  tag %s  from %s\n",
           Loaded.hash, h->features, h->hidden, h->outputBuckets, Loaded.uncWeight ? "+unc" : "",
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
