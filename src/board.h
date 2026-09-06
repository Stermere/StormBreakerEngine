/*
 * board.h - position representation.
 *
 * Bitboards for set-wise attack maths plus a mailbox array, so "what is on this
 * square" is one load; the three mutators keep them in sync and nothing else may
 * touch them. Irreversible state cannot be recovered by playing a move backwards,
 * so do_move pushes it onto `history` and undo_move pops it.
 */
#ifndef BOARD_H
#define BOARD_H

#include "move.h"
#include "types.h"

/* do_move pushes one Undo per ply and checks the bound only in an assert, so this
 * covers the worst case: the longest competitive game ran 538 plies, and a search
 * from there adds MAX_PLY more. */
#define MAX_GAME_PLY 2048

#define FEN_STARTPOS "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"

/* FEN strings top out near 90 characters; rounded up generously. */
#define FEN_MAX_LEN 128

typedef struct {
    Key key;
    CastlingRights castling;
    Square epSquare;
    int halfmoveClock;
    Piece captured;

    /* Cached rather than recomputed: movegen wants `checkers` at every node and
     * `pinned` for every legality test, and restoring them here is cheaper than
     * paying for the slider lookups twice. */
    Bitboard checkers;
    Bitboard pinned;
} Undo;

typedef struct {
    /* byType[NO_PIECE_TYPE] is the full occupancy, so index 0 is never wasted. */
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

    /* Zobrist key over the pawns alone, maintained by the same three mutators. Pawn
     * structure is the part of a position that survives the moves a search makes,
     * which is what makes it a good key for evidence gathered across a whole tree. */
    Key pawnKey;

    Bitboard checkers;
    Bitboard pinned;

    /* Castling geometry, indexed by castling_index(). Chess960 puts the king and
     * rooks on arbitrary files, so this is derived from the diagram rather than
     * tabulated; none of it is saved in Undo, because a right is only ever removed
     * and a lost right never consults its geometry again. */
    Square castlingRook[CASTLING_NB];
    Bitboard castlingEmptyPath[CASTLING_NB];
    Bitboard castlingKingPath[CASTLING_NB];

    /* Rights lost when a piece leaves or arrives on a square. One table covers both
     * halves of the rule: the king or rook moving away, and a rook captured where it
     * stands. */
    uint8_t castlingLoss[SQUARE_NB];

    /* Plies from the root of the *game*, so repetitions are seen across the whole
     * game rather than just the search. */
    int gamePly;
    Undo history[MAX_GAME_PLY];

    /* Affects NOTATION only: generation and legality read the geometry above, which
     * is right for both variants. Not cosmetic - with the king on b1 the standard
     * spelling of O-O-O is "b1c1", which is also an ordinary king step. */
    bool chess960;
} Position;

static inline Piece piece_on(const Position *pos, Square s) { return pos->board[s]; }
static inline bool is_empty(const Position *pos, Square s) { return pos->board[s] == NO_PIECE; }

static inline Bitboard occupied_bb(const Position *pos) { return pos->byType[NO_PIECE_TYPE]; }
static inline Bitboard color_bb(const Position *pos, Color c) { return pos->byColor[c]; }

static inline Bitboard pieces_bb(const Position *pos, Color c, PieceType pt) {
    return pos->byColor[c] & pos->byType[pt];
}

/* Both types at once - sliders are almost always wanted in pairs. */
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

static inline Bitboard board_pinned(const Position *pos) { return pos->pinned; }

/* SQ_NONE if the right is absent. */
static inline Square castling_rook_square(const Position *pos, Color c, bool kingside) {
    return pos->castlingRook[castling_index(c, kingside)];
}

/*
 * The only sanctioned way to alter the board: each keeps the bitboards, mailbox,
 * piece counts and pawn key in lockstep. They deliberately do NOT touch the full
 * Zobrist key - do_move owns that, because it also folds in side to move, castling
 * and en passant, none of which these three can see.
 */
void board_put_piece(Position *pos, Piece pc, Square s);
void board_remove_piece(Position *pos, Square s);
void board_move_piece(Position *pos, Square from, Square to);

/* False, with `pos` untouched, if the FEN is malformed, so a bad `position fen`
 * from a GUI cannot corrupt state. */
bool board_set_fen(Position *pos, const char *fen);

/* As board_set_fen, but `*why` names the rejected field - "nine pawns" reads very
 * differently from "malformed FEN", and that difference is whether a user can act
 * on it. `why` may be NULL, and is untouched on success. */
bool board_set_fen_reason(Position *pos, const char *fen, const char **why);

void board_set_startpos(Position *pos);

/* Scharnagl "SP" numbering, the one tournaments and published tables use; false
 * outside 0..959. SP 518 is the standard array, which is what the self-test anchors
 * on - a numbering off by one is otherwise very hard to notice. */
bool board_set_chess960_start(Position *pos, int idx);

/* `buf` must hold at least FEN_MAX_LEN bytes. */
void board_to_fen(const Position *pos, char *buf);

/* Reference recomputation from scratch. do_move maintains the keys incrementally;
 * debug builds assert them against these. */
Key board_compute_key(const Position *pos);

Key board_compute_pawn_key(const Position *pos);

/* Board, FEN and key - the UCI `d` command. */
void board_print(const Position *pos);

/*
 * Where the king and rook end up. `rookFrom` doubles as the move's destination,
 * because castling is encoded king-captures-own-rook, and the targets are computed
 * rather than looked up so Chess960 needs no second table. It lives in the header
 * because the NNUE accumulator must derive the same two squares, and a copy that
 * drifted would put the rook's feature on the wrong square.
 */
static inline void castling_targets(Square kingFrom, Square rookFrom, Square *kingTo,
                                    Square *rookTo) {
    const bool kingside = rookFrom > kingFrom;
    *kingTo             = make_square(kingside ? FILE_G : FILE_C, rank_of(kingFrom));
    *rookTo             = make_square(kingside ? FILE_F : FILE_D, rank_of(kingFrom));
}

/* `m` must be legal, and undo_move must be passed the SAME move. do_move pushes the
 * irreversible state onto history[gamePly] before mutating anything, so undo_move
 * restores it verbatim rather than trying to derive it. */
void board_do_move(Position *pos, Move m);
void board_undo_move(Position *pos, Move m);

/* Pass the turn, for null-move pruning. Only legal when the side to move is not in
 * check. */
void board_do_null_move(Position *pos);
void board_undo_null_move(Position *pos);

/* An explicit `occupied` is what lets callers reason about x-rays: drop a piece from
 * it and the attackers behind it appear. */
Bitboard board_attackers_to(const Position *pos, Square s, Bitboard occupied);

/* Cheaper than board_attackers_to when only the yes/no answer is needed. */
bool board_square_attacked(const Position *pos, Square s, Color by, Bitboard occupied);

/* Fifty-move rule, repetition or insufficient material. `ply` is the distance from
 * the search root: one repetition inside the search already counts, while repeating
 * a position from before the root needs the full threefold count. */
bool board_is_draw(const Position *pos, int ply);

/* Validates that all representations agree. Debug builds only. */
bool board_is_consistent(const Position *pos);

#endif
