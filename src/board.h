/*
 * board.h - position representation.
 *
 * A hybrid representation: bitboards for set-wise attack maths, plus a mailbox
 * array so "what is on this square" is a single load rather than a scan across
 * six bitboards. Every strong engine carries both; they are kept in sync by
 * put_piece/remove_piece and nothing else may touch them directly.
 *
 * Irreversible state (castling rights, en passant square, halfmove clock, the
 * captured piece) cannot be recovered by playing a move backwards, so do_move
 * pushes it onto `history` and undo_move pops it.
 */
#ifndef BOARD_H
#define BOARD_H

#include "move.h"
#include "types.h"

/* Long enough for any realistic game plus a full search from its end. */
#define MAX_GAME_PLY 1024

#define FEN_STARTPOS "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"

/* FEN strings top out near 90 characters; round up generously. */
#define FEN_MAX_LEN 128

typedef struct {
    Key key; /* Zobrist key BEFORE the move was made */
    CastlingRights castling;
    Square epSquare;
    int halfmoveClock;
    Piece captured;

    /* TODO(movegen): cache checkers/pinned/check-squares here once movegen
     * lands. Recomputing them per node is a measurable Elo loss. */
} Undo;

typedef struct {
    /* byType[NO_PIECE_TYPE] is the full occupancy, so index 0 is never wasted. */
    Bitboard byType[PIECE_TYPE_NB];
    Bitboard byColor[COLOR_NB];
    Piece board[SQUARE_NB];
    int pieceCount[PIECE_NB];

    Color sideToMove;
    CastlingRights castling;
    Square epSquare; /* SQ_NONE when there is no en passant target */
    int halfmoveClock;
    int fullmoveNumber;

    Key key;

    /* Plies played from the root of the *game*, used to index `history` and to
     * detect repetitions across the whole game rather than just the search. */
    int gamePly;
    Undo history[MAX_GAME_PLY];

    bool chess960;
} Position;

/* ------------------------------------------------------------- accessors -- */

static inline Piece piece_on(const Position *pos, Square s) { return pos->board[s]; }
static inline bool is_empty(const Position *pos, Square s) { return pos->board[s] == NO_PIECE; }

static inline Bitboard occupied_bb(const Position *pos) { return pos->byType[NO_PIECE_TYPE]; }
static inline Bitboard color_bb(const Position *pos, Color c) { return pos->byColor[c]; }

static inline Bitboard pieces_bb(const Position *pos, Color c, PieceType pt) {
    return pos->byColor[c] & pos->byType[pt];
}

static inline Square king_square(const Position *pos, Color c) {
    return lsb(pieces_bb(pos, c, KING));
}

static inline int piece_count(const Position *pos, Color c, PieceType pt) {
    return pos->pieceCount[make_piece(c, pt)];
}

/* ------------------------------------------------------- board mutation -- */

/*
 * The ONLY sanctioned way to alter the board. Each keeps the bitboards, the
 * mailbox and the piece counts in lockstep; touching any of them directly is
 * how the three representations silently drift apart. They deliberately do NOT
 * update the Zobrist key - do_move owns that, because it also has to fold in
 * side-to-move, castling and en passant.
 */
void board_put_piece(Position *pos, Piece pc, Square s);
void board_remove_piece(Position *pos, Square s);
void board_move_piece(Position *pos, Square from, Square to);

/* --------------------------------------------------------------- setup --- */

/* Parses `fen` into `pos`. Returns false and leaves `pos` untouched if the FEN
 * is malformed, so a bad `position fen ...` from a GUI cannot corrupt state. */
bool board_set_fen(Position *pos, const char *fen);

void board_set_startpos(Position *pos);

/* Writes the position's FEN into `buf` (at least FEN_MAX_LEN bytes). */
void board_to_fen(const Position *pos, char *buf);

/* Recomputes the Zobrist key from scratch. do_move maintains the key
 * incrementally; this is the reference used to assert that it stayed correct. */
Key board_compute_key(const Position *pos);

/* Pretty-prints the board, FEN and key - the UCI `d` command. */
void board_print(const Position *pos);

/* ---------------------------------------------------------- move making -- */

/*
 * TODO(engine): the four functions below are the heart of make/unmake and are
 * intentionally unimplemented. Implement do_move/undo_move first, then run
 * `make perft` - the suite in tests/perft/ will find essentially every bug in
 * move generation and move making before it can cost you Elo.
 *
 * Requirements when you implement them:
 *   - update byType, byColor, board and pieceCount together (never one alone)
 *   - update `key` incrementally, and assert(key == board_compute_key(pos))
 *     in debug builds
 *   - push the pre-move Undo onto history[gamePly] before mutating anything
 *   - handle: captures, double pawn pushes (setting epSquare), en passant,
 *     castling (king and rook), promotions, and the halfmove clock reset
 */
void board_do_move(Position *pos, Move m);
void board_undo_move(Position *pos, Move m);

/* Null move: pass the turn. Used by null-move pruning in the search. */
void board_do_null_move(Position *pos);
void board_undo_null_move(Position *pos);

/* ----------------------------------------------------------- predicates -- */

/* TODO(engine): needed by movegen and search. */

/* Bitboard of enemy pieces giving check to `pos->sideToMove`. */
Bitboard board_checkers(const Position *pos);

/* True if `s` is attacked by any piece of colour `by`, given `occupied`.
 * Passing an explicit occupancy lets the caller test x-rays and pins. */
bool board_square_attacked(const Position *pos, Square s, Color by, Bitboard occupied);

/* True if the position is drawn by the fifty-move rule, threefold repetition,
 * or insufficient material. */
bool board_is_draw(const Position *pos);

/* Validates that all representations agree. Debug builds only - call it from
 * asserts inside do_move/undo_move while bringing move making up. */
bool board_is_consistent(const Position *pos);

#endif /* BOARD_H */
