/*
 * syzygy.c - Syzygy endgame tablebase probing.
 *
 * Derived from Ronald de Man's reference implementation by way of Jon Dart's
 * Fathom (MIT); CREDITS.md records the relationship and retains the notice.
 * A file format is not something an implementation gets to choose, so the
 * constant tables below and the shape of the index and decompression code
 * follow it exactly. What is this engine's own is everything that touches a
 * position: placement reads our bitboards directly, capture resolution uses
 * our generator and our make/unmake, and nothing is marshalled through a
 * second board representation on the way in.
 *
 * WHAT A TABLE IS. A Syzygy file is an indexed, block-compressed array. A
 * position becomes an index by placing its men in a canonical order and
 * folding away the symmetries the format exploits - left-right always, and
 * up-down and diagonal reflection when there are no pawns. The index selects a
 * block; the block is a stream of re-pair-compressed symbols that expands to
 * the byte for that position. Three quantities, three places to be wrong, and
 * none of them announce themselves: a wrong prober returns a plausible number
 * for a plausible position. docs/EXPERIMENTS.md E24 is how this one was
 * checked.
 *
 * WDL answers won/drawn/lost and is what the search probes. DTZ answers "plies
 * to the next capture or pawn move that preserves the result" and is what
 * converts at the root under the fifty-move rule. There is no depth-to-mate
 * here because standard Syzygy carries none: DTM lives in .rtbm files that the
 * distributions do not ship.
 */
#include "syzygy.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bitboard.h"
#include "movegen.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

/* ------------------------------------------------------------- constants -- */

enum { TB_PIECES = 7, TB_HASHBITS = 12, TB_MAX_SYMS = 4096 };

/* How many distinct endgames exist up to TB_PIECES men, counted rather than
 * guessed: 254/256 at six men, 650/861 at seven. These MUST match TB_PIECES -
 * carrying the six-man numbers with a seven-man enumeration silently drops
 * every seven-man table on the floor, because the guards in init_tb return
 * quietly when an array is full and MaxPieces then never reaches 7. */
enum { TB_MAX_PIECE_TABLES = 650, TB_MAX_PAWN_TABLES = 861 };

/* The two table kinds this engine reads. The format defines a third, .rtbm
 * (depth to mate), which the distributions do not ship and nothing here asks
 * for. */
enum { TB_WDL = 0, TB_DTZ = 1 };
static const char *const TbSuffix[2] = {".rtbw", ".rtbz"};
static const uint32_t TbMagic[2]     = {0x5d23e871u, 0xa50c66d7u};

/* How a position becomes an index. Pawnless material folds on both diagonals;
 * pawns break the up-down and diagonal symmetries, so those tables are split
 * by the file of the leading pawn instead. */
enum { PIECE_ENC = 0, FILE_ENC = 1 };

/*
 * Material keys. Each coloured piece type gets a prime and a position's key is
 * the sum over its men, so two positions share a key exactly when they share
 * material. The values are the format's: a key has to agree with the one the
 * table was built under.
 */
#define PRIME_WHITE_QUEEN  11811845319353239651ull
#define PRIME_WHITE_ROOK   10979190538029446137ull
#define PRIME_WHITE_BISHOP 12311744257139811149ull
#define PRIME_WHITE_KNIGHT 15202887380319082783ull
#define PRIME_WHITE_PAWN   17008651141875982339ull
#define PRIME_BLACK_QUEEN  15484752644942473553ull
#define PRIME_BLACK_ROOK   18264461213049635989ull
#define PRIME_BLACK_BISHOP 15394650811035483107ull
#define PRIME_BLACK_KNIGHT 13469005675588064321ull
#define PRIME_BLACK_PAWN   11695583624105689831ull

/* clang-format off */

static const int8_t OffDiag[] = {
  0,-1,-1,-1,-1,-1,-1,-1,
  1, 0,-1,-1,-1,-1,-1,-1,
  1, 1, 0,-1,-1,-1,-1,-1,
  1, 1, 1, 0,-1,-1,-1,-1,
  1, 1, 1, 1, 0,-1,-1,-1,
  1, 1, 1, 1, 1, 0,-1,-1,
  1, 1, 1, 1, 1, 1, 0,-1,
  1, 1, 1, 1, 1, 1, 1, 0
};

static const uint8_t Triangle[] = {
  6, 0, 1, 2, 2, 1, 0, 6,
  0, 7, 3, 4, 4, 3, 7, 0,
  1, 3, 8, 5, 5, 8, 3, 1,
  2, 4, 5, 9, 9, 5, 4, 2,
  2, 4, 5, 9, 9, 5, 4, 2,
  1, 3, 8, 5, 5, 8, 3, 1,
  0, 7, 3, 4, 4, 3, 7, 0,
  6, 0, 1, 2, 2, 1, 0, 6
};

static const uint8_t FlipDiag[] = {
   0,  8, 16, 24, 32, 40, 48, 56,
   1,  9, 17, 25, 33, 41, 49, 57,
   2, 10, 18, 26, 34, 42, 50, 58,
   3, 11, 19, 27, 35, 43, 51, 59,
   4, 12, 20, 28, 36, 44, 52, 60,
   5, 13, 21, 29, 37, 45, 53, 61,
   6, 14, 22, 30, 38, 46, 54, 62,
   7, 15, 23, 31, 39, 47, 55, 63
};

static const uint8_t Lower[] = {
  28,  0,  1,  2,  3,  4,  5,  6,
   0, 29,  7,  8,  9, 10, 11, 12,
   1,  7, 30, 13, 14, 15, 16, 17,
   2,  8, 13, 31, 18, 19, 20, 21,
   3,  9, 14, 18, 32, 22, 23, 24,
   4, 10, 15, 19, 22, 33, 25, 26,
   5, 11, 16, 20, 23, 25, 34, 27,
   6, 12, 17, 21, 24, 26, 27, 35
};

static const uint8_t Diag[] = {
   0,  0,  0,  0,  0,  0,  0,  8,
   0,  1,  0,  0,  0,  0,  9,  0,
   0,  0,  2,  0,  0, 10,  0,  0,
   0,  0,  0,  3, 11,  0,  0,  0,
   0,  0,  0, 12,  4,  0,  0,  0,
   0,  0, 13,  0,  0,  5,  0,  0,
   0, 14,  0,  0,  0,  0,  6,  0,
  15,  0,  0,  0,  0,  0,  0,  7
};

static const uint8_t Flap[2][64] = {
  {  0,  0,  0,  0,  0,  0,  0,  0,
     0,  6, 12, 18, 18, 12,  6,  0,
     1,  7, 13, 19, 19, 13,  7,  1,
     2,  8, 14, 20, 20, 14,  8,  2,
     3,  9, 15, 21, 21, 15,  9,  3,
     4, 10, 16, 22, 22, 16, 10,  4,
     5, 11, 17, 23, 23, 17, 11,  5,
     0,  0,  0,  0,  0,  0,  0,  0 },
  {  0,  0,  0,  0,  0,  0,  0,  0,
     0,  1,  2,  3,  3,  2,  1,  0,
     4,  5,  6,  7,  7,  6,  5,  4,
     8,  9, 10, 11, 11, 10,  9,  8,
    12, 13, 14, 15, 15, 14, 13, 12,
    16, 17, 18, 19, 19, 18, 17, 16,
    20, 21, 22, 23, 23, 22, 21, 20,
     0,  0,  0,  0,  0,  0,  0,  0 }
};

static const uint8_t PawnTwist[2][64] = {
  {  0,  0,  0,  0,  0,  0,  0,  0,
    47, 35, 23, 11, 10, 22, 34, 46,
    45, 33, 21,  9,  8, 20, 32, 44,
    43, 31, 19,  7,  6, 18, 30, 42,
    41, 29, 17,  5,  4, 16, 28, 40,
    39, 27, 15,  3,  2, 14, 26, 38,
    37, 25, 13,  1,  0, 12, 24, 36,
     0,  0,  0,  0,  0,  0,  0,  0 },
  {  0,  0,  0,  0,  0,  0,  0,  0,
    47, 45, 43, 41, 40, 42, 44, 46,
    39, 37, 35, 33, 32, 34, 36, 38,
    31, 29, 27, 25, 24, 26, 28, 30,
    23, 21, 19, 17, 16, 18, 20, 22,
    15, 13, 11,  9,  8, 10, 12, 14,
     7,  5,  3,  1,  0,  2,  4,  6,
     0,  0,  0,  0,  0,  0,  0,  0 }
};

