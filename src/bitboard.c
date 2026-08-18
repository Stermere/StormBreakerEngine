/*
 * bitboard.c - attack table generation.
 *
 * Everything here is built once at startup by bb_init(). The tables are pure
 * functions of the board geometry, so they contain no position state and are
 * safe to share across search threads without synchronisation.
 */
#include "bitboard.h"

#include <stdio.h>

Bitboard PawnAttacks[COLOR_NB][SQUARE_NB];
Bitboard KnightAttacks[SQUARE_NB];
Bitboard KingAttacks[SQUARE_NB];
Bitboard SquaresBetween[SQUARE_NB][SQUARE_NB];
Bitboard LineThrough[SQUARE_NB][SQUARE_NB];

/* Offsets as (file, rank) deltas so edge wrapping is impossible by
 * construction: a candidate is rejected unless both components stay on board. */
static const int KnightDeltas[8][2] = {{1, 2},   {2, 1},   {2, -1}, {1, -2},
                                       {-1, -2}, {-2, -1}, {-2, 1}, {-1, 2}};

static const int KingDeltas[8][2] = {{0, 1},  {1, 1},   {1, 0},  {1, -1},
                                     {0, -1}, {-1, -1}, {-1, 0}, {-1, 1}};

static const int BishopDeltas[4][2] = {{1, 1}, {1, -1}, {-1, -1}, {-1, 1}};
static const int RookDeltas[4][2]   = {{0, 1}, {1, 0}, {0, -1}, {-1, 0}};

/* Returns SQ_NONE when the offset leaves the board. */
static Square offset_square(Square s, int df, int dr) {
    const int f = (int)file_of(s) + df;
    const int r = (int)rank_of(s) + dr;
    if (f < 0 || f > 7 || r < 0 || r > 7)
        return SQ_NONE;
    return make_square((File)f, (Rank)r);
}

/* Walk each ray outward from `s`, stopping on (and including) the first
 * occupied square. This is the textbook classical approach: simple and
 * obviously correct, which is exactly what perft bring-up needs. */
static Bitboard slide(Square s, Bitboard occupied, const int deltas[4][2]) {
    Bitboard attacks = BB_EMPTY;

    for (int i = 0; i < 4; ++i) {
        Square cur = s;
        for (;;) {
            cur = offset_square(cur, deltas[i][0], deltas[i][1]);
            if (cur == SQ_NONE)
                break;
            attacks |= square_bb(cur);
            if (bb_test(occupied, cur))
                break; /* blocker is attacked, but nothing behind it is */
        }
    }
    return attacks;
}

Bitboard bishop_attacks(Square s, Bitboard occupied) { return slide(s, occupied, BishopDeltas); }
Bitboard rook_attacks(Square s, Bitboard occupied) { return slide(s, occupied, RookDeltas); }

Bitboard queen_attacks(Square s, Bitboard occupied) {
    return bishop_attacks(s, occupied) | rook_attacks(s, occupied);
}

Bitboard attacks_bb(PieceType pt, Square s, Bitboard occupied) {
    switch (pt) {
    case KNIGHT: return KnightAttacks[s];
    case BISHOP: return bishop_attacks(s, occupied);
    case ROOK: return rook_attacks(s, occupied);
    case QUEEN: return queen_attacks(s, occupied);
    case KING: return KingAttacks[s];
    default: return BB_EMPTY; /* pawns are directional: use pawn_attacks() */
    }
}

void bb_init(void) {
    for (Square s = SQ_A1; s <= SQ_H8; ++s) {
        const Bitboard b = square_bb(s);

        PawnAttacks[WHITE][s] = shift_north_east(b) | shift_north_west(b);
        PawnAttacks[BLACK][s] = shift_south_east(b) | shift_south_west(b);

        KnightAttacks[s] = BB_EMPTY;
        for (int i = 0; i < 8; ++i) {
            const Square t = offset_square(s, KnightDeltas[i][0], KnightDeltas[i][1]);
            if (t != SQ_NONE)
                KnightAttacks[s] |= square_bb(t);
        }

        KingAttacks[s] = BB_EMPTY;
        for (int i = 0; i < 8; ++i) {
            const Square t = offset_square(s, KingDeltas[i][0], KingDeltas[i][1]);
            if (t != SQ_NONE)
                KingAttacks[s] |= square_bb(t);
        }
    }

    /* Between/Line are derived from empty-board slider attacks. Two squares are
     * aligned iff one attacks the other with nothing in the way; the squares
     * strictly between them are then the intersection of the two attack sets
     * computed with the far square treated as a blocker. */
    for (Square a = SQ_A1; a <= SQ_H8; ++a) {
        for (Square b = SQ_A1; b <= SQ_H8; ++b) {
            SquaresBetween[a][b] = BB_EMPTY;
            LineThrough[a][b]    = BB_EMPTY;

            if (a == b)
                continue;

            const PieceType movers[2] = {BISHOP, ROOK};
            for (int i = 0; i < 2; ++i) {
                const PieceType pt = movers[i];
                if (!(attacks_bb(pt, a, BB_EMPTY) & square_bb(b)))
                    continue;

                LineThrough[a][b] = (attacks_bb(pt, a, BB_EMPTY) & attacks_bb(pt, b, BB_EMPTY)) |
                                    square_bb(a) | square_bb(b);

                SquaresBetween[a][b] =
                    attacks_bb(pt, a, square_bb(b)) & attacks_bb(pt, b, square_bb(a));
            }
        }
    }
}

void bb_print(Bitboard b) {
    for (int r = RANK_8; r >= RANK_1; --r) {
        printf("  +---+---+---+---+---+---+---+---+\n%d ", r + 1);
        for (int f = FILE_A; f <= FILE_H; ++f)
            printf("| %c ", bb_test(b, make_square((File)f, (Rank)r)) ? 'X' : ' ');
        printf("|\n");
    }
    printf("  +---+---+---+---+---+---+---+---+\n    a   b   c   d   e   f   g   h\n");
    printf("  0x%016llXULL\n", (unsigned long long)b);
}
