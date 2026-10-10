/*
 * board.h - position representation: bitboards plus a mailbox, kept in sync by the three
 * mutators below and nothing else (invariant 5). do_move pushes irreversible state onto
 * `history`; undo_move restores it.
 */
#ifndef BOARD_H
#define BOARD_H

#include "move.h"
#include "types.h"

/* The bound is only asserted. The longest competitive game is 538 plies, plus MAX_PLY. */
#define MAX_GAME_PLY 2048

#define FEN_STARTPOS "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"

#define FEN_MAX_LEN 128

typedef struct {
    Key key;
    CastlingRights castling;
    Square epSquare;
    int halfmoveClock;
    Piece captured;
    Bitboard checkers;
    Bitboard pinned;
} Undo;

typedef struct {
    /* byType[NO_PIECE_TYPE] is the full occupancy. */
    Bitboard byType[PIECE_TYPE_NB];
    Bitboard byColor[COLOR_NB];
    Piece board[SQUARE_NB];
    int pieceCount[PIECE_NB];

    Color sideToMove;
    CastlingRights castling;
    Square epSquare;
    int halfmoveClock;
    int fullmoveNumber;

    Key key;

    /* Keys over the pawns, and over each colour's other pieces, for correction history.
     * Kept by the mutators. */
    Key pawnKey;
    Key nonPawnKey[COLOR_NB];

    Bitboard checkers;
    Bitboard pinned;

    /* Castling geometry by castling_index(), derived from the diagram for Chess960. Not
     * saved in Undo: rights are only ever removed. */
    Square castlingRook[CASTLING_NB];
    Bitboard castlingEmptyPath[CASTLING_NB];
    Bitboard castlingKingPath[CASTLING_NB];

    /* Rights lost when a piece leaves or arrives on a square. */
    uint8_t castlingLoss[SQUARE_NB];

    /* Plies since the game's root position, so repetitions span the whole game. */
    int gamePly;
    Undo history[MAX_GAME_PLY];

    /* Notation only; the rules come from the geometry. It matters: with the king on b1,
     * standard O-O-O is "b1c1", also an ordinary king step. */
    bool chess960;
} Position;

static inline Piece piece_on(const Position *pos, Square s) { return pos->board[s]; }
static inline bool is_empty(const Position *pos, Square s) { return pos->board[s] == NO_PIECE; }

static inline Bitboard occupied_bb(const Position *pos) { return pos->byType[NO_PIECE_TYPE]; }
static inline Bitboard color_bb(const Position *pos, Color c) { return pos->byColor[c]; }

static inline Bitboard pieces_bb(const Position *pos, Color c, PieceType pt) {
    return pos->byColor[c] & pos->byType[pt];
}

static inline Bitboard pieces2_bb(const Position *pos, Color c, PieceType a, PieceType b) {
    return pos->byColor[c] & (pos->byType[a] | pos->byType[b]);
}

static inline Square king_square(const Position *pos, Color c) {
    return lsb(pieces_bb(pos, c, KING));
}

static inline int piece_count(const Position *pos, Color c, PieceType pt) {
    return pos->pieceCount[make_piece(c, pt)];
}

static inline Bitboard board_checkers(const Position *pos) { return pos->checkers; }

/* SQ_NONE if the right is absent. */
static inline Square castling_rook_square(const Position *pos, Color c, bool kingside) {
    return pos->castlingRook[castling_index(c, kingside)];
}

/* The only way to alter the board (invariant 5): they keep bitboards, mailbox, counts and
 * the pawn/non-pawn keys in step. The full key is do_move's job. */
void board_put_piece(Position *pos, Piece pc, Square s);
void board_remove_piece(Position *pos, Square s);
void board_move_piece(Position *pos, Square from, Square to);

/* False, with `pos` untouched, if the FEN is malformed. */
bool board_set_fen(Position *pos, const char *fen);

/* As board_set_fen, and `*why` describes the rejection. `why` may be NULL. */
bool board_set_fen_reason(Position *pos, const char *fen, const char **why);

void board_set_startpos(Position *pos);

/* Scharnagl SP numbering, 0..959; SP 518 is the standard array. */
bool board_set_chess960_start(Position *pos, int idx);

/* Double Fischer Random: an SP number per side. */
bool board_set_dfrc_start(Position *pos, int whiteIdx, int blackIdx);

/* `buf` must hold FEN_MAX_LEN bytes. */
void board_to_fen(const Position *pos, char *buf);

/* From scratch; debug builds check the incremental keys against these. */
Key board_compute_key(const Position *pos);

Key board_compute_pawn_key(const Position *pos);

Key board_compute_non_pawn_key(const Position *pos, Color c);

/* The UCI `d` command. */
void board_print(const Position *pos);

/* Castling is encoded king-takes-own-rook, so `rookFrom` is the move's destination. Shared
 * with the NNUE delta, which must agree on the squares. */
static inline void castling_targets(Square kingFrom, Square rookFrom, Square *kingTo,
                                    Square *rookTo) {
    const bool kingside = rookFrom > kingFrom;
    *kingTo             = make_square(kingside ? FILE_G : FILE_C, rank_of(kingFrom));
    *rookTo             = make_square(kingside ? FILE_F : FILE_D, rank_of(kingFrom));
}

/* `m` must be legal, and undo_move must get the same move. */
void board_do_move(Position *pos, Move m);
void board_undo_move(Position *pos, Move m);

/* Not in check only. */
void board_do_null_move(Position *pos);
void board_undo_null_move(Position *pos);

/* Remove a piece from `occupied` to reveal the attackers behind it. */
Bitboard board_attackers_to(const Position *pos, Square s, Bitboard occupied);

bool board_square_attacked(const Position *pos, Square s, Color by, Bitboard occupied);

/* Fifty-move rule, repetition or insufficient material. `ply` is the distance from the
 * search root: a repetition inside the search counts once, before the root threefold. */
bool board_is_draw(const Position *pos, int ply);

/* After bb_init() and zobrist_init(). */
void board_cuckoo_init(void);

/* Whether the side to move has a reversible move back to a position on this line, at most
 * `limit` plies up and fewer than `ply` (inside the search). */
bool board_upcoming_repetition(const Position *pos, int ply, int limit);

/* Whether all representations agree. For debug asserts. */
bool board_is_consistent(const Position *pos);

#endif