static const int16_t KKIdx[10][64] = {
  { -1, -1, -1,  0,  1,  2,  3,  4,
    -1, -1, -1,  5,  6,  7,  8,  9,
    10, 11, 12, 13, 14, 15, 16, 17,
    18, 19, 20, 21, 22, 23, 24, 25,
    26, 27, 28, 29, 30, 31, 32, 33,
    34, 35, 36, 37, 38, 39, 40, 41,
    42, 43, 44, 45, 46, 47, 48, 49,
    50, 51, 52, 53, 54, 55, 56, 57 },
  { 58, -1, -1, -1, 59, 60, 61, 62,
    63, -1, -1, -1, 64, 65, 66, 67,
    68, 69, 70, 71, 72, 73, 74, 75,
    76, 77, 78, 79, 80, 81, 82, 83,
    84, 85, 86, 87, 88, 89, 90, 91,
    92, 93, 94, 95, 96, 97, 98, 99,
   100,101,102,103,104,105,106,107,
   108,109,110,111,112,113,114,115},
  {116,117, -1, -1, -1,118,119,120,
   121,122, -1, -1, -1,123,124,125,
   126,127,128,129,130,131,132,133,
   134,135,136,137,138,139,140,141,
   142,143,144,145,146,147,148,149,
   150,151,152,153,154,155,156,157,
   158,159,160,161,162,163,164,165,
   166,167,168,169,170,171,172,173 },
  {174, -1, -1, -1,175,176,177,178,
   179, -1, -1, -1,180,181,182,183,
   184, -1, -1, -1,185,186,187,188,
   189,190,191,192,193,194,195,196,
   197,198,199,200,201,202,203,204,
   205,206,207,208,209,210,211,212,
   213,214,215,216,217,218,219,220,
   221,222,223,224,225,226,227,228 },
  {229,230, -1, -1, -1,231,232,233,
   234,235, -1, -1, -1,236,237,238,
   239,240, -1, -1, -1,241,242,243,
   244,245,246,247,248,249,250,251,
   252,253,254,255,256,257,258,259,
   260,261,262,263,264,265,266,267,
   268,269,270,271,272,273,274,275,
   276,277,278,279,280,281,282,283 },
  {284,285,286,287,288,289,290,291,
   292,293, -1, -1, -1,294,295,296,
   297,298, -1, -1, -1,299,300,301,
   302,303, -1, -1, -1,304,305,306,
   307,308,309,310,311,312,313,314,
   315,316,317,318,319,320,321,322,
   323,324,325,326,327,328,329,330,
   331,332,333,334,335,336,337,338 },
  { -1, -1,339,340,341,342,343,344,
    -1, -1,345,346,347,348,349,350,
    -1, -1,441,351,352,353,354,355,
    -1, -1, -1,442,356,357,358,359,
    -1, -1, -1, -1,443,360,361,362,
    -1, -1, -1, -1, -1,444,363,364,
    -1, -1, -1, -1, -1, -1,445,365,
    -1, -1, -1, -1, -1, -1, -1,446 },
  { -1, -1, -1,366,367,368,369,370,
    -1, -1, -1,371,372,373,374,375,
    -1, -1, -1,376,377,378,379,380,
    -1, -1, -1,447,381,382,383,384,
    -1, -1, -1, -1,448,385,386,387,
    -1, -1, -1, -1, -1,449,388,389,
    -1, -1, -1, -1, -1, -1,450,390,
    -1, -1, -1, -1, -1, -1, -1,451 },
  {452,391,392,393,394,395,396,397,
    -1, -1, -1, -1,398,399,400,401,
    -1, -1, -1, -1,402,403,404,405,
    -1, -1, -1, -1,406,407,408,409,
    -1, -1, -1, -1,453,410,411,412,
    -1, -1, -1, -1, -1,454,413,414,
    -1, -1, -1, -1, -1, -1,455,415,
    -1, -1, -1, -1, -1, -1, -1,456 },
  {457,416,417,418,419,420,421,422,
    -1,458,423,424,425,426,427,428,
    -1, -1, -1, -1, -1,429,430,431,
    -1, -1, -1, -1, -1,432,433,434,
    -1, -1, -1, -1, -1,435,436,437,
    -1, -1, -1, -1, -1,459,438,439,
    -1, -1, -1, -1, -1, -1,460,440,
    -1, -1, -1, -1, -1, -1, -1,461 }
};

/* clang-format on */

static const uint8_t FileToFile[] = {0, 1, 2, 3, 3, 2, 1, 0};
static const int WdlToMap[5]      = {1, 3, 0, 2, 0};
static const uint8_t PAFlags[5]   = {8, 0, 0, 0, 4};
static const int WdlToDtz[5]      = {-1, -101, 0, 101, 1};

static size_t Binomial[7][64];
static size_t PawnIdx[6][24];
static size_t PawnFactorFile[6][4];

/* ------------------------------------------------------------ byte order -- */

/* Tables are little-endian on disk; the compressed stream is read big-endian.
 * Both are a load and possibly a bswap, never bytes assembled by hand - no
 * compiler reliably folds that back into a load. */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define TB_BIG_ENDIAN 1
#else
#define TB_BIG_ENDIAN 0
#endif

static inline uint16_t bswap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

static inline uint32_t bswap32(uint32_t v) {
    return ((v >> 24) & 0xFFu) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) |
           ((v << 24) & 0xFF000000u);
}

static inline uint64_t bswap64(uint64_t v) {
    return ((uint64_t)bswap32((uint32_t)v) << 32) | bswap32((uint32_t)(v >> 32));
}

static inline uint16_t read_le16(const void *p) {
    uint16_t v;
    memcpy(&v, p, sizeof(v));
    return TB_BIG_ENDIAN ? bswap16(v) : v;
}

static inline uint32_t read_le32(const void *p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return TB_BIG_ENDIAN ? bswap32(v) : v;
}

static inline uint64_t read_be64(const void *p) {
    uint64_t v;
    memcpy(&v, p, sizeof(v));
    return TB_BIG_ENDIAN ? v : bswap64(v);
}

static inline uint32_t read_be32(const void *p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return TB_BIG_ENDIAN ? v : bswap32(v);
}

/* --------------------------------------------------------------- mapping -- */

#if defined(_WIN32)
typedef HANDLE TbFile;
typedef HANDLE TbMap;
#define TB_FILE_NONE INVALID_HANDLE_VALUE
#else
typedef int TbFile;
typedef size_t TbMap;
#define TB_FILE_NONE (-1)
#endif

enum { TB_MAX_PATHS = 16, TB_PATH_CAP = 4096 };
static char PathBuffer[TB_PATH_CAP];
static const char *Paths[TB_MAX_PATHS];
static int NumPaths;

static TbFile open_tb(const char *name, const char *suffix) {
    char file[TB_PATH_CAP];

    for (int i = 0; i < NumPaths; ++i) {
        snprintf(file, sizeof(file), "%s/%s%s", Paths[i], name, suffix);
#if defined(_WIN32)
        const TbFile fd = CreateFileA(file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, NULL);
#else
        const TbFile fd = open(file, O_RDONLY);
#endif
        if (fd != TB_FILE_NONE)
            return fd;
    }
    return TB_FILE_NONE;
}

static void close_tb(TbFile fd) {
#if defined(_WIN32)
    CloseHandle(fd);
#else
    close(fd);
#endif
}

/*
 * A complete table is a multiple of 64 bytes plus a 16-byte header. A
 * truncated one is the failure this exists for: it maps and probes perfectly
 * well and answers nonsense for the positions whose blocks are missing.
 */
static bool table_present(const char *name, const char *suffix) {
    const TbFile fd = open_tb(name, suffix);
    if (fd == TB_FILE_NONE)
        return false;

    bool ok;
#if defined(_WIN32)
    LARGE_INTEGER size;
    ok = GetFileSizeEx(fd, &size) && ((uint64_t)size.QuadPart & 63) == 16;
#else
    struct stat st;
    ok = fstat(fd, &st) == 0 && ((uint64_t)st.st_size & 63) == 16;
#endif
    if (!ok)
        fprintf(stderr, "syzygy: %s%s is truncated, ignoring it\n", name, suffix);
    close_tb(fd);
    return ok;
}

