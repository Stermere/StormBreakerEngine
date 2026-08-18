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
 * PERFORMANCE NOTE: this is the correct-but-slow classical ray walk. It is
 * fine for perft bring-up and for anything outside the search, but it is far
 * too slow for a competitive search - expect a large Elo gain from replacing
 * it with magic bitboards (or PEXT when USE_PEXT is defined).
 *
 * TODO(perf): implement magic/PEXT sliding attacks behind this same signature.
 */
Bitboard bishop_attacks(Square s, Bitboard occupied);
Bitboard rook_attacks(Square s, Bitboard occupied);
Bitboard queen_attacks(Square s, Bitboard occupied);

/* Attacks for any piece type. `occupied` is ignored for leapers. */
Bitboard attacks_bb(PieceType pt, Square s, Bitboard occupied);

/* Debug helper: dump a bitboard as an 8x8 grid to stdout. */
void bb_print(Bitboard b);

#endif /* BITBOARD_H */
