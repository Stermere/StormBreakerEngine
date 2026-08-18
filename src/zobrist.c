/*
 * zobrist.c - fixed-seed Zobrist key generation.
 *
 * See the determinism contract in zobrist.h before touching anything here.
 */
#include "zobrist.h"

Key ZobristPiece[PIECE_NB][SQUARE_NB];
Key ZobristEnPassant[8];
Key ZobristCastling[16];
Key ZobristSideToMove;

/*
 * xorshift64* - passes BigCrush, is two instructions per word, and is fully
 * reproducible across compilers and architectures because every operation is
 * defined on exact-width unsigned integers.
 */
static uint64_t rng_state;

static uint64_t rng_next(void) {
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * 0x2545F4914F6CDD1DULL;
}

void zobrist_init(void) {
    rng_state = 0x9D39247E33776D41ULL; /* fixed seed - see zobrist.h */

    for (int p = 0; p < PIECE_NB; ++p)
        for (int s = 0; s < SQUARE_NB; ++s)
            ZobristPiece[p][s] = rng_next();

    for (int f = 0; f < 8; ++f)
        ZobristEnPassant[f] = rng_next();

    /* Castling keys are built from four independent right-keys and then XORed
     * per mask. This lets do_move update the key by XORing a single combined
     * value instead of toggling rights one at a time. */
    Key rights[4];
    for (int i = 0; i < 4; ++i)
        rights[i] = rng_next();

    for (int mask = 0; mask < 16; ++mask) {
        ZobristCastling[mask] = 0;
        for (int i = 0; i < 4; ++i)
            if (mask & (1 << i))
                ZobristCastling[mask] ^= rights[i];
    }

    ZobristSideToMove = rng_next();
}
