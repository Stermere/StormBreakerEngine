/*
 * types.h - fundamental types, constants and CPU intrinsics.
 *
 * Board representation is little-endian rank-file (LERF): A1 = 0, H8 = 63,
 * so square = rank * 8 + file. This is the convention essentially every
 * modern engine uses; keeping it means published bitboard tricks translate
 * verbatim.
 */
#ifndef TYPES_H
#define TYPES_H

#include <stdbool.h>
#include <stdint.h>

#if defined(_MSC_VER)
#include <intrin.h>
#endif
#if defined(USE_PEXT) || defined(USE_POPCNT)
#include <immintrin.h>
#endif

/* ------------------------------------------------------------------ meta -- */

#define ENGINE_NAME    "StormBreaker"
#define ENGINE_VERSION "0.2.0-dev"
#define ENGINE_AUTHOR  "Collin Kees"

/* --------------------------------------------------------------- limits -- */

enum {
    MAX_PLY   = 246, /* deepest search ply; also sizes the PV and killer tables */
    MAX_MOVES = 256, /* upper bound on legal moves in any legal position */
    SQUARE_NB = 64,
    COLOR_NB  = 2,
    PIECE_NB  = 16 /* piece codes are sparse: see the Piece enum */
};

/* ----------------------------------------------------------- basic types -- */

typedef uint64_t Bitboard;
typedef uint64_t Key; /* Zobrist hash */
typedef int Value;
typedef int Depth;

/* --------------------------------------------------------------- colours -- */

typedef enum { WHITE = 0, BLACK = 1 } Color;

/*
 * Piece encoding: piece = (color << 3) | type.
 * Colour is therefore `p >> 3` and type is `p & 7`, both single instructions,
 * and NO_PIECE = 0 makes an empty mailbox square falsy.
 */
typedef enum {
    NO_PIECE_TYPE = 0,
    PAWN          = 1,
    KNIGHT        = 2,
    BISHOP        = 3,
    ROOK          = 4,
    QUEEN         = 5,
    KING          = 6,
    PIECE_TYPE_NB = 7
} PieceType;

typedef enum {
    NO_PIECE = 0,
    W_PAWN   = 1,
    W_KNIGHT = 2,
    W_BISHOP = 3,
    W_ROOK   = 4,
    W_QUEEN  = 5,
    W_KING   = 6,
    B_PAWN   = 9,
    B_KNIGHT = 10,
    B_BISHOP = 11,
    B_ROOK   = 12,
    B_QUEEN  = 13,
    B_KING   = 14
} Piece;

static inline Color color_of(Piece p) { return (Color)(p >> 3); }
static inline PieceType type_of(Piece p) { return (PieceType)(p & 7); }
static inline Piece make_piece(Color c, PieceType pt) { return (Piece)((c << 3) | pt); }

/* --------------------------------------------------------------- squares -- */

/* clang-format off */
typedef enum {
    SQ_A1, SQ_B1, SQ_C1, SQ_D1, SQ_E1, SQ_F1, SQ_G1, SQ_H1,
    SQ_A2, SQ_B2, SQ_C2, SQ_D2, SQ_E2, SQ_F2, SQ_G2, SQ_H2,
    SQ_A3, SQ_B3, SQ_C3, SQ_D3, SQ_E3, SQ_F3, SQ_G3, SQ_H3,
    SQ_A4, SQ_B4, SQ_C4, SQ_D4, SQ_E4, SQ_F4, SQ_G4, SQ_H4,
    SQ_A5, SQ_B5, SQ_C5, SQ_D5, SQ_E5, SQ_F5, SQ_G5, SQ_H5,
    SQ_A6, SQ_B6, SQ_C6, SQ_D6, SQ_E6, SQ_F6, SQ_G6, SQ_H6,
    SQ_A7, SQ_B7, SQ_C7, SQ_D7, SQ_E7, SQ_F7, SQ_G7, SQ_H7,
    SQ_A8, SQ_B8, SQ_C8, SQ_D8, SQ_E8, SQ_F8, SQ_G8, SQ_H8,
    SQ_NONE = 64
} Square;
/* clang-format on */

typedef enum { FILE_A, FILE_B, FILE_C, FILE_D, FILE_E, FILE_F, FILE_G, FILE_H } File;
typedef enum { RANK_1, RANK_2, RANK_3, RANK_4, RANK_5, RANK_6, RANK_7, RANK_8 } Rank;

