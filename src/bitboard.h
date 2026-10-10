/* bitboard.h - bitboards (bit i is square i, A1 = 0) and attack lookup. bb_init() first. */
#ifndef BITBOARD_H
#define BITBOARD_H

#include "types.h"

#define BB_EMPTY 0ULL
#define BB_ALL   0xFFFFFFFFFFFFFFFFULL

#define BB_FILE_A 0x0101010101010101ULL
#define BB_FILE_B (BB_FILE_A << 1)
#define BB_FILE_C (BB_FILE_A << 2)
#define BB_FILE_D (BB_FILE_A << 3)
#define BB_FILE_E (BB_FILE_A << 4)
#define BB_FILE_F (BB_FILE_A << 5)
#define BB_FILE_G (BB_FILE_A << 6)
#define BB_FILE_H (BB_FILE_A << 7)

#define BB_RANK_1 0x00000000000000FFULL
#define BB_RANK_2 (BB_RANK_1 << 8)
#define BB_RANK_3 (BB_RANK_1 << 16)
#define BB_RANK_4 (BB_RANK_1 << 24)
#define BB_RANK_5 (BB_RANK_1 << 32)
#define BB_RANK_6 (BB_RANK_1 << 40)
#define BB_RANK_7 (BB_RANK_1 << 48)
#define BB_RANK_8 (BB_RANK_1 << 56)

#define BB_LIGHT_SQUARES 0x55AA55AA55AA55AAULL
#define BB_DARK_SQUARES  0xAA55AA55AA55AA55ULL

static inline Bitboard square_bb(Square s) { return 1ULL << s; }
static inline Bitboard file_bb(File f) { return BB_FILE_A << f; }
static inline Bitboard rank_bb(Rank r) { return BB_RANK_1 << (8 * r); }

static inline bool bb_test(Bitboard b, Square s) { return (b & square_bb(s)) != 0; }

/* Cheaper than popcount(b) <= 1. */
static inline bool bb_at_most_one(Bitboard b) { return (b & (b - 1)) == 0; }
static inline bool bb_more_than_one(Bitboard b) { return (b & (b - 1)) != 0; }

/* Masking before the shift stops a piece wrapping from the H file round to the A file. */
static inline Bitboard shift_north(Bitboard b) { return b << 8; }
static inline Bitboard shift_south(Bitboard b) { return b >> 8; }
static inline Bitboard shift_east(Bitboard b) { return (b & ~BB_FILE_H) << 1; }
static inline Bitboard shift_west(Bitboard b) { return (b & ~BB_FILE_A) >> 1; }
static inline Bitboard shift_north_east(Bitboard b) { return (b & ~BB_FILE_H) << 9; }
static inline Bitboard shift_north_west(Bitboard b) { return (b & ~BB_FILE_A) << 7; }
static inline Bitboard shift_south_east(Bitboard b) { return (b & ~BB_FILE_H) >> 7; }
static inline Bitboard shift_south_west(Bitboard b) { return (b & ~BB_FILE_A) >> 9; }

/* Read through the accessors below. */
extern Bitboard PawnAttacks[COLOR_NB][SQUARE_NB];
extern Bitboard KnightAttacks[SQUARE_NB];
extern Bitboard KingAttacks[SQUARE_NB];

/* Squares strictly between a and b on a shared line, else empty. */
extern Bitboard SquaresBetween[SQUARE_NB][SQUARE_NB];

/* The whole line through both squares, else empty. */
extern Bitboard LineThrough[SQUARE_NB][SQUARE_NB];

void bb_init(void);

static inline Bitboard pawn_attacks(Color c, Square s) { return PawnAttacks[c][s]; }
static inline Bitboard knight_attacks(Square s) { return KnightAttacks[s]; }
static inline Bitboard king_attacks(Square s) { return KingAttacks[s]; }

static inline bool aligned(Square a, Square b, Square c) {
    return (LineThrough[a][b] & square_bb(c)) != 0;
}

typedef struct {
    Bitboard *attacks;
    Bitboard mask;
    uint64_t magic;
    unsigned shift;
} Magic;

extern Magic BishopMagics[SQUARE_NB];
extern Magic RookMagics[SQUARE_NB];

/* PEXT or magic multiplication; either way the same table and the same answer. */
static inline unsigned magic_index(const Magic *m, Bitboard occupied) {
#ifdef USE_PEXT
    return (unsigned)pext(occupied, m->mask);
#else
    return (unsigned)(((occupied & m->mask) * m->magic) >> m->shift);
#endif
}

static inline Bitboard bishop_attacks(Square s, Bitboard occupied) {
    const Magic *const m = &BishopMagics[s];
    return m->attacks[magic_index(m, occupied)];
}

static inline Bitboard rook_attacks(Square s, Bitboard occupied) {
    const Magic *const m = &RookMagics[s];
    return m->attacks[magic_index(m, occupied)];
}

static inline Bitboard queen_attacks(Square s, Bitboard occupied) {
    return bishop_attacks(s, occupied) | rook_attacks(s, occupied);
}

/* `occupied` is ignored for leapers. */
static inline Bitboard attacks_bb(PieceType pt, Square s, Bitboard occupied) {
    switch (pt) {
    case KNIGHT: return KnightAttacks[s];
    case BISHOP: return bishop_attacks(s, occupied);
    case ROOK: return rook_attacks(s, occupied);
    case QUEEN: return queen_attacks(s, occupied);
    case KING: return KingAttacks[s];
    default: return BB_EMPTY;
    }
}

#endif