static void *map_tb(const char *name, const char *suffix, TbMap *mapping) {
    const TbFile fd = open_tb(name, suffix);
    if (fd == TB_FILE_NONE)
        return NULL;

#if defined(_WIN32)
    DWORD hi        = 0;
    const DWORD lo  = GetFileSize(fd, &hi);
    const HANDLE mh = CreateFileMapping(fd, NULL, PAGE_READONLY, hi, lo, NULL);
    close_tb(fd);
    if (mh == NULL)
        return NULL;
    void *data = MapViewOfFile(mh, FILE_MAP_READ, 0, 0, 0);
    if (data == NULL) {
        CloseHandle(mh);
        return NULL;
    }
    *mapping = mh;
#else
    struct stat st;
    if (fstat(fd, &st)) {
        close_tb(fd);
        return NULL;
    }
    void *data = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    close_tb(fd);
    if (data == MAP_FAILED)
        return NULL;
    *mapping = (size_t)st.st_size;
#ifdef POSIX_MADV_RANDOM
    /* Probes land on unrelated blocks; read-ahead only evicts what is worth
     * keeping. */
    posix_madvise(data, (size_t)st.st_size, POSIX_MADV_RANDOM);
#endif
#endif
    return data;
}

static void unmap_tb(void *data, TbMap mapping) {
    if (!data)
        return;
#if defined(_WIN32)
    UnmapViewOfFile(data);
    CloseHandle(mapping);
#else
    munmap(data, mapping);
#endif
}

/* ---------------------------------------------------------------- tables -- */

typedef struct {
    const uint8_t *indexTable;
    const uint16_t *sizeTable;
    const uint8_t *data;
    const uint16_t *offset;
    uint8_t *symLen;
    const uint8_t *symPat;
    uint8_t blockSize;
    uint8_t idxBits;
    uint8_t minLen;
    uint8_t constValue[2];
    uint64_t base[1]; /* h entries; allocated with the struct */
} PairsData;

typedef struct {
    PairsData *precomp;
    size_t factor[TB_PIECES];
    uint8_t pieces[TB_PIECES];
    uint8_t norm[TB_PIECES];
} EncInfo;

typedef struct {
    uint64_t key;
    uint8_t *data[2];
    TbMap mapping[2];
    bool ready[2];
    /* Latched when init_table() rejects this table, so a corrupt file is
     * mapped and parsed once rather than on every probe. It lives here rather
     * than on the hash slot because hash_add() gives an endgame TWO slots (its
     * key and the mirrored key2): a marker on one slot leaves the other able
     * to re-enter init_table, which re-maps the file and leaks the first
     * mapping along with the PairsData the failed attempt allocated. */
    bool failed[2];
    uint8_t num;
    bool symmetric, hasPawns, hasDtz;
    union {
        bool kk_enc; /* pawnless: exactly two men are unique */
        uint8_t pawns[2];
    };
} BaseEntry;

/* The ei layout, with no gaps for the depth-to-mate tables this does not read:
 *   pawnless  [0] wdl white, [1] wdl black, [2] dtz
 *   pawnful   [0..7] wdl (file + 4*black), [8..11] dtz (file) */
typedef struct {
    BaseEntry be;
    EncInfo ei[3];
    const void *dtzMap;
    uint16_t dtzMapIdx[4];
    uint8_t dtzFlags;
} PieceEntry;

typedef struct {
    BaseEntry be;
    EncInfo ei[12];
    const void *dtzMap;
    uint16_t dtzMapIdx[4][4];
    uint8_t dtzFlags[4];
} PawnEntry;

typedef struct {
    uint64_t key;
    BaseEntry *ptr;
} HashEntry;

static PieceEntry *PieceEntries;
static PawnEntry *PawnEntries;
static HashEntry TbHash[1 << TB_HASHBITS];
static int NumPieceEntries, NumPawnEntries;
static int NumWdl, NumDtz;
static int MaxPieces;

#define AS_PIECE(x) ((PieceEntry *)(x))
#define AS_PAWN(x)  ((PawnEntry *)(x))

static int num_tables(const BaseEntry *be) { return be->hasPawns ? 4 : 1; }

static EncInfo *first_ei(BaseEntry *be, int type) {
    if (be->hasPawns)
        return &AS_PAWN(be)->ei[type == TB_WDL ? 0 : 8];
    return &AS_PIECE(be)->ei[type == TB_WDL ? 0 : 2];
}

/* ------------------------------------------------------------------ keys -- */

static uint64_t material_key(const Position *pos, bool mirror) {
    const Bitboard w = color_bb(pos, mirror ? BLACK : WHITE);
    const Bitboard b = color_bb(pos, mirror ? WHITE : BLACK);

    return (uint64_t)popcount(w & pos->byType[QUEEN]) * PRIME_WHITE_QUEEN +
           (uint64_t)popcount(w & pos->byType[ROOK]) * PRIME_WHITE_ROOK +
           (uint64_t)popcount(w & pos->byType[BISHOP]) * PRIME_WHITE_BISHOP +
           (uint64_t)popcount(w & pos->byType[KNIGHT]) * PRIME_WHITE_KNIGHT +
           (uint64_t)popcount(w & pos->byType[PAWN]) * PRIME_WHITE_PAWN +
           (uint64_t)popcount(b & pos->byType[QUEEN]) * PRIME_BLACK_QUEEN +
           (uint64_t)popcount(b & pos->byType[ROOK]) * PRIME_BLACK_ROOK +
           (uint64_t)popcount(b & pos->byType[BISHOP]) * PRIME_BLACK_BISHOP +
           (uint64_t)popcount(b & pos->byType[KNIGHT]) * PRIME_BLACK_KNIGHT +
           (uint64_t)popcount(b & pos->byType[PAWN]) * PRIME_BLACK_PAWN;
}

/* The same sum from a piece-count array, for the names discovered at load. */
static uint64_t key_from_counts(const int *pcs, bool mirror) {
    static const uint64_t W[5] = {PRIME_WHITE_PAWN, PRIME_WHITE_KNIGHT, PRIME_WHITE_BISHOP,
                                  PRIME_WHITE_ROOK, PRIME_WHITE_QUEEN};
    static const uint64_t B[5] = {PRIME_BLACK_PAWN, PRIME_BLACK_KNIGHT, PRIME_BLACK_BISHOP,
                                  PRIME_BLACK_ROOK, PRIME_BLACK_QUEEN};

    uint64_t key = 0;
    for (int t = 0; t < 5; ++t) {
        key += (uint64_t)pcs[(mirror ? 8 : 0) + t + 1] * W[t];
        key += (uint64_t)pcs[(mirror ? 0 : 8) + t + 1] * B[t];
    }
    return key;
}

/* The table's name, "KQPvKR", from the side the caller names. */
static void table_name(const Position *pos, char *str, bool flip) {
    static const char Chars[] = " PNBRQK";
    Color c                   = flip ? BLACK : WHITE;

    for (int side = 0; side < 2; ++side) {
        for (int pt = KING; pt >= PAWN; --pt)
            for (int i = popcount(pieces_bb(pos, c, (PieceType)pt)); i > 0; --i)
                *str++ = Chars[pt];
        if (side == 0)
            *str++ = 'v';
        c = c == WHITE ? BLACK : WHITE;
    }
    *str = 0;
}

/* --------------------------------------------------------------- indices -- */

static void init_indices(void) {
    for (int i = 0; i < 7; ++i)
        for (int j = 0; j < 64; ++j) {
            size_t f = 1, l = 1;
            for (int k = 0; k < i; ++k) {
                f *= (size_t)(j - k);
                l *= (size_t)(k + 1);
            }
            Binomial[i][j] = f / l;
        }

    for (int i = 0; i < 6; ++i) {
        size_t s = 0;
        for (int j = 0; j < 24; ++j) {
            PawnIdx[i][j] = s;
            s += Binomial[i][PawnTwist[0][(1 + (j % 6)) * 8 + (j / 6)]];
            if ((j + 1) % 6 == 0) {
                PawnFactorFile[i][j / 6] = s;
                s                        = 0;
            }
        }
    }
}

static inline void swap_int(int *a, int *b) {
    const int t = *a;
    *a          = *b;
    *b          = t;
}

static int leading_pawn(int *p, const BaseEntry *be) {
    for (int i = 1; i < be->pawns[0]; ++i)
        if (Flap[0][p[0]] > Flap[0][p[i]])
            swap_int(&p[0], &p[i]);
    return FileToFile[p[0] & 7];
}

/*
 * Position -> index.
 *
 * The squares in `p` are folded onto the canonical region the table was built
 * over - left-right always, and for pawnless material also up-down and along
 * the main diagonal. What is left is a mixed-radix number whose digits are
 * "which combination of like men", which the factors turn into an offset.
 */