static inline Square make_square(File f, Rank r) { return (Square)((r << 3) | f); }
static inline File file_of(Square s) { return (File)(s & 7); }
static inline Rank rank_of(Square s) { return (Rank)(s >> 3); }
static inline bool is_ok_square(Square s) { return s >= SQ_A1 && s <= SQ_H8; }

/* Mirror a square vertically: used to read black's position with white's tables. */
static inline Square flip_rank(Square s) { return (Square)(s ^ 56); }

/* Rank as seen by `c`, so relative_rank(BLACK, SQ_A7) == RANK_2. */
static inline Rank relative_rank(Color c, Square s) { return (Rank)((s >> 3) ^ (c * 7)); }

/* Square delta of a single pawn push for `c`: north for white, south for black. */
static inline int pawn_push(Color c) { return c == WHITE ? 8 : -8; }

/* -------------------------------------------------------------- castling -- */

/* Bit flags so a position's whole castling state fits in 4 bits. */
typedef enum {
    NO_CASTLING  = 0,
    WHITE_OO     = 1,
    WHITE_OOO    = 2,
    BLACK_OO     = 4,
    BLACK_OOO    = 8,
    WHITE_ANY    = WHITE_OO | WHITE_OOO,
    BLACK_ANY    = BLACK_OO | BLACK_OOO,
    ANY_CASTLING = WHITE_ANY | BLACK_ANY
} CastlingRights;

/* ---------------------------------------------------------------- values -- */

enum {
    VALUE_ZERO     = 0,
    VALUE_DRAW     = 0,
    VALUE_INFINITE = 32001,
    VALUE_NONE     = 32002,
    VALUE_MATE     = 32000,
    /* Scores at or beyond this are forced mates; used to detect and report them. */
    VALUE_MATE_IN_MAX_PLY  = VALUE_MATE - MAX_PLY,
    VALUE_MATED_IN_MAX_PLY = -VALUE_MATE_IN_MAX_PLY
};

static inline Value mate_in(int ply) { return VALUE_MATE - ply; }
static inline Value mated_in(int ply) { return ply - VALUE_MATE; }
static inline bool is_mate_score(Value v) {
    return v >= VALUE_MATE_IN_MAX_PLY || v <= VALUE_MATED_IN_MAX_PLY;
}

/* ------------------------------------------------------------ intrinsics -- */

/* Population count. */
static inline int popcount(Bitboard b) {
#if defined(_MSC_VER) && defined(USE_POPCNT)
    return (int)_mm_popcnt_u64(b);
#elif defined(__GNUC__)
    return __builtin_popcountll(b);
#else
    /* Portable fallback (SWAR). */
    b = b - ((b >> 1) & 0x5555555555555555ULL);
    b = (b & 0x3333333333333333ULL) + ((b >> 2) & 0x3333333333333333ULL);
    b = (b + (b >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return (int)((b * 0x0101010101010101ULL) >> 56);
#endif
}

/* Index of the least significant set bit. Undefined for b == 0. */
static inline Square lsb(Bitboard b) {
#if defined(_MSC_VER)
    unsigned long idx;
    _BitScanForward64(&idx, b);
    return (Square)idx;
#else
    return (Square)__builtin_ctzll(b);
#endif
}

/* Index of the most significant set bit. Undefined for b == 0. */
static inline Square msb(Bitboard b) {
#if defined(_MSC_VER)
    unsigned long idx;
    _BitScanReverse64(&idx, b);
    return (Square)idx;
#else
    return (Square)(63 ^ __builtin_clzll(b));
#endif
}

/* Remove and return the least significant set bit. The standard iteration idiom:
 *     while (bb) { Square s = pop_lsb(&bb); ... }
 */
static inline Square pop_lsb(Bitboard *b) {
    const Square s = lsb(*b);
    *b &= *b - 1;
    return s;
}

/* Parallel bit extract - the fast magic-free sliding attack lookup on Intel.
 * Guarded because it is microcoded and very slow on AMD Zen 1/2. */
#ifdef USE_PEXT
static inline uint64_t pext(uint64_t src, uint64_t mask) { return _pext_u64(src, mask); }
#endif

#endif /* TYPES_H */
