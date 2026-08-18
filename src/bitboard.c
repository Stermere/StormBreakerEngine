/*
 * bitboard.c - attack table generation.
 *
 * Everything here is built once at startup by bb_init(). The tables are pure
 * functions of the board geometry, so they contain no position state and are
 * safe to share across search threads without synchronisation.
 */
#include "bitboard.h"

#include <assert.h>
#include <stdio.h>

Bitboard PawnAttacks[COLOR_NB][SQUARE_NB];
Bitboard KnightAttacks[SQUARE_NB];
Bitboard KingAttacks[SQUARE_NB];
Bitboard SquaresBetween[SQUARE_NB][SQUARE_NB];
Bitboard LineThrough[SQUARE_NB][SQUARE_NB];

Magic BishopMagics[SQUARE_NB];
Magic RookMagics[SQUARE_NB];

/*
 * Backing store for the sliding attack tables. The sizes are the well-known
 * totals of 2^popcount(mask) summed over all squares; init_magics asserts that
 * the fill lands exactly on the end, so a wrong constant cannot go unnoticed.
 */
static Bitboard BishopTable[5248];
static Bitboard RookTable[102400];

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

/*
 * Reference sliding attacks: walk each ray outward from `s`, stopping on (and
 * including) the first occupied square.
 *
 * This is far too slow for the search, but it is obviously correct, so it is
 * the single source of truth the fast tables are built and verified against.
 */
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

/* -------------------------------------------------------------- magics --- */

/*
 * xorshift64*, seeded from a constant. Magic numbers are part of the engine's
 * behaviour, so they must be identical on every run and every machine - see
 * the determinism contract in zobrist.h. This deliberately does not share
 * state with the Zobrist generator, so changing one cannot perturb the other.
 *
 * Its only consumers are the magic search and the debug self-test, neither of
 * which exists in a release build that uses PEXT.
 */
#if !defined(USE_PEXT) || !defined(NDEBUG)
static uint64_t magic_rng_state = 0x246C0B1AF3E17D9BULL;

static uint64_t magic_rng(void) {
    magic_rng_state ^= magic_rng_state >> 12;
    magic_rng_state ^= magic_rng_state << 25;
    magic_rng_state ^= magic_rng_state >> 27;
    return magic_rng_state * 0x2545F4914F6CDD1DULL;
}
#endif

#ifndef USE_PEXT
/* A useful magic has very few set bits, so ANDing three draws together finds
 * candidates orders of magnitude faster than uniform random ones. */
static uint64_t magic_rng_sparse(void) { return magic_rng() & magic_rng() & magic_rng(); }

/*
 * Per-rank starting points for the magic search, restarted at every square.
 *
 * The search is a random walk, and where it starts changes how long it walks.
 * These are the seeds Stockfish uses for the same xorshift64* generator, and
 * they measure here at roughly 55ms to find all 128 multipliers against 90ms
 * from a single seed. That is paid once per process, and a match spawns one
 * process per game.
 *
 * They affect only how quickly a valid magic is found, never which attacks it
 * produces - init_magics accepts a candidate only when it reproduces slide()
 * exactly for every occupancy.
 */
static const uint64_t MagicSeeds[8] = {8977, 44560, 54343, 38998, 5731, 95205, 104912, 17020};
#endif

/*
 * Builds the attack table for one piece type and fills in its Magic entries.
 *
 * Under USE_PEXT no search happens: the CPU extracts the masked bits directly,
 * so the table is simply indexed by the occupancy subset number. Otherwise a
 * multiplier is searched for that maps every occupancy subset onto a distinct
 * index, tolerating collisions only when both subsets yield the same attacks.
 */
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

        /* Blockers on the board edge cannot hide anything behind them, so
         * excluding them roughly halves the table without losing a case. The
         * square's own rank and file are exempt: a rook on A1 genuinely is
         * blocked by a piece on A8. */
        const Bitboard edges = ((BB_RANK_1 | BB_RANK_8) & ~rank_bb(rank_of(s))) |
                               ((BB_FILE_A | BB_FILE_H) & ~file_bb(file_of(s)));

        m->mask    = slide(s, BB_EMPTY, deltas) & ~edges;
        m->shift   = 64 - (unsigned)popcount(m->mask);
        m->attacks = table + used;

        /* Enumerate every subset of the mask (Carry-Rippler). */
        int size        = 0;
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

#ifndef USE_PEXT
        /* Search for a multiplier that hashes all `size` subsets without a
         * destructive collision. `epoch` marks which entries belong to the
         * attempt in progress, which avoids clearing the slice each time. */
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
                    break; /* two different attack sets want the same slot */
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

    /* Must precede everything below: the tables here are what attacks_bb()
     * reads once sliders are involved. */
    init_magics(BishopTable, BishopMagics, BishopDeltas);
    init_magics(RookTable, RookMagics, RookDeltas);

#ifndef NDEBUG
    /* Cross-check the fast path against the reference walk on pseudo-random
     * occupancies. A wrong magic would otherwise surface as a bizarre illegal
     * move deep in a search rather than here. */
    for (Square s = SQ_A1; s <= SQ_H8; ++s)
        for (int i = 0; i < 256; ++i) {
            const Bitboard occ = magic_rng() & magic_rng();
            assert(bishop_attacks(s, occ) == slide(s, occ, BishopDeltas));
            assert(rook_attacks(s, occ) == slide(s, occ, RookDeltas));
        }
#endif

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