static size_t encode(int *p, const EncInfo *ei, const BaseEntry *be, int enc) {
    const int n = be->num;
    size_t idx;
    int k;

    if (p[0] & 0x04)
        for (int i = 0; i < n; ++i)
            p[i] ^= 0x07;

    if (enc == PIECE_ENC) {
        if (p[0] & 0x20)
            for (int i = 0; i < n; ++i)
                p[i] ^= 0x38;

        for (int i = 0; i < n; ++i)
            if (OffDiag[p[i]]) {
                if (OffDiag[p[i]] > 0 && i < (be->kk_enc ? 2 : 3))
                    for (int j = 0; j < n; ++j)
                        p[j] = FlipDiag[p[j]];
                break;
            }

        if (be->kk_enc) {
            idx = (size_t)KKIdx[Triangle[p[0]]][p[1]];
            k   = 2;
        } else {
            const int s1 = (p[1] > p[0]);
            const int s2 = (p[2] > p[0]) + (p[2] > p[1]);

            if (OffDiag[p[0]])
                idx = (size_t)(Triangle[p[0]] * 63 * 62 + (p[1] - s1) * 62 + (p[2] - s2));
            else if (OffDiag[p[1]])
                idx = (size_t)(6 * 63 * 62 + Diag[p[0]] * 28 * 62 + Lower[p[1]] * 62 + p[2] - s2);
            else if (OffDiag[p[2]])
                idx = (size_t)(6 * 63 * 62 + 4 * 28 * 62 + Diag[p[0]] * 7 * 28 +
                               (Diag[p[1]] - s1) * 28 + Lower[p[2]]);
            else
                idx = (size_t)(6 * 63 * 62 + 4 * 28 * 62 + 4 * 7 * 28 + Diag[p[0]] * 7 * 6 +
                               (Diag[p[1]] - s1) * 6 + (Diag[p[2]] - s2));
            k = 3;
        }
        idx *= ei->factor[0];
    } else {
        for (int i = 1; i < be->pawns[0]; ++i)
            for (int j = i + 1; j < be->pawns[0]; ++j)
                if (PawnTwist[0][p[i]] < PawnTwist[0][p[j]])
                    swap_int(&p[i], &p[j]);

        k   = be->pawns[0];
        idx = PawnIdx[k - 1][Flap[0][p[0]]];
        for (int i = 1; i < k; ++i)
            idx += Binomial[k - i][PawnTwist[0][p[i]]];
        idx *= ei->factor[0];

        if (be->pawns[1]) {
            const int t = k + be->pawns[1];
            for (int i = k; i < t; ++i)
                for (int j = i + 1; j < t; ++j)
                    if (p[i] > p[j])
                        swap_int(&p[i], &p[j]);
            size_t s = 0;
            for (int i = k; i < t; ++i) {
                const int sq = p[i];
                int skips    = 0;
                for (int j = 0; j < k; ++j)
                    skips += (sq > p[j]);
                s += Binomial[i - k + 1][sq - skips - 8];
            }
            idx += s * ei->factor[k];
            k = t;
        }
    }

    while (k < n) {
        const int t = k + ei->norm[k];
        for (int i = k; i < t; ++i)
            for (int j = i + 1; j < t; ++j)
                if (p[i] > p[j])
                    swap_int(&p[i], &p[j]);
        size_t s = 0;
        for (int i = k; i < t; ++i) {
            const int sq = p[i];
            int skips    = 0;
            for (int j = 0; j < k; ++j)
                skips += (sq > p[j]);
            s += Binomial[i - k + 1][sq - skips];
        }
        idx += s * ei->factor[k];
        k = t;
    }

    return idx;
}

/* Placements of k like men on n squares. */
static size_t subfactor(size_t k, size_t n) {
    size_t f = n, l = 1;
    for (size_t i = 1; i < k; ++i) {
        f *= n - i;
        l *= i + 1;
    }
    return f / l;
}

static size_t init_enc_info(EncInfo *ei, BaseEntry *be, const uint8_t *tb, int shift, int t,
                            int enc) {
    const bool morePawns = enc != PIECE_ENC && be->pawns[1] > 0;

    for (int i = 0; i < be->num; ++i) {
        ei->pieces[i] = (uint8_t)((tb[i + 1 + morePawns] >> shift) & 0x0f);
        ei->norm[i]   = 0;
    }

    const int order  = (tb[0] >> shift) & 0x0f;
    const int order2 = morePawns ? (tb[1] >> shift) & 0x0f : 0x0f;

    int k = ei->norm[0] = (uint8_t)(enc != PIECE_ENC ? be->pawns[0] : be->kk_enc ? 2 : 3);

    if (morePawns) {
        ei->norm[k] = be->pawns[1];
        k += ei->norm[k];
    }

    for (int i = k; i < be->num; i += ei->norm[i])
        for (int j = i; j < be->num && ei->pieces[j] == ei->pieces[i]; ++j)
            ei->norm[i]++;

    int n    = 64 - k;
    size_t f = 1;

    for (int i = 0; k < be->num || i == order || i == order2; ++i) {
        if (i == order) {
            ei->factor[0] = f;
            f *= enc == FILE_ENC ? PawnFactorFile[ei->norm[0] - 1][t] : be->kk_enc ? 462 : 31332;
        } else if (i == order2) {
            ei->factor[ei->norm[0]] = f;
            f *= subfactor(ei->norm[ei->norm[0]], 48 - ei->norm[0]);
        } else {
            ei->factor[k] = f;
            f *= subfactor(ei->norm[k], (size_t)n);
            n -= ei->norm[k];
            k += ei->norm[k];
        }
    }

    return f;
}

/* ---------------------------------------------------------- decompression -- */

static void calc_sym_len(PairsData *d, uint32_t s, char *tmp) {
    const uint8_t *w  = d->symPat + 3 * s;
    const uint32_t s2 = ((uint32_t)w[2] << 4) | (w[1] >> 4);

    if (s2 == 0x0fff) {
        d->symLen[s] = 0;
    } else {
        const uint32_t s1 = (uint32_t)((w[1] & 0xf) << 8) | w[0];
        if (!tmp[s1])
            calc_sym_len(d, s1, tmp);
        if (!tmp[s2])
            calc_sym_len(d, s2, tmp);
        d->symLen[s] = (uint8_t)(d->symLen[s1] + d->symLen[s2] + 1);
    }
    tmp[s] = 1;
}

static PairsData *setup_pairs(uint8_t **ptr, size_t tbSize, size_t *size, uint8_t *flags,
                              int type) {
    PairsData *d;
    uint8_t *data = *ptr;

    *flags = data[0];

    /* A table whose every entry is the same value stores only that value. */
    if (data[0] & 0x80) {
        d = (PairsData *)malloc(sizeof(PairsData));
        if (!d)
            return NULL;
        d->idxBits       = 0;
        d->constValue[0] = type == TB_WDL ? data[1] : 0;
        d->constValue[1] = 0;
        *ptr             = data + 2;
        size[0] = size[1] = size[2] = 0;
        return d;
    }

    const uint8_t blockSize      = data[1];
    const uint8_t idxBits        = data[2];
    const uint32_t realNumBlocks = read_le32(data + 4);
    const uint32_t numBlocks     = realNumBlocks + data[3];
    const int maxLen             = data[8];
    const int minLen             = data[9];
    const int h                  = maxLen - minLen + 1;
    const uint32_t numSyms       = read_le16(data + 10 + 2 * h);

    if (numSyms >= TB_MAX_SYMS)
        return NULL;

    d = (PairsData *)malloc(sizeof(PairsData) + (size_t)h * sizeof(uint64_t) + numSyms);
    if (!d)
        return NULL;

    d->blockSize = blockSize;
    d->idxBits   = idxBits;
    d->offset    = (const uint16_t *)(const void *)&data[10];
    d->symLen    = (uint8_t *)d + sizeof(PairsData) + (size_t)h * sizeof(uint64_t);
    d->symPat    = &data[12 + 2 * h];
    d->minLen    = (uint8_t)minLen;
    *ptr         = &data[12 + 2 * h + 3 * numSyms + (numSyms & 1)];

    const size_t numIndices = (tbSize + (1ULL << idxBits) - 1) >> idxBits;
    size[0]                 = 6ULL * numIndices;
    size[1]                 = 2ULL * numBlocks;
    size[2]                 = (size_t)realNumBlocks << blockSize;

    char tmp[TB_MAX_SYMS];
    memset(tmp, 0, numSyms);
    for (uint32_t s = 0; s < numSyms; ++s)
        if (!tmp[s])
            calc_sym_len(d, s, tmp);

    d->base[h - 1] = 0;
    for (int i = h - 2; i >= 0; --i)
        d->base[i] = (d->base[i + 1] + read_le16(&d->offset[i]) - read_le16(&d->offset[i + 1])) / 2;
    for (int i = 0; i < h; ++i)
        d->base[i] <<= 64 - (minLen + i);

    d->offset -= d->minLen;
    return d;
}

