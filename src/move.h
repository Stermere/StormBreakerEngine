/*
 * move.h - 16-bit move encoding.
 *
 * Layout:
 *   bits  0-5   origin square      (0..63)
 *   bits  6-11  destination square (0..63)
 *   bits 12-13  promotion piece    (0=knight, 1=bishop, 2=rook, 3=queen)
 *   bits 14-15  move type          (see MoveType)
 *
 * Packing a move into 16 bits keeps move lists cache-resident and lets the
 * transposition table store a move in half a word. The promotion field is only
 * meaningful when type == MT_PROMOTION.
 *
 * Castling is encoded king-captures-own-rook (origin = king, destination =
 * rook). That looks odd but it is the only encoding that stays unambiguous in
 * Chess960, so the representation is ready for it.
 *
 * The RULES are not: move generation hard-codes standard castling geometry and
 * board_set_fen rejects Shredder-FEN. UCI_Chess960 currently switches how
 * castling is SPELLED to the GUI and nothing more. See the TODO(chess960) in
 * board.c for what is still missing.
 */
#ifndef MOVE_H
#define MOVE_H

#include "types.h"

typedef uint16_t Move;

enum {
    MOVE_NONE = 0,
    MOVE_NULL = 65 /* b1b1: not a legal move, so it is safe as a sentinel */
};

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

/* A move is "ok" if it is neither sentinel. Note MOVE_NONE == 0 is falsy. */
static inline bool is_ok_move(Move m) { return m != MOVE_NONE && m != MOVE_NULL; }

/* A scored move. Move ordering is the single largest contributor to search
 * efficiency, so generation and scoring share one struct to stay cache-local. */
typedef struct {
    Move m;
    int score;
} ScoredMove;

/* Writes long algebraic notation ("e2e4", "e7e8q") into `buf`, which must hold
 * at least 6 bytes. Returns `buf`. Implemented in uci.c because the exact
 * spelling of castling depends on the UCI_Chess960 setting. */
char *move_to_str(Move m, char *buf);

#endif /* MOVE_H */
