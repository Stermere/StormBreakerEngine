/* bitboard.c - attack tables, built once by bb_init() and read-only after. */
#include "bitboard.h"

#include <assert.h>

Bitboard PawnAttacks[COLOR_NB][SQUARE_NB];
Bitboard KnightAttacks[SQUARE_NB];
Bitboard KingAttacks[SQUARE_NB];
Bitboard SquaresBetween[SQUARE_NB][SQUARE_NB];
Bitboard LineThrough[SQUARE_NB][SQUARE_NB];

Magic BishopMagics[SQUARE_NB];
Magic RookMagics[SQUARE_NB];

/* Sum of 2^popcount(mask) over all squares; init_magics asserts the fill matches. */
static Bitboard BishopTable[5248];
static Bitboard RookTable[102400];

/* (file, rank) deltas, so nothing can wrap round an edge. */
static const int KnightDeltas[8][2] = {{1, 2},   {2, 1},   {2, -1}, {1, -2},
                                       {-1, -2}, {-2, -1}, {-2, 1}, {-1, 2}};

static const int KingDeltas[8][2] = {{0, 1},  {1, 1},   {1, 0},  {1, -1},
                                     {0, -1}, {-1, -1}, {-1, 0}, {-1, 1}};

static const int BishopDeltas[4][2] = {{1, 1}, {1, -1}, {-1, -1}, {-1, 1}};
static const int RookDeltas[4][2]   = {{0, 1}, {1, 0}, {0, -1}, {-1, 0}};

/* SQ_NONE when the offset leaves the board. */
static Square offset_square(Square s, int df, int dr) {
    const int f = (int)file_of(s) + df;
    const int r = (int)rank_of(s) + dr;
    if (f < 0 || f > 7 || r < 0 || r > 7)
        return SQ_NONE;
    return make_square((File)f, (Rank)r);
}

/* Ray walk up to and including the first blocker: slow, obviously correct, and the
 * reference the fast tables are built and checked against. */
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
                break;
        }
    }
    return attacks;
}

#if !defined(USE_PEXT) || !defined(NDEBUG)
/* Constant seed, separate from Zobrist's, so magics are identical everywhere. */
static uint64_t magic_rng_state = 0x246C0B1AF3E17D9BULL;

static uint64_t magic_rng(void) {
    magic_rng_state ^= magic_rng_state >> 12;
    magic_rng_state ^= magic_rng_state << 25;
    magic_rng_state ^= magic_rng_state >> 27;
    return magic_rng_state * 0x2545F4914F6CDD1DULL;
}
#endif

#ifndef USE_PEXT

/* A useful magic has very few set bits, so ANDing three draws finds one far faster. */
static uint64_t magic_rng_sparse(void) { return magic_rng() & magic_rng() & magic_rng(); }

/* Per-rank seeds that make the magic search faster (~55ms rather than ~90ms). */
static const uint64_t MagicSeeds[8] = {8977, 44560, 54343, 38998, 5731, 95205, 104912, 17020};
#endif

/* With PEXT the table is indexed by pext() directly. Otherwise search for a multiplier
 * with no destructive collisions. */
static void init_magics(Bitboard *table, Magic magics[SQUARE_NB], const int deltas[4][2]) {
    Bitboard reference[4096];
    size_t used = 0;
#ifndef USE_PEXT
    Bitboard occupancy[4096];
    int epoch[4096] = {0};
    int current     = 0;
#endif

    for (Square s = SQ_A1; s <= SQ_H8; ++s) {
        Magic *const m = &magics[s];

        /* Edge blockers hide nothing, except along the square's own rank and file. */
        const Bitboard edges = ((BB_RANK_1 | BB_RANK_8) & ~rank_bb(rank_of(s))) |
                               ((BB_FILE_A | BB_FILE_H) & ~file_bb(file_of(s)));

        m->mask    = slide(s, BB_EMPTY, deltas) & ~edges;
        m->shift   = 64 - (unsigned)popcount(m->mask);
        m->attacks = table + used;

        int size = 0;
        /* Enumerate every subset of the mask (Carry-Rippler). */
        Bitboard subset = BB_EMPTY;
        do {
            reference[size] = slide(s, subset, deltas);
#ifdef USE_PEXT
            m->attacks[pext(subset, m->mask)] = reference[size];
#else
            occupancy[size] = subset;
#endif
            ++size;
            subset = (subset - m->mask) & m->mask;
        } while (subset);

        used += (size_t)size;

        /* `epoch` marks the current attempt's entries, so the slice is never cleared. */
#ifndef USE_PEXT
        magic_rng_state = MagicSeeds[rank_of(s)];

        for (int i = 0; i < size;) {
            for (m->magic = 0; popcount((m->magic * m->mask) >> 56) < 6;)
                m->magic = magic_rng_sparse();

            for (++current, i = 0; i < size; ++i) {
                const unsigned idx = magic_index(m, occupancy[i]);

                if (epoch[idx] < current) {
                    epoch[idx]      = current;
                    m->attacks[idx] = reference[i];
                } else if (m->attacks[idx] != reference[i]) {
                    break;
                }
            }
        }
#endif
    }

    assert(used == (deltas == BishopDeltas ? 5248u : 102400u));
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

    /* Before anything below calls attacks_bb(). */
    init_magics(BishopTable, BishopMagics, BishopDeltas);
    init_magics(RookTable, RookMagics, RookDeltas);

#ifndef NDEBUG
    for (Square s = SQ_A1; s <= SQ_H8; ++s)
        for (int i = 0; i < 256; ++i) {
            const Bitboard occ = magic_rng() & magic_rng();
            assert(bishop_attacks(s, occ) == slide(s, occ, BishopDeltas));
            assert(rook_attacks(s, occ) == slide(s, occ, RookDeltas));
        }
#endif

    /* From empty-board slider attacks: aligned squares see each other, and the squares
     * between are the overlap of their attacks with each other as blocker. */
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