/*
 * idx -> the symbol's bytes.
 *
 * The index table names a block and an offset inside it; the block is a bit
 * stream of canonical-Huffman codes over symbols that each expand to a run of
 * bytes. So the walk is: skip whole symbols until the offset falls inside one,
 * then descend that symbol's pair tree to the byte.
 */
static const uint8_t *decompress_pairs(const PairsData *d, size_t idx) {
    if (!d->idxBits)
        return d->constValue;

    const uint32_t mainIdx = (uint32_t)(idx >> d->idxBits);
    int litIdx =
        (int)(idx & (((size_t)1 << d->idxBits) - 1)) - (int)((size_t)1 << (d->idxBits - 1));

    uint32_t block = read_le32(d->indexTable + 6 * mainIdx);
    /* Unsigned, as the format defines it. Read as signed this goes negative,
     * the block walk below runs off the front of sizeTable, and `--block`
     * underflows a uint32 into a four-billion-entry read. */
    litIdx += (int)read_le16(d->indexTable + 6 * mainIdx + 4);

    if (litIdx < 0)
        while (litIdx < 0)
            litIdx += d->sizeTable[--block] + 1;
    else
        while (litIdx > d->sizeTable[block])
            litIdx -= d->sizeTable[block++] + 1;

    const uint32_t *ptr =
        (const uint32_t *)(const void *)(d->data + ((size_t)block << d->blockSize));

    const int m            = d->minLen;
    const uint16_t *offset = d->offset;
    const uint64_t *base   = d->base - m;
    const uint8_t *symLen  = d->symLen;
    uint32_t sym;

    uint64_t code   = read_be64(ptr);
    uint32_t bitCnt = 0; /* empty bits at the bottom of `code` */
    ptr += 2;

    for (;;) {
        int l = m;
        while (code < base[l])
            ++l;
        sym = read_le16(&offset[l]);
        sym += (uint32_t)((code - base[l]) >> (64 - l));
        if (litIdx < (int)symLen[sym] + 1)
            break;
        litIdx -= (int)symLen[sym] + 1;
        code <<= l;
        bitCnt += (uint32_t)l;
        if (bitCnt >= 32) {
            bitCnt -= 32;
            code |= (uint64_t)read_be32(ptr++) << bitCnt;
        }
    }

    const uint8_t *symPat = d->symPat;
    while (symLen[sym] != 0) {
        const uint8_t *w  = symPat + 3 * sym;
        const uint32_t s1 = (uint32_t)((w[1] & 0xf) << 8) | w[0];
        if (litIdx < (int)symLen[s1] + 1) {
            sym = s1;
        } else {
            litIdx -= (int)symLen[s1] + 1;
            sym = ((uint32_t)w[2] << 4) | (w[1] >> 4);
        }
    }

    return &symPat[3 * sym];
}

/* ----------------------------------------------------------- table setup -- */

static bool init_table(BaseEntry *be, const char *name, int type) {
    uint8_t *data = (uint8_t *)map_tb(name, TbSuffix[type], &be->mapping[type]);
    if (!data)
        return false;

    if (read_le32(data) != TbMagic[type]) {
        fprintf(stderr, "syzygy: %s%s has the wrong magic, ignoring it\n", name, TbSuffix[type]);
        unmap_tb(data, be->mapping[type]);
        return false;
    }

    be->data[type] = data;

    const bool split = type != TB_DTZ && (data[4] & 0x01);
    data += 5;

    size_t tbSize[4][2];
    const int num = num_tables(be);
    EncInfo *ei   = first_ei(be, type);
    const int enc = be->hasPawns ? FILE_ENC : PIECE_ENC;

    for (int t = 0; t < num; ++t) {
        tbSize[t][0] = init_enc_info(&ei[t], be, data, 0, t, enc);
        if (split)
            tbSize[t][1] = init_enc_info(&ei[num + t], be, data, 4, t, enc);
        data += be->num + 1 + (be->hasPawns && be->pawns[1]);
    }
    data += (uintptr_t)data & 1;

    size_t size[4][2][3];
    for (int t = 0; t < num; ++t) {
        uint8_t flags;
        ei[t].precomp = setup_pairs(&data, tbSize[t][0], size[t][0], &flags, type);
        if (!ei[t].precomp)
            return false;
        if (type == TB_DTZ) {
            if (!be->hasPawns)
                AS_PIECE(be)->dtzFlags = flags;
            else
                AS_PAWN(be)->dtzFlags[t] = flags;
        }
        if (split) {
            ei[num + t].precomp = setup_pairs(&data, tbSize[t][1], size[t][1], &flags, type);
            if (!ei[num + t].precomp)
                return false;
        } else if (type != TB_DTZ) {
            ei[num + t].precomp = NULL;
        }
    }

    if (type == TB_DTZ) {
        const void *map      = data;
        uint16_t(*mapIdx)[4] = be->hasPawns ? &AS_PAWN(be)->dtzMapIdx[0] : &AS_PIECE(be)->dtzMapIdx;
        const uint8_t *flags = be->hasPawns ? &AS_PAWN(be)->dtzFlags[0] : &AS_PIECE(be)->dtzFlags;

        if (be->hasPawns)
            AS_PAWN(be)->dtzMap = map;
        else
            AS_PIECE(be)->dtzMap = map;

        for (int t = 0; t < num; ++t) {
            if (!(flags[t] & 2))
                continue;
            if (!(flags[t] & 16)) {
                for (int i = 0; i < 4; ++i) {
                    mapIdx[t][i] = (uint16_t)(data + 1 - (const uint8_t *)map);
                    data += 1 + data[0];
                }
            } else {
                data += (uintptr_t)data & 1;
                for (int i = 0; i < 4; ++i) {
                    mapIdx[t][i] = (uint16_t)((const uint16_t *)(const void *)data + 1 -
                                              (const uint16_t *)map);
                    data += 2 + 2 * read_le16(data);
                }
            }
        }
        data += (uintptr_t)data & 1;
    }

    for (int t = 0; t < num; ++t) {
        ei[t].precomp->indexTable = data;
        data += size[t][0][0];
        if (split) {
            ei[num + t].precomp->indexTable = data;
            data += size[t][1][0];
        }
    }

    for (int t = 0; t < num; ++t) {
        ei[t].precomp->sizeTable = (const uint16_t *)(const void *)data;
        data += size[t][0][1];
        if (split) {
            ei[num + t].precomp->sizeTable = (const uint16_t *)(const void *)data;
            data += size[t][1][1];
        }
    }

    for (int t = 0; t < num; ++t) {
        data                = (uint8_t *)(((uintptr_t)data + 0x3f) & ~(uintptr_t)0x3f);
        ei[t].precomp->data = data;
        data += size[t][0][2];
        if (split) {
            data                      = (uint8_t *)(((uintptr_t)data + 0x3f) & ~(uintptr_t)0x3f);
            ei[num + t].precomp->data = data;
            data += size[t][1][2];
        }
    }

    return true;
}

static void hash_add(BaseEntry *be, uint64_t key) {
    int idx = (int)(key >> (64 - TB_HASHBITS));
    while (TbHash[idx].ptr)
        idx = (idx + 1) & ((1 << TB_HASHBITS) - 1);
    TbHash[idx].key = key;
    TbHash[idx].ptr = be;
}

/* Piece letters strongest first: within a side the format always names the
 * strongest man first - KNPvK, never KPNvK. */
static const char GenLetters[5] = {'Q', 'R', 'B', 'N', 'P'};
static int GenW[6], GenB[6];

static void init_tb(const char *name);

static void emit_name(int wn, int bn) {
    char name[16];
    size_t n = 0;

    name[n++] = 'K';
    for (int i = 0; i < wn; ++i)
        name[n++] = GenLetters[GenW[i]];
    name[n++] = 'v';
    name[n++] = 'K';
    for (int i = 0; i < bn; ++i)
        name[n++] = GenLetters[GenB[i]];
    name[n] = 0;

    init_tb(name);
}

