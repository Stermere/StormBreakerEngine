/*
 * move.h - 16-bit move encoding.
 *
 *   bits  0-5   origin square
 *   bits  6-11  destination square
 *   bits 12-13  promotion piece (0=knight .. 3=queen), promotions only
 *   bits 14-15  move type
 *
 * Castling is encoded king-captures-own-rook, the only spelling that stays
 * unambiguous in Chess960, where a king on b1 castling long reads as a king step.
 */
#ifndef MOVE_H
#define MOVE_H

#include "types.h"

typedef uint16_t Move;

enum { MOVE_NONE = 0, MOVE_NULL = 65 };

typedef enum {
    MT_NORMAL     = 0,
    MT_PROMOTION  = 1 << 14,
    MT_EN_PASSANT = 2 << 14,
    MT_CASTLING   = 3 << 14
} MoveType;

static inline Square from_sq(Move m) { return (Square)(m & 0x3F); }
static inline Square to_sq(Move m) { return (Square)((m >> 6) & 0x3F); }
static inline MoveType type_of_move(Move m) { return (MoveType)(m & (3 << 14)); }

/* Only meaningful for promotions. */
static inline PieceType promotion_type(Move m) { return (PieceType)(((m >> 12) & 3) + KNIGHT); }

static inline Move make_move(Square from, Square to) { return (Move)(from | (to << 6)); }

static inline Move make_move_typed(Square from, Square to, MoveType mt) {
    return (Move)(from | (to << 6) | mt);
}

static inline Move make_promotion(Square from, Square to, PieceType promo) {
    return (Move)(from | (to << 6) | ((promo - KNIGHT) << 12) | MT_PROMOTION);
}

/* Neither sentinel. Note that MOVE_NONE == 0 is falsy. */
static inline bool is_ok_move(Move m) { return m != MOVE_NONE && m != MOVE_NULL; }

/* Generation and scoring share one struct to keep ordering cache-local. */
typedef struct {
    Move m;
    int score;
} ScoredMove;

/*
 * Long algebraic ("e2e4", "e7e8q") into `buf`, which must hold six bytes; returns
 * `buf`. `chess960` picks how CASTLING is spelled - false the king's destination
 * "e1g1", true king-takes-rook "e1h1" - and is a parameter because the answer
 * belongs to the position rather than the process, so pass pos->chess960.
 */
char *move_to_str(Move m, bool chess960, char *buf);

#endif
