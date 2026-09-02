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

/*
 * Long enough for any realistic game plus a full search from its end.
 *
 * do_move pushes one Undo per ply and never checks the bound outside an
 * assert, so this has to cover the worst case rather than the typical one:
 * the longest game ever played competitively ran 269 moves (538 plies), and a
 * search from that position can add MAX_PLY more. 1024 did not clear that -
 * 538 + 246 overruns it - and the overrun is a silent write past the end of
 * Position. Doubling costs 40 KB in a struct that is copied once per search.
 */
#define MAX_GAME_PLY 2048

#define FEN_STARTPOS "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"

/* FEN strings top out near 90 characters; round up generously. */
#define FEN_MAX_LEN 128

typedef struct {
    Key key; /* Zobrist key BEFORE the move was made */
    CastlingRights castling;
    Square epSquare;
    int halfmoveClock;
    Piece captured;

    /* Derived state is cached rather than recomputed: movegen needs `checkers`
     * at every node and `pinned` for every legality test, and both cost a
     * handful of slider lookups. Restoring them on undo is a memcpy-cheap way
     * to avoid paying for them twice. */
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
    Square epSquare; /* SQ_NONE when there is no en passant target */
    int halfmoveClock;
    int fullmoveNumber;

    Key key;

    /*
     * Zobrist key over the pawns alone, maintained by the same three mutators
     * that maintain the bitboards. Pawn structure is the part of a position
     * that survives the moves a search actually makes, which is what makes it
     * a useful key for anything accumulating evidence across a whole tree -
     * correction history today, a pawn evaluation cache if one is ever wanted.
     */
    Key pawnKey;

    /* Enemy pieces currently giving check to `sideToMove`, and the pieces of
     * `sideToMove` that are pinned against their own king. Maintained by
     * do_move/undo_move; see the note on Undo above. */
    Bitboard checkers;
    Bitboard pinned;

    /*
     * Castling geometry, indexed by castling_index(). Chess960 puts the king
     * and both rooks on arbitrary files, so none of this can be the constant
     * table it used to be - it is derived from the diagram by board_set_fen,
     * and for a standard position it comes out as exactly the squares that
     * table held.
     *
     * None of it is saved in Undo, because none of it changes: a right is only
     * ever REMOVED as a game goes on, and a right that is gone never consults
     * its geometry again.
     *
     * The king's castling origin is deliberately absent. A surviving right
     * means the king has not moved, so king_square() already IS that square,
     * and a second copy would be one more thing that could drift.
     */
    Square castlingRook[CASTLING_NB];        /* rook's origin; SQ_NONE without the right */
    Bitboard castlingEmptyPath[CASTLING_NB]; /* must be clear; excludes the king and rook */
    Bitboard castlingKingPath[CASTLING_NB];  /* the king's origin, crossings and destination */

    /* Rights lost when a piece leaves or arrives on a square - the king's
     * origin and the rooks'. One table covers both halves of the rule: the
     * king or rook moving away, and a rook captured where it stands. */
    uint8_t castlingLoss[SQUARE_NB];

    /* Plies played from the root of the *game*, used to index `history` and to
     * detect repetitions across the whole game rather than just the search. */
    int gamePly;
    Undo history[MAX_GAME_PLY];

    /*
     * Chess960 affects NOTATION only. Move generation and legality read the
     * geometry above, which is correct for both variants, so a Chess960
     * position is played correctly whether or not this is set; what it changes
     * is how castling is SPELLED - king-captures-own-rook rather than the
     * king's destination, and the rook's file rather than KQkq in a FEN.
     *
     * That is not cosmetic. On a board where the king starts on b1, the
     * standard spelling of O-O-O is "b1c1", which is also the spelling of an
     * ordinary king step - so on a Chess960 board the standard notation is
     * genuinely ambiguous and must not be used.
     */
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

/* Both piece types at once - sliders are almost always wanted in pairs. */
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

/* The rook a castling right belongs to, or SQ_NONE if the right is absent. */
static inline Square castling_rook_square(const Position *pos, Color c, bool kingside) {
    return pos->castlingRook[castling_index(c, kingside)];
}

/* ------------------------------------------------------- board mutation -- */

/*
 * The ONLY sanctioned way to alter the board. Each keeps the bitboards, the
 * mailbox, the piece counts and the pawn key in lockstep; touching any of them
 * directly is how the representations silently drift apart. They deliberately
 * do NOT update the full Zobrist key - do_move owns that, because it also has
 * to fold in side-to-move, castling and en passant.
 *
 * The pawn key is here rather than in do_move precisely because it needs none
 * of that. It is a function of where the pawns are, so the three ways a pawn
 * can appear, vanish or travel are the three places that have to know - and
 * undo gets it back for free, because XOR is its own inverse.
 */
void board_put_piece(Position *pos, Piece pc, Square s);
void board_remove_piece(Position *pos, Square s);
void board_move_piece(Position *pos, Square from, Square to);

/* --------------------------------------------------------------- setup --- */

/* Parses `fen` into `pos`. Returns false and leaves `pos` untouched if the FEN
 * is malformed, so a bad `position fen ...` from a GUI cannot corrupt state. */
bool board_set_fen(Position *pos, const char *fen);

void board_set_startpos(Position *pos);

/*
 * Sets up Chess960 start position `idx` under the Scharnagl "SP" numbering
 * that tournaments, GUIs and every published table use. Returns false for an
 * index outside 0..959.
 *
 * SP 518 is the standard array, which is the anchor the self-test asserts on:
 * a numbering that is off by even one is otherwise very hard to notice.
 */
bool board_set_chess960_start(Position *pos, int idx);

/* Writes the position's FEN into `buf` (at least FEN_MAX_LEN bytes). */
void board_to_fen(const Position *pos, char *buf);

/* Recomputes the Zobrist key from scratch. do_move maintains the key
 * incrementally; this is the reference used to assert that it stayed correct. */
Key board_compute_key(const Position *pos);

/* The same reference recomputation, for the pawn key the mutators maintain. */
Key board_compute_pawn_key(const Position *pos);

/* Pretty-prints the board, FEN and key - the UCI `d` command. */
void board_print(const Position *pos);

/* ---------------------------------------------------------- move making -- */

/*
 * Where the king and rook end up when castling. `rookFrom` doubles as the
 * move's destination square, because castling is encoded king-captures-own-rook.
 *
 * Correct for Chess960 unchanged, which is the whole reason the destinations
 * are computed rather than looked up: the king always finishes on the g-file
 * or the c-file and the rook beside it, however far either has to travel and
 * whichever of them started closer. Either may also finish where it already
 * stands, and either may finish on the other's origin square - see the note in
 * board_do_move about why both pieces are lifted before either is put down.
 *
 * In the header rather than in board.c because the NNUE accumulator has to
 * derive the same two squares to know which features a castle changes. Two
 * copies of this would be two things to keep in step, and a copy that drifted
 * would put the rook's feature on the wrong square - which scores plausibly
 * and loses Elo silently.
 */
static inline void castling_targets(Square kingFrom, Square rookFrom, Square *kingTo,
                                    Square *rookTo) {
    const bool kingside = rookFrom > kingFrom;
    *kingTo             = make_square(kingside ? FILE_G : FILE_C, rank_of(kingFrom));
    *rookTo             = make_square(kingside ? FILE_F : FILE_D, rank_of(kingFrom));
}

/*
 * Plays / retracts `m`, which must be legal in `pos`.
 *
 * do_move pushes the irreversible state onto history[gamePly] before mutating
 * anything, so undo_move restores it verbatim rather than trying to derive it.
 * Both maintain the Zobrist key incrementally; debug builds assert it against
 * board_compute_key().
 *
 * undo_move must be passed the SAME move that was given to do_move.
 */
void board_do_move(Position *pos, Move m);
void board_undo_move(Position *pos, Move m);

/* Null move: pass the turn. Used by null-move pruning in the search. Only
 * legal when the side to move is not in check. */
void board_do_null_move(Position *pos);
void board_undo_null_move(Position *pos);

/* ----------------------------------------------------------- predicates -- */

/* Every piece of either colour attacking `s`, given `occupied`. Passing an
 * explicit occupancy is what lets callers reason about x-rays: remove a piece
 * from `occupied` and the attackers behind it appear. */
Bitboard board_attackers_to(const Position *pos, Square s, Bitboard occupied);

/* True if `s` is attacked by any piece of colour `by`, given `occupied`.
 * Cheaper than board_attackers_to when only the yes/no answer is needed. */
bool board_square_attacked(const Position *pos, Square s, Color by, Bitboard occupied);

/* True if the position is drawn by the fifty-move rule, repetition, or
 * insufficient material. `ply` is the distance from the search root: a single
 * repetition inside the search is already a draw for search purposes, whereas
 * repeating a position that occurred before the root needs the full threefold
 * count. Pass 0 outside the search. */
bool board_is_draw(const Position *pos, int ply);

/* Validates that all representations agree. Debug builds only. */
bool board_is_consistent(const Position *pos);

#endif /* BOARD_H */