/*
 * Every endgame name, by brute force over both sides.
 *
 * Which SIDE the format names first is a rule this deliberately does not try
 * to reproduce - it is roughly "more men first, then by strength", with enough
 * exceptions (KBBvKQ, but KQvKP) that deriving it is a way to be subtly wrong
 * for one table and never notice. Both orderings are offered and the one whose
 * file exists is the one that registers, so the filesystem settles it. The
 * cost is a few thousand failed opens, once, at load.
 */
static void gen_black(int wn, int bn, int i, int start) {
    if (i == bn) {
        emit_name(wn, bn);
        return;
    }
    for (int t = start; t < 5; ++t) {
        GenB[i] = t;
        gen_black(wn, bn, i + 1, t);
    }
}

static void gen_white(int wn, int bn, int i, int start) {
    if (i == wn) {
        gen_black(wn, bn, 0, 0);
        return;
    }
    for (int t = start; t < 5; ++t) {
        GenW[i] = t;
        gen_white(wn, bn, i + 1, t);
    }
}

/* Registers one endgame, by name, if its WDL table is on disk. */
static void init_tb(const char *name) {
    if (!table_present(name, TbSuffix[TB_WDL]))
        return;

    int pcs[16];
    memset(pcs, 0, sizeof(pcs));

    int colour = 0;
    for (const char *s = name; *s; ++s) {
        switch (*s) {
        case 'v': colour = 8; break;
        case 'P': pcs[colour + PAWN]++; break;
        case 'N': pcs[colour + KNIGHT]++; break;
        case 'B': pcs[colour + BISHOP]++; break;
        case 'R': pcs[colour + ROOK]++; break;
        case 'Q': pcs[colour + QUEEN]++; break;
        case 'K': pcs[colour + KING]++; break;
        default: break;
        }
    }

    const uint64_t key  = key_from_counts(pcs, false);
    const uint64_t key2 = key_from_counts(pcs, true);
    const bool hasPawns = pcs[PAWN] || pcs[8 + PAWN];

    /* Loudly, not quietly: a table that exists on disk and is refused here is
     * a hole in the engine's endgame knowledge that nothing downstream would
     * report. The caps are sized for TB_PIECES above, so this is unreachable
     * unless that sizing is wrong. */
    BaseEntry *be;
    if (hasPawns) {
        if (NumPawnEntries >= TB_MAX_PAWN_TABLES) {
            fprintf(stderr,
                    "syzygy: no room for %s (%d pawnful tables); TB_MAX_PAWN_TABLES is "
                    "too small for TB_PIECES=%d\n",
                    name, NumPawnEntries, TB_PIECES);
            return;
        }
        be = &PawnEntries[NumPawnEntries++].be;
    } else {
        if (NumPieceEntries >= TB_MAX_PIECE_TABLES) {
            fprintf(stderr,
                    "syzygy: no room for %s (%d pawnless tables); TB_MAX_PIECE_TABLES is "
                    "too small for TB_PIECES=%d\n",
                    name, NumPieceEntries, TB_PIECES);
            return;
        }
        be = &PieceEntries[NumPieceEntries++].be;
    }

    be->hasPawns  = hasPawns;
    be->key       = key;
    be->symmetric = key == key2;
    be->num       = 0;
    for (int i = 0; i < 16; ++i)
        be->num = (uint8_t)(be->num + pcs[i]);

    be->ready[TB_WDL]  = false;
    be->ready[TB_DTZ]  = false;
    be->failed[TB_WDL] = false;
    be->failed[TB_DTZ] = false;
    be->data[TB_WDL]   = NULL;
    be->data[TB_DTZ]   = NULL;

    ++NumWdl;
    be->hasDtz = table_present(name, TbSuffix[TB_DTZ]);
    NumDtz += be->hasDtz;

    if (be->num > MaxPieces)
        MaxPieces = be->num;

    if (!hasPawns) {
        int unique = 0;
        for (int i = 0; i < 16; ++i)
            if (pcs[i] == 1)
                ++unique;
        be->kk_enc = unique == 2;
    } else {
        be->pawns[0] = (uint8_t)pcs[PAWN];
        be->pawns[1] = (uint8_t)pcs[8 + PAWN];
        if (pcs[8 + PAWN] && (!pcs[PAWN] || pcs[PAWN] > pcs[8 + PAWN])) {
            const uint8_t t = be->pawns[0];
            be->pawns[0]    = be->pawns[1];
            be->pawns[1]    = t;
        }
    }

    hash_add(be, key);
    if (key != key2)
        hash_add(be, key2);
}

/* ---------------------------------------------------------------- probing -- */

/*
 * The squares of every man, grouped in the piece order the table expects.
 * `flip` swaps the colours (the table may be stored from the other side's
 * point of view) and `mirror` reflects vertically for pawnful material.
 *
 * Reading our own bitboards here is the single largest difference from a
 * general-purpose prober: there is no second board representation to fill in
 * before a probe can start.
 */
static int fill_squares(const Position *pos, const uint8_t *pc, bool flip, int mirror, int *p,
                        int i) {
    /* The format codes a man as `type | colour << 3` with 1 = pawn, which is
     * this engine's own layout - hence nothing is translated here. */
    Color c = (Color)((pc[i] >> 3) & 1);
    if (flip)
        c = c == WHITE ? BLACK : WHITE;

    Bitboard bb = pieces_bb(pos, c, (PieceType)(pc[i] & 7));
    do {
        p[i++] = (int)lsb(bb) ^ mirror;
        bb &= bb - 1;
    } while (bb);
    return i;
}

/*
 * The raw table lookup: WDL as -2..2, DTZ as a distance.
 *
 * `success` comes back 0 when the position cannot be probed at all, and -1
 * when a DTZ table stores only the other side's half - which the caller
 * answers by searching a ply and asking again.
 */
static int probe_table(const Position *pos, int s, int *success, int type) {
    const uint64_t key = material_key(pos, false);

    /* Bare kings are not a table, and are drawn. */
    if (type == TB_WDL && key == 0)
        return 0;

    int hashIdx = (int)(key >> (64 - TB_HASHBITS));
    while (TbHash[hashIdx].key && TbHash[hashIdx].key != key)
        hashIdx = (hashIdx + 1) & ((1 << TB_HASHBITS) - 1);
    if (!TbHash[hashIdx].ptr) {
        *success = 0;
        return 0;
    }

    BaseEntry *be = TbHash[hashIdx].ptr;
    if (be->failed[type]) {
        *success = 0;
        return 0;
    }

    if (type == TB_DTZ && !be->hasDtz) {
        *success = 0;
        return 0;
    }

    /* Mapped on first use rather than at startup: a generation touches a
     * handful of endgames, and mapping all 290 would be address space spent on
     * tables nobody asks for. */
    if (!be->ready[type]) {
        char name[16];
        table_name(pos, name, be->key != key);
        if (!init_table(be, name, type)) {
            /*
             * Every other rejection in this file names itself; this one used
             * to be the exception, and a table that silently answers "not
             * probable" forever is indistinguishable from one that is simply
             * absent. `failed` latches, so this prints once per endgame.
             */
            printf("info string syzygy: %s%s is unusable and will not be probed again\n", name,
                   TbSuffix[type]);
            fflush(stdout);
            be->failed[type] = true;
            *success         = 0;
            return 0;
        }
        be->ready[type] = true;
    }

    bool bside, flip;
    if (!be->symmetric) {
        flip  = key != be->key;
        bside = (pos->sideToMove == WHITE) == flip;
    } else {
        flip  = pos->sideToMove != WHITE;
        bside = false;
    }

    EncInfo *ei = first_ei(be, type);
    /* Zeroed because fill_squares only writes the men this table has, and the
     * compiler cannot see that encode reads exactly those. Seven ints. */
    int p[TB_PIECES] = {0};
    size_t idx;
    int t         = 0;
    uint8_t flags = 0;

    if (!be->hasPawns) {
        if (type == TB_DTZ) {
            flags = AS_PIECE(be)->dtzFlags;
            if ((flags & 1) != bside && !be->symmetric) {
                *success = -1;
                return 0;
            }
        }
        ei = type != TB_DTZ ? &ei[bside] : ei;
        for (int i = 0; i < be->num;)
            i = fill_squares(pos, ei->pieces, flip, 0, p, i);
        idx = encode(p, ei, be, PIECE_ENC);
    } else {
        int i = fill_squares(pos, ei->pieces, flip, flip ? 0x38 : 0, p, 0);
        t     = leading_pawn(p, be);
        if (type == TB_DTZ) {
            flags = AS_PAWN(be)->dtzFlags[t];
            if ((flags & 1) != bside && !be->symmetric) {
                *success = -1;
                return 0;
            }
        }
        ei = type == TB_WDL ? &ei[t + 4 * bside] : &ei[t];
        while (i < be->num)
            i = fill_squares(pos, ei->pieces, flip, flip ? 0x38 : 0, p, i);
        idx = encode(p, ei, be, FILE_ENC);
    }

    const uint8_t *w = decompress_pairs(ei->precomp, idx);

    if (type == TB_WDL)
        return (int)w[0] - 2;

    int v = w[0] + ((w[1] & 0x0f) << 8);

    if (flags & 2) {
        const int m        = WdlToMap[s + 2];
        const uint16_t *mi = be->hasPawns ? AS_PAWN(be)->dtzMapIdx[t] : AS_PIECE(be)->dtzMapIdx;
        const void *map    = be->hasPawns ? AS_PAWN(be)->dtzMap : AS_PIECE(be)->dtzMap;
        if (!(flags & 16))
            v = ((const uint8_t *)map)[mi[m] + v];
        else
            v = (int)read_le16(&((const uint16_t *)map)[mi[m] + v]);
    }

    if (!(flags & PAFlags[s + 2]) || (s & 1))
        v *= 2;

    return v;
}

