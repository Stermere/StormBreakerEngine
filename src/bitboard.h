/*
 * bitboard.h - bitboard constants, shifts and attack lookup.
 *
 * Bit i of a Bitboard corresponds to Square i (A1 = bit 0, H8 = bit 63).
 * Printed with bb_print(), rank 8 appears at the top as you would expect.
 *
 * Tables here are filled by bb_init(), which main() calls exactly once before
 * anything else runs.
 */
#ifndef BITBOARD_H
#define BITBOARD_H

#include "types.h"

/* --------------------------------------------------------------- masks -- */

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

/* ------------------------------------------------------------- accessors -- */

static inline Bitboard square_bb(Square s) { return 1ULL << s; }
static inline Bitboard file_bb(File f) { return BB_FILE_A << f; }
static inline Bitboard rank_bb(Rank r) { return BB_RANK_1 << (8 * r); }

static inline bool bb_test(Bitboard b, Square s) { return (b & square_bb(s)) != 0; }
static inline Bitboard bb_set(Bitboard b, Square s) { return b | square_bb(s); }
static inline Bitboard bb_clear(Bitboard b, Square s) { return b & ~square_bb(s); }

/* True when at most one bit is set - cheaper than popcount(b) <= 1. */
static inline bool bb_at_most_one(Bitboard b) { return (b & (b - 1)) == 0; }
static inline bool bb_more_than_one(Bitboard b) { return (b & (b - 1)) != 0; }

/* ---------------------------------------------------------------- shifts -- */

/* Board-edge-safe directional shifts. Masking before the shift stops pieces
 * wrapping from the H file round to the A file. */
static inline Bitboard shift_north(Bitboard b) { return b << 8; }
static inline Bitboard shift_south(Bitboard b) { return b >> 8; }
static inline Bitboard shift_east(Bitboard b) { return (b & ~BB_FILE_H) << 1; }
static inline Bitboard shift_west(Bitboard b) { return (b & ~BB_FILE_A) >> 1; }
static inline Bitboard shift_north_east(Bitboard b) { return (b & ~BB_FILE_H) << 9; }
static inline Bitboard shift_north_west(Bitboard b) { return (b & ~BB_FILE_A) << 7; }
static inline Bitboard shift_south_east(Bitboard b) { return (b & ~BB_FILE_H) >> 7; }
static inline Bitboard shift_south_west(Bitboard b) { return (b & ~BB_FILE_A) >> 9; }

/* Forward from `c`'s point of view: north for white, south for black. */
static inline Bitboard shift_forward(Bitboard b, Color c) {
    return c == WHITE ? shift_north(b) : shift_south(b);
}

/* ------------------------------------------------------------ attack API -- */

/* Precomputed tables. Read via the accessors below, never indexed directly
 * elsewhere, so the storage strategy can change (e.g. to magic bitboards)
 * without touching call sites. */
extern Bitboard PawnAttacks[COLOR_NB][SQUARE_NB];
extern Bitboard KnightAttacks[SQUARE_NB];
extern Bitboard KingAttacks[SQUARE_NB];

/* SquaresBetween[a][b] - squares strictly between a and b when they share a
 * rank, file or diagonal; empty otherwise. The classic check-blocking mask. */
extern Bitboard SquaresBetween[SQUARE_NB][SQUARE_NB];

/* LineThrough[a][b] - the full rank/file/diagonal containing both a and b,
 * empty if they are not aligned. Used for pin detection. */
extern Bitboard LineThrough[SQUARE_NB][SQUARE_NB];

void bb_init(void);

static inline Bitboard pawn_attacks(Color c, Square s) { return PawnAttacks[c][s]; }
static inline Bitboard knight_attacks(Square s) { return KnightAttacks[s]; }
static inline Bitboard king_attacks(Square s) { return KingAttacks[s]; }

static inline bool aligned(Square a, Square b, Square c) {
    return (LineThrough[a][b] & square_bb(c)) != 0;
}

/*
 * Sliding attacks from `s` given `occupied`.
 *
 * These are the hottest lookups in the engine, so they are table-driven and
 * inline rather than a ray walk. Two interchangeable indexing schemes share
 * the same table:
 *
 *   - PEXT (USE_PEXT): the CPU extracts the relevant occupancy bits directly.
 *     Fastest where the instruction is fast - Intel Haswell onwards and AMD
 *     Zen 3 onwards. It is microcoded and very slow on Zen 1/2, which is why
 *     the Makefile only defines USE_PEXT for the arch profiles where it wins.
 *
 *   - Magic multiplication: a perfect hash of the relevant occupancy bits,
 *     found once at startup. Portable, and within a few percent of PEXT.
 *
 * Both index the same attack table, so switching schemes cannot change any
 * result - only the speed of getting it.
 */
typedef struct {
    Bitboard *attacks; /* slice of the shared table for this square */
    Bitboard mask;     /* occupancy bits that actually block this square */
    uint64_t magic;    /* unused under USE_PEXT */
    unsigned shift;    /* unused under USE_PEXT */
} Magic;

extern Magic BishopMagics[SQUARE_NB];
extern Magic RookMagics[SQUARE_NB];

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

/* Attacks for any piece type. `occupied` is ignored for leapers. */
static inline Bitboard attacks_bb(PieceType pt, Square s, Bitboard occupied) {
    switch (pt) {
    case KNIGHT: return KnightAttacks[s];
    case BISHOP: return bishop_attacks(s, occupied);
    case ROOK: return rook_attacks(s, occupied);
    case QUEEN: return queen_attacks(s, occupied);
    case KING: return KingAttacks[s];
    default: return BB_EMPTY; /* pawns are directional: use pawn_attacks() */
    }
}

/* Debug helper: dump a bitboard as an 8x8 grid to stdout. */
void bb_print(Bitboard b);

#endif /* BITBOARD_H */