/* ----------------------------------------------------- capture resolution -- */

/* A capture in the format's sense: the destination is occupied, or it is an en
 * passant capture. A promotion counts only when it takes something. */
static inline bool is_capture_move(const Position *pos, Move m) {
    return piece_on(pos, to_sq(m)) != NO_PIECE || type_of_move(m) == MT_EN_PASSANT;
}

static bool has_legal_move(Position *pos) {
    ScoredMove list[MAX_MOVES];
    const int n = movegen_generate(pos, board_checkers(pos) ? GEN_EVASIONS : GEN_ALL, list);
    for (int i = 0; i < n; ++i)
        if (movegen_is_legal(pos, list[i].m))
            return true;
    return false;
}

/*
 * Every capture, resolved down to a position a table actually holds.
 *
 * A WDL table only stores positions with the fifty-move counter at zero, so a
 * position with captures available is not in any table: its value is the best
 * of its captures against the table value of the position with none. That is
 * why a WDL "probe" is a small search rather than a lookup.
 */
static int probe_ab(Position *pos, int alpha, int beta, int *success) {
    ScoredMove list[MAX_MOVES];
    /* GEN_CAPTURES, not GEN_ALL: this loop wants captures and throws every
     * quiet move away, and generating them was a quarter of the probe. The
     * set is exactly right - it holds every capture including capturing
     * underpromotions, plus quiet queen promotions that the filter below
     * drops. In check the evasion set is the only safe one, and it is small. */
    const int count =
        movegen_generate(pos, board_checkers(pos) ? GEN_EVASIONS : GEN_CAPTURES, list);

    for (int i = 0; i < count; ++i) {
        const Move m = list[i].m;
        if (!is_capture_move(pos, m) || !movegen_is_legal(pos, m))
            continue;

        board_do_move(pos, m);
        const int v = -probe_ab(pos, -beta, -alpha, success);
        board_undo_move(pos, m);

        if (*success == 0)
            return 0;
        if (v > alpha) {
            if (v >= beta)
                return v;
            alpha = v;
        }
    }

    const int v = probe_table(pos, 0, success, TB_WDL);
    return alpha >= v ? alpha : v;
}

/*
 * WDL from the side to move: -2 loss, -1 cursed loss, 0 draw, 1 cursed win,
 * 2 win. `*success` comes back 2 when a capture forces the value, which
 * probe_dtz needs in order to know the distance without another table read.
 *
 * En passant is the awkward case throughout: the tables know nothing of it, so
 * an ep capture's value has to be weighed against the position as the table
 * sees it, and the stalemate branch at the end is where the two genuinely
 * disagree.
 */
static int probe_wdl_captures(Position *pos, int *success) {
    *success = 1;

    ScoredMove list[MAX_MOVES];
    const int count =
        movegen_generate(pos, board_checkers(pos) ? GEN_EVASIONS : GEN_CAPTURES, list);

    int bestCap = -3, bestEp = -3;

    for (int i = 0; i < count; ++i) {
        const Move m = list[i].m;
        if (!is_capture_move(pos, m) || !movegen_is_legal(pos, m))
            continue;

        board_do_move(pos, m);
        const int v = -probe_ab(pos, -2, -bestCap, success);
        board_undo_move(pos, m);

        if (*success == 0)
            return 0;
        if (v > bestCap) {
            if (v == 2) {
                *success = 2;
                return 2;
            }
            if (type_of_move(m) != MT_EN_PASSANT)
                bestCap = v;
            else if (v > bestEp)
                bestEp = v;
        }
    }

    const int v = probe_table(pos, 0, success, TB_WDL);
    if (*success == 0)
        return 0;

    if (bestEp > bestCap) {
        if (bestEp > v) {
            *success = 2;
            return bestEp;
        }
        bestCap = bestEp;
    }

    if (bestCap >= v) {
        *success = 1 + (bestCap > 0);
        return bestCap;
    }

    /* The table said draw, but it was asked about the position WITHOUT en
     * passant rights. If that position is stalemate and an ep capture exists,
     * the ep capture is the only move and its value is the position's. */
    if (bestEp > -3 && v == 0) {
        /* Regenerated in full, deliberately: this asks whether the position is
         * STALEMATE but for the en passant capture, and the list above holds
         * only captures. Reusing it would call every quiet position stalemate. */
        ScoredMove all[MAX_MOVES];
        const int n = movegen_generate(pos, board_checkers(pos) ? GEN_EVASIONS : GEN_ALL, all);

        bool haveNonEp = false;
        for (int i = 0; i < n && !haveNonEp; ++i)
            haveNonEp = type_of_move(all[i].m) != MT_EN_PASSANT && movegen_is_legal(pos, all[i].m);

        if (!haveNonEp && board_checkers(pos) == BB_EMPTY) {
            *success = 2;
            return bestEp;
        }
    }

    return v;
}

/*
 * DTZ from the side to move, in the format's units: 0 draw, 1..100 a win in n
 * plies from a zero counter, over 100 a win the fifty-move rule takes away,
 * and the mirror for losses. May be off by one, which is why the root compares
 * distances rather than trusting one.
 */
static int probe_dtz(Position *pos, int *success) {
    const int wdl = probe_wdl_captures(pos, success);
    if (*success == 0 || wdl == 0)
        return 0;

    /* Forced by a capture, so the distance is a single ply. */
    if (*success == 2)
        return WdlToDtz[wdl + 2];

    ScoredMove list[MAX_MOVES];
    int count = 0;

    if (wdl > 0) {
        /* A pawn move zeroes the counter, so if one holds the win the distance
         * is one ply and no DTZ read is needed at all. */
        count = movegen_generate(pos, board_checkers(pos) ? GEN_EVASIONS : GEN_ALL, list);
        for (int i = 0; i < count; ++i) {
            const Move m = list[i].m;
            if (type_of(piece_on(pos, from_sq(m))) != PAWN || is_capture_move(pos, m) ||
                !movegen_is_legal(pos, m))
                continue;

            board_do_move(pos, m);
            const int v = -probe_wdl_captures(pos, success);
            board_undo_move(pos, m);

            if (*success == 0)
                return 0;
            if (v == wdl)
                return WdlToDtz[wdl + 2];
        }
    }

    const int dtz = probe_table(pos, wdl, success, TB_DTZ);
    if (*success >= 0)
        return WdlToDtz[wdl + 2] + ((wdl > 0) ? dtz : -dtz);

    /* The table holds only the other side's half, so the distance has to come
     * from searching one ply and asking again. */
    int best;
    if (wdl > 0) {
        best = INT32_MAX;
    } else {
        /* The worst case is already a losing capture or pawn move, which is
         * what this seeds; in a mate it leaves -1. */
        best  = WdlToDtz[wdl + 2];
        count = movegen_generate(pos, board_checkers(pos) ? GEN_EVASIONS : GEN_ALL, list);
    }

    for (int i = 0; i < count; ++i) {
        const Move m = list[i].m;
        /* Captures and pawn moves zero the counter and are already accounted
         * for - by the scan above when winning, by `best` when losing. */
        if (is_capture_move(pos, m) || type_of(piece_on(pos, from_sq(m))) == PAWN ||
            !movegen_is_legal(pos, m))
            continue;

        board_do_move(pos, m);
        const int v = -probe_dtz(pos, success);
        /* Mate in one is distance one, not the zero a drawn child would say. */
        const bool childMated = (v == 1) && board_checkers(pos) != BB_EMPTY && !has_legal_move(pos);
        board_undo_move(pos, m);

        if (*success == 0)
            return 0;

        if (childMated)
            best = 1;
        else if (wdl > 0) {
            if (v > 0 && v + 1 < best)
                best = v + 1;
        } else {
            if (v - 1 < best)
                best = v - 1;
        }
    }

    /*
     * The winning seed is INT32_MAX, and every path that should replace it
     * needs a quiet non-pawn move that preserves the win. That move must exist
     * for a genuine win whose distance did not already come from the capture
     * or pawn scan above - but "must" is an argument, not a check, and the
     * caller does `dtz + halfmoveClock`, which overflows on the sentinel and
     * reports a proven win for an undetermined position. Decline instead.
     */
    if (best == INT32_MAX) {
        *success = 0;
        return 0;
    }

    return best;
}

/* ------------------------------------------------------------ public API -- */

bool syzygy_init(const char *path) {
    syzygy_free();

    if (path == NULL || *path == '\0')
        return false;

    /* Split once and held: open_tb walks this list for every name it looks
     * for. */
    snprintf(PathBuffer, sizeof(PathBuffer), "%s", path);
#if defined(_WIN32)
    const char sep = ';';
#else
    const char sep = ':';
#endif
    NumPaths     = 0;
    char *cursor = PathBuffer;
    while (cursor && *cursor && NumPaths < TB_MAX_PATHS) {
        Paths[NumPaths++] = cursor;
        char *next        = strchr(cursor, sep);
        if (!next)
            break;
        *next  = '\0';
        cursor = next + 1;
    }
    if (NumPaths == 0)
        return false;

    init_indices();

    PieceEntries = (PieceEntry *)calloc(TB_MAX_PIECE_TABLES, sizeof(PieceEntry));
    PawnEntries  = (PawnEntry *)calloc(TB_MAX_PAWN_TABLES, sizeof(PawnEntry));
    if (!PieceEntries || !PawnEntries) {
        syzygy_free();
        return false;
    }
    memset(TbHash, 0, sizeof(TbHash));

    /*
     * Up to seven men, which is every table the format defines. A name whose
     * file is absent simply does not register.
     *
     * Six men and up only on a 64-bit size_t: `encode`'s index products
     * overflow 32 bits from six men onward, and the result is not a failure but
     * a wrong index into a valid table - a plausible answer for the wrong
     * position. Fathom refuses the same way and for the same reason.
     */
    const int menLimit = sizeof(size_t) >= 8 ? 5 : 3;
    for (int extra = 1; extra <= menLimit; ++extra)
        for (int wn = 0; wn <= extra; ++wn)
            gen_white(wn, extra - wn, 0, 0);

    if (MaxPieces == 0) {
        syzygy_free();
        return false;
    }

    return true;
}

void syzygy_free(void) {
    for (int i = 0; i < NumPieceEntries; ++i) {
        for (int type = 0; type < 2; ++type)
            if (PieceEntries[i].be.data[type])
                unmap_tb(PieceEntries[i].be.data[type], PieceEntries[i].be.mapping[type]);
        for (int k = 0; k < 3; ++k)
            free(PieceEntries[i].ei[k].precomp);
    }
    for (int i = 0; i < NumPawnEntries; ++i) {
        for (int type = 0; type < 2; ++type)
            if (PawnEntries[i].be.data[type])
                unmap_tb(PawnEntries[i].be.data[type], PawnEntries[i].be.mapping[type]);
        for (int k = 0; k < 12; ++k)
            free(PawnEntries[i].ei[k].precomp);
    }

    free(PieceEntries);
    free(PawnEntries);
    PieceEntries    = NULL;
    PawnEntries     = NULL;
    NumPieceEntries = NumPawnEntries = 0;
    NumWdl = NumDtz = 0;
    MaxPieces       = 0;
    NumPaths        = 0;
    memset(TbHash, 0, sizeof(TbHash));
}

int syzygy_max_pieces(void) { return MaxPieces; }

/*
 * Probing makes moves on the caller's position and takes them back rather than
 * working on a copy: a Position carries its whole repetition history and runs
 * to tens of kilobytes, so copying one per probe would cost far more than the
 * lookup it protects. The board is restored exactly.
 */
static bool probe_guard(const Position *pos) {
    /* Capture resolution recurses at most once per man and probe_dtz adds a
     * quiet ply on top; refuse rather than run off the end of the history. */
    return MaxPieces != 0 && pos->castling == NO_CASTLING &&
           pos->gamePly + TB_PIECES + 2 < MAX_GAME_PLY && popcount(occupied_bb(pos)) <= MaxPieces;
}

Value syzygy_probe_wdl(Position *pos, int ply) {
    if (!probe_guard(pos) || pos->halfmoveClock != 0)
        return VALUE_NONE;

    int success   = 0;
    const int wdl = probe_wdl_captures(pos, &success);
    if (!success)
        return VALUE_NONE;

    /* Cursed wins and blessed losses are draws under the fifty-move rule,
     * which this engine plays by. */
    if (wdl > 1)
        return VALUE_TB_WIN - ply;
    if (wdl < -1)
        return -VALUE_TB_WIN + ply;
    return VALUE_DRAW;
}

SyzygyRoot syzygy_probe_root(Position *pos) {
    SyzygyRoot out = {MOVE_NONE, VALUE_NONE, 0};

    if (!probe_guard(pos))
        return out;

    Position *board = pos;

    int success   = 0;
    const int dtz = probe_dtz(board, &success);
    if (!success)
        return out;

    out.dtz = dtz;

    /* The fifty-move-aware result: a win whose distance plus the counter
     * already on the board runs past 100 is drawn, however won the table says
     * it is. */
    const int clock = pos->halfmoveClock;
    if (dtz > 0)
        out.value = (dtz + clock <= 100) ? VALUE_TB_WIN : VALUE_DRAW;
    else if (dtz < 0)
        out.value = (-dtz + clock <= 100) ? -VALUE_TB_WIN : VALUE_DRAW;
    else
        out.value = VALUE_DRAW;

    /*
     * The move: among the legal ones, the child that keeps the result and is
     * nearest to zeroing. Taken from the generator's own list, so nothing the
     * generator did not produce can ever reach the board.
     */
    ScoredMove list[MAX_MOVES];
    const int count = movegen_generate(board, board_checkers(board) ? GEN_EVASIONS : GEN_ALL, list);

    int bestOutcome = -2, bestTiebreak = 0;
    for (int i = 0; i < count; ++i) {
        const Move m = list[i].m;
        if (!movegen_is_legal(board, m))
            continue;

        const bool zeroing =
            is_capture_move(board, m) || type_of(piece_on(board, from_sq(m))) == PAWN;

        board_do_move(board, m);
        int childOk        = 0;
        const int childDtz = -probe_dtz(board, &childOk);
        board_undo_move(board, m);

        if (!childOk)
            continue;

        /* A zeroing move restarts the fifty-move clock, so its distance is
         * measured from zero; anything else inherits the clock. */
        const int childClock = zeroing ? 0 : clock + 1;

        /*
         * Rank by OUTCOME first and distance second, both from the root
         * mover's point of view.
         *
         * Outcome first is what makes this safe. Ranking on distance alone
         * needs every branch's sign convention to be right, and an earlier
         * version of this loop got one of them backwards - it rejected our own
         * winning moves in a drawn position and never rejected a losing one,
         * which degenerated to "play the first legal move" and could throw the
         * game away. search.c cuts the root list down to whatever is chosen
         * here, so nothing downstream can correct it.
         *
         * The fifty-move rule is applied here rather than to the raw distance:
         * a win too far away to claim is a draw, and a loss too far away is a
         * draw too, which is exactly the resource a lost position plays for.
         */
        const int childOutcome = (childDtz > 0 && childDtz + childClock <= 100)    ? 1
                                 : (childDtz < 0 && -childDtz + childClock <= 100) ? -1
                                                                                   : 0;

        /* Shortest win, longest resistance; irrelevant for a draw. */
        const int tiebreak = childOutcome > 0 ? -childDtz : childOutcome < 0 ? -childDtz : 0;

        if (out.move == MOVE_NONE || childOutcome > bestOutcome ||
            (childOutcome == bestOutcome && tiebreak > bestTiebreak)) {
            bestOutcome  = childOutcome;
            bestTiebreak = tiebreak;
            out.move     = m;
        }
    }

    return out;
}
