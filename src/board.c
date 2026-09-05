/*
 * board.c - position setup, FEN I/O, hashing and move making.
 */
#include "board.h"

#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "bitboard.h"
#include "movegen.h"
#include "zobrist.h"

static const char PieceChars[PIECE_NB] = {
    [W_PAWN] = 'P',   [W_KNIGHT] = 'N', [W_BISHOP] = 'B', [W_ROOK] = 'R',
    [W_QUEEN] = 'Q',  [W_KING] = 'K',   [B_PAWN] = 'p',   [B_KNIGHT] = 'n',
    [B_BISHOP] = 'b', [B_ROOK] = 'r',   [B_QUEEN] = 'q',  [B_KING] = 'k'};

static Piece piece_from_char(char c) {
    switch (c) {
    case 'P': return W_PAWN;
    case 'N': return W_KNIGHT;
    case 'B': return W_BISHOP;
    case 'R': return W_ROOK;
    case 'Q': return W_QUEEN;
    case 'K': return W_KING;
    case 'p': return B_PAWN;
    case 'n': return B_KNIGHT;
    case 'b': return B_BISHOP;
    case 'r': return B_ROOK;
    case 'q': return B_QUEEN;
    case 'k': return B_KING;
    default: return NO_PIECE;
    }
}

/* A piece's contribution to the pawn key: its own Zobrist key if it is a pawn,
 * zero otherwise. Branch-free on purpose - these three functions are the
 * hottest code in make/unmake, and ZobristPiece[pc][s] is a line do_move is
 * about to read anyway. */
static inline Key pawn_key_term(Piece pc, Square s) {
    return ZobristPiece[pc][s] & ZobristPawnSelect[pc];
}

void board_put_piece(Position *pos, Piece pc, Square s) {
    pos->board[s] = pc;
    pos->byType[NO_PIECE_TYPE] |= square_bb(s);
    pos->byType[type_of(pc)] |= square_bb(s);
    pos->byColor[color_of(pc)] |= square_bb(s);
    pos->pieceCount[pc]++;
    pos->pawnKey ^= pawn_key_term(pc, s);
}

void board_remove_piece(Position *pos, Square s) {
    const Piece pc = pos->board[s];
    pos->byType[NO_PIECE_TYPE] &= ~square_bb(s);
    pos->byType[type_of(pc)] &= ~square_bb(s);
    pos->byColor[color_of(pc)] &= ~square_bb(s);
    pos->pieceCount[pc]--;
    pos->board[s] = NO_PIECE;
    pos->pawnKey ^= pawn_key_term(pc, s);
}

void board_move_piece(Position *pos, Square from, Square to) {
    const Piece pc      = pos->board[from];
    const Bitboard mask = square_bb(from) | square_bb(to);
    pos->byType[NO_PIECE_TYPE] ^= mask;
    pos->byType[type_of(pc)] ^= mask;
    pos->byColor[color_of(pc)] ^= mask;
    pos->board[from] = NO_PIECE;
    pos->board[to]   = pc;
    pos->pawnKey ^= pawn_key_term(pc, from) ^ pawn_key_term(pc, to);
}

/*
 * Could `capturer` actually take en passant on `ep`?
 *
 * The en passant file belongs in the hash only when the right can be
 * exercised. Folding it in regardless splits the transposition table: the same
 * position reached with a double push ahead of it hashes differently from the
 * one reached without, so neither entry can answer for the other, and every
 * such pair costs an entry and a re-search to buy a right that does not exist.
 *
 * A pawn of `capturer` can take on `ep` exactly when it stands on a square
 * that a pawn of the opposite colour standing on `ep` would attack - pawn
 * attacks reverse under a colour flip, which is what makes the one lookup
 * enough.
 *
 * Pseudo-legal on purpose: a pin can still forbid the capture. What matters is
 * that the incremental update in do_move and this function ask exactly the
 * same question, since the key's correctness is the agreement between them. A
 * sharper test would have to be sharper in both places and would buy very
 * little.
 */
static bool ep_capturable(const Position *pos, Color capturer, Square ep) {
    return (pawn_attacks((Color)(capturer ^ 1), ep) & pieces_bb(pos, capturer, PAWN)) != BB_EMPTY;
}

Key board_compute_key(const Position *pos) {
    Key k = 0;

    Bitboard occ = occupied_bb(pos);
    while (occ) {
        const Square s = pop_lsb(&occ);
        k ^= ZobristPiece[piece_on(pos, s)][s];
    }

    k ^= ZobristCastling[pos->castling];

    if (pos->epSquare != SQ_NONE && ep_capturable(pos, pos->sideToMove, pos->epSquare))
        k ^= ZobristEnPassant[file_of(pos->epSquare)];

    if (pos->sideToMove == BLACK)
        k ^= ZobristSideToMove;

    return k;
}

/*
 * Side to move is deliberately not hashed here. This key names a pawn
 * structure, and a structure is the same structure whoever is on move; the one
 * consumer that cares indexes the side separately, which keeps both halves of
 * the evidence addressable instead of splitting every entry in two.
 */
Key board_compute_pawn_key(const Position *pos) {
    Key k = 0;

    Bitboard pawns = pos->byType[PAWN];
    while (pawns) {
        const Square s = pop_lsb(&pawns);
        k ^= ZobristPiece[piece_on(pos, s)][s];
    }

    return k;
}

/* ========================================================================== *
 *  Attack and pin queries
 * ========================================================================== */

Bitboard board_attackers_to(const Position *pos, Square s, Bitboard occupied) {
    /* pawn_attacks(BLACK, s) is the set of squares a WHITE pawn would have to
     * stand on to attack s - the relation is symmetric, so the colours look
     * inverted here on purpose. */
    return (pawn_attacks(BLACK, s) & pieces_bb(pos, WHITE, PAWN)) |
           (pawn_attacks(WHITE, s) & pieces_bb(pos, BLACK, PAWN)) |
           (knight_attacks(s) & pos->byType[KNIGHT]) | (king_attacks(s) & pos->byType[KING]) |
           (bishop_attacks(s, occupied) & (pos->byType[BISHOP] | pos->byType[QUEEN])) |
           (rook_attacks(s, occupied) & (pos->byType[ROOK] | pos->byType[QUEEN]));
}

bool board_square_attacked(const Position *pos, Square s, Color by, Bitboard occupied) {
    const Color them = (Color)(by ^ 1);

    /* Cheapest and likeliest first: the leaper lookups are single loads, the
     * two slider lookups are the only real work in this function. */
    if (pawn_attacks(them, s) & pieces_bb(pos, by, PAWN))
        return true;
    if (knight_attacks(s) & pieces_bb(pos, by, KNIGHT))
        return true;
    if (king_attacks(s) & pieces_bb(pos, by, KING))
        return true;
    if (bishop_attacks(s, occupied) & pieces2_bb(pos, by, BISHOP, QUEEN))
        return true;
    return (rook_attacks(s, occupied) & pieces2_bb(pos, by, ROOK, QUEEN)) != 0;
}

/* Pieces of `us` that are the sole blocker between their own king and an enemy
 * slider. Moving one off the pinning line exposes the king, which is precisely
 * the test movegen_is_legal has to make. */
static Bitboard compute_pinned(const Position *pos, Color us, Square ksq) {
    const Color them   = (Color)(us ^ 1);
    const Bitboard occ = occupied_bb(pos);
    Bitboard pinned    = BB_EMPTY;

    /* Only sliders that would attack the king on an EMPTY board can pin, so
     * the per-slider work below runs a handful of times, not sixteen. */
    Bitboard snipers = (rook_attacks(ksq, BB_EMPTY) & pieces2_bb(pos, them, ROOK, QUEEN)) |
                       (bishop_attacks(ksq, BB_EMPTY) & pieces2_bb(pos, them, BISHOP, QUEEN));

    while (snipers) {
        const Square sniper  = pop_lsb(&snipers);
        const Bitboard block = SquaresBetween[ksq][sniper] & occ;

        /* bb_at_most_one() is also true for an empty set, which would be a
         * check rather than a pin - hence the explicit non-empty test. */
        if (block && bb_at_most_one(block))
            pinned |= block & color_bb(pos, us);
    }
    return pinned;
}

/* Refreshes the cached derived state for whoever is to move. Must be called
 * after every change to the board. */
static void set_check_info(Position *pos) {
    const Color us   = pos->sideToMove;
    const Square ksq = king_square(pos, us);

    pos->checkers = board_attackers_to(pos, ksq, occupied_bb(pos)) & color_bb(pos, (Color)(us ^ 1));
    pos->pinned   = compute_pinned(pos, us, ksq);
}

/*
 * Grants one castling right and derives everything movegen needs from it.
 *
 * `rookSq` is where that side's castling rook stands; the king's origin is
 * wherever the king stands, because a right can only exist if it has not
 * moved. The two paths are what the standard-chess constant table used to
 * hold, computed instead:
 *
 *   kingPath   every square the king occupies or crosses, INCLUDING its
 *              origin - which is what makes the attack scan in
 *              movegen_is_legal reject castling out of check for free.
 *   emptyPath  every square that must be unoccupied: both travel lanes MINUS
 *              the king's and rook's own squares, which are allowed to be
 *              occupied by the very two pieces that are moving.
 *
 * In Chess960 the lanes overlap in ways standard chess never shows - the king
 * may not move at all, the rook may already stand where the king is going - so
 * both are built as set unions rather than as spans, and the subtraction at
 * the end is what stops a castle from blocking itself.
 */
static void grant_castling(Position *p, Color c, Square rookSq) {
    const Square ksq    = king_square(p, c);
    const bool kingside = rookSq > ksq;
    const int idx       = castling_index(c, kingside);

    /* A malformed field can name two rooks on the same side of the king
     * ("BA"), which is not a position Chess960 can reach. First claim wins,
     * rather than overwriting: a second grant would leave castlingLoss holding
     * revocation bits for a square that is no longer the castling rook's. */
    if (p->castlingRook[idx] != SQ_NONE)
        return;

    Square kingTo, rookTo;
    castling_targets(ksq, rookSq, &kingTo, &rookTo);

    p->castling |= castling_right(c, kingside);
    p->castlingRook[idx]      = rookSq;
    p->castlingKingPath[idx]  = SquaresBetween[ksq][kingTo] | square_bb(ksq) | square_bb(kingTo);
    p->castlingEmptyPath[idx] = (SquaresBetween[ksq][kingTo] | square_bb(kingTo) |
                                 SquaresBetween[rookSq][rookTo] | square_bb(rookTo)) &
                                ~(square_bb(ksq) | square_bb(rookSq));

    p->castlingLoss[ksq] |= (uint8_t)castling_right(c, kingside);
    p->castlingLoss[rookSq] |= (uint8_t)castling_right(c, kingside);
}

/*
 * The outermost rook of colour `c` on the given side of its king, or SQ_NONE.
 *
 * This is what X-FEN's KQkq mean on a Chess960 board: not the rook on h1, but
 * the rook furthest from the king on that side. Scanning inward from the edge
 * and stopping at the king's file returns exactly that, and on a standard
 * board it returns h1 or a1 - the fixed lookup this replaces was the special
 * case where the outermost rook is also the only one.
 */
static Square outermost_rook(const Position *p, Color c, Square ksq, bool kingside) {
    const Rank home  = rank_of(ksq);
    const Piece rook = make_piece(c, ROOK);
    const int step   = kingside ? -1 : 1;

    for (int f = kingside ? FILE_H : FILE_A; f != (int)file_of(ksq); f += step)
        if (piece_on(p, make_square((File)f, home)) == rook)
            return make_square((File)f, home);

    return SQ_NONE;
}

/*
 * Resolves the FEN castling field into rights and geometry.
 *
 * Two spellings arrive and they mean the same thing:
 *
 *   KQkq   X-FEN. Names the outermost rook on each side of the king. Every
 *          standard-chess FEN is this, and so is most Chess960 output.
 *   AHah   Shredder-FEN. Names the rook's file outright. Needed whenever a
 *          colour has two rooks on one side of its king, where the outermost
 *          one is not enough to say which of them may castle.
 *
 * A claimed right the diagram cannot back is DROPPED rather than rejected, for
 * the reason spelled out at the call site - and it matters more here than in
 * standard chess, because a Chess960 castling field that disagrees with the
 * piece placement is routine output from position editors.
 *
 * Dropping is not cosmetic. gen_castling() consults `castling` and the
 * geometry beside it and nothing else, so an unbacked right emits a castling
 * move, and board_do_move then lifts a piece off an empty square and puts a
 * rook down where none stood. The engine invents material out of nothing, and
 * every search below that node scores a position that cannot exist.
 */
static void resolve_castling(Position *p, const char *field) {
    for (const char *c = field; *c; ++c) {
        const Color col  = islower((unsigned char)*c) ? BLACK : WHITE;
        const char u     = (char)toupper((unsigned char)*c);
        const Square ksq = king_square(p, col);

        /* Every derivation below starts from where the king stands, so a king
         * that is not on its back rank cannot hold a right at all. */
        if (rank_of(ksq) != (col == WHITE ? RANK_1 : RANK_8))
            continue;

        Square rookSq;
        if (u == 'K' || u == 'Q') {
            rookSq = outermost_rook(p, col, ksq, u == 'K');
        } else {
            rookSq = make_square((File)(u - 'A'), rank_of(ksq));
            if (piece_on(p, rookSq) != make_piece(col, ROOK))
                rookSq = SQ_NONE;
        }

        if (rookSq != SQ_NONE)
            grant_castling(p, col, rookSq);
    }
}

/*
 * Whether this position's castling can only be spelled the Chess960 way.
 *
 * A king off the e-file, or a castling rook off the a- or h-file, cannot be
 * written as KQkq without losing information, and its castling moves cannot be
 * written as king destinations without colliding with ordinary king steps.
 * Latching `chess960` on when we see one means a GUI that sends such a FEN
 * without having set UCI_Chess960 still gets moves it can read back.
 *
 * The converse is not detectable and is not attempted: a Chess960 game in
 * which both sides have already castled leaves a diagram indistinguishable
 * from a standard one, which is exactly why the UCI option exists at all.
 */
static bool castling_is_nonstandard(const Position *p) {
    for (Color c = WHITE; c <= BLACK; ++c) {
        const Rank home = c == WHITE ? RANK_1 : RANK_8;

        if (!(p->castling & (c == WHITE ? WHITE_ANY : BLACK_ANY)))
            continue;
        if (king_square(p, c) != make_square(FILE_E, home))
            return true;

        const Square oo  = castling_rook_square(p, c, true);
        const Square ooo = castling_rook_square(p, c, false);
        if (oo != SQ_NONE && oo != make_square(FILE_H, home))
            return true;
        if (ooo != SQ_NONE && ooo != make_square(FILE_A, home))
            return true;
    }
    return false;
}

/*
 * Whether an en passant target describes a double push that really happened.
 *
 * Same class of problem as the castling rights above, and the same
 * consequence. gen_pawn_moves() emits an en passant capture whenever a pawn
 * stands beside the target square, without checking that there is anything to
 * capture; do_move then removes the "captured" pawn from a square that may be
 * empty, which decrements pieceCount[NO_PIECE] and folds a pawn that was never
 * there into the Zobrist key. The key then disagrees with
 * board_compute_key(), which poisons every transposition table entry the
 * search writes from that position onwards.
 *
 * The target sits on the sixth rank from the mover's side whichever colour is
 * to move, so one expression covers both.
 */
static bool ep_target_is_real(const Position *p, Square ep) {
    const Color us      = p->sideToMove;
    const int up        = pawn_push(us);
    const Square pawnSq = (Square)(ep - up); /* where the double-pushed pawn sits */
    const Square fromSq = (Square)(ep + up); /* where it started */

    if (relative_rank(us, ep) != RANK_6)
        return false;

    return piece_on(p, pawnSq) == make_piece((Color)(us ^ 1), PAWN) && is_empty(p, ep) &&
           is_empty(p, fromSq);
}

/*
 * Every rejection below names the field it rejected on, in terms of the
 * diagram rather than of the parser: a user who typed nine pawns is helped by
 * being told they typed nine pawns, and not at all by "invalid fen".
 */
static bool fen_reject(const char **why, const char *reason) {
    if (why)
        *why = reason;
    return false;
}

bool board_set_fen_reason(Position *pos, const char *fen, const char **why) {
    /* Parse into scratch so a malformed FEN from a GUI leaves `pos` intact. */
    Position p;
    memset(&p, 0, sizeof(p));
    for (Square s = SQ_A1; s <= SQ_H8; ++s)
        p.board[s] = NO_PIECE;
    p.epSquare = SQ_NONE;

    const char *c = fen;
    while (*c == ' ')
        ++c;

    /* --- field 1: piece placement, rank 8 first --- */
    int file = FILE_A;
    int rank = RANK_8;
    for (; *c && *c != ' '; ++c) {
        if (*c == '/') {
            if (file != 8)
                return fen_reject(why, "a rank of the diagram does not add up to eight squares");
            file = FILE_A;
            if (--rank < RANK_1)
                return fen_reject(why, "the diagram has more than eight ranks");
        } else if (*c >= '1' && *c <= '8') {
            file += *c - '0';
            if (file > 8)
                return fen_reject(why, "a run of empty squares overflows its rank");
        } else {
            const Piece pc = piece_from_char(*c);
            if (pc == NO_PIECE || file > 7 || rank < RANK_1)
                return fen_reject(why, "the diagram contains a character that is not a piece");
            board_put_piece(&p, pc, make_square((File)file, (Rank)rank));
            ++file;
        }
    }
    if (rank != RANK_1 || file != 8)
        return fen_reject(why, "the diagram stops short of all sixty-four squares");

    /* --- field 2: side to move --- */
    while (*c == ' ')
        ++c;
    if (*c == 'w')
        p.sideToMove = WHITE;
    else if (*c == 'b')
        p.sideToMove = BLACK;
    else
        return fen_reject(why, "the side to move is neither 'w' nor 'b'");
    ++c;

    /*
     * --- field 3: castling rights ---
     *
     * Only copied out and validated as a character set here. Resolving it
     * needs the kings located, and locating them needs the piece-count check
     * further down to have passed - so the field is held until then rather
     * than parsed in place.
     */
    while (*c == ' ')
        ++c;
    p.castling = NO_CASTLING;
    for (int i = 0; i < CASTLING_NB; ++i)
        p.castlingRook[i] = SQ_NONE;

    char castlingField[16] = {0};
    size_t castlingLen     = 0;
    bool shredderSpelling  = false;

    if (*c == '-') {
        ++c;
    } else {
        for (; *c && *c != ' '; ++c) {
            const char u = (char)toupper((unsigned char)*c);

            if (u >= 'A' && u <= 'H' && u != 'K' && u != 'Q')
                shredderSpelling = true;
            else if (u != 'K' && u != 'Q')
                return fen_reject(why,
                                  "the castling field contains a character that is not a right");

            /* A field longer than the four real rights is malformed, and
             * silently truncating it would accept a FEN we cannot describe. */
            if (castlingLen + 1 >= sizeof(castlingField))
                return fen_reject(why, "the castling field is longer than the rights it can spell");
            castlingField[castlingLen++] = *c;
        }
    }

    /* --- field 4: en passant target --- */
    while (*c == ' ')
        ++c;
    if (*c == '-') {
        ++c;
    } else {
        if (c[0] < 'a' || c[0] > 'h' || c[1] < '1' || c[1] > '8')
            return fen_reject(why, "the en passant field is not a square");
        p.epSquare = make_square((File)(c[0] - 'a'), (Rank)(c[1] - '1'));
        c += 2;
    }

    /* --- fields 5 and 6: clocks. Both optional; EPD lines routinely omit them. --- */
    p.halfmoveClock  = 0;
    p.fullmoveNumber = 1;
    if (sscanf(c, " %d %d", &p.halfmoveClock, &p.fullmoveNumber) < 2)
        p.fullmoveNumber = 1;
    if (p.halfmoveClock < 0 || p.fullmoveNumber < 1)
        return fen_reject(why, "the halfmove or fullmove clock is negative");

    /* --- sanity: exactly one king each, or nothing downstream is meaningful --- */
    if (p.pieceCount[W_KING] != 1 || p.pieceCount[B_KING] != 1)
        return fen_reject(why, "the diagram does not have exactly one king a side");

    /*
     * At most a full board.
     *
     * This is a gate on what the ENGINE can represent, not on what chess
     * allows, and the two are deliberately different. A position editor, a
     * puzzle or a hand-typed FEN routinely produces a diagram no legal game
     * could reach - nine pawns, five knights, a pawn on the seventh with the
     * back rank still full - and every one of those is a position the engine
     * can play perfectly well. Refusing them buys nothing and costs a user
     * their analysis, so the only thing checked is that the diagram fits in
     * what the consumers are sized for.
     *
     * Sixty-four is the whole of the 8x8 grammar, so this rejects nothing a
     * FEN can actually spell; it is here because the things downstream are
     * sized by it rather than by the board:
     *
     *   - nnue_accumulate() collects one feature row per occupied square, and
     *     its array is sized to match this exactly.
     *   - tools/export_net.py proves the int16 accumulator cannot wrap using
     *     the same count. The pinned net reaches 29,143 of int16's 32,767 at
     *     sixty-four men, so the proof holds - but it is a proof about THIS
     *     many pieces, and lowering this number is the supported way out if a
     *     future net cannot clear the bound.
     *   - MAX_MOVES bounds the move list. Annealing over placement and piece
     *     type tops out at 270 pseudo-legal moves anywhere in this range,
     *     because past about thirty men the pieces block each other faster
     *     than they add moves.
     *
     * Occupancy never grows during a search - a promotion swaps a piece and a
     * capture removes one - so a diagram that passes here stays inside every
     * one of those bounds for the whole tree.
     */
    if (popcount(occupied_bb(&p)) > 64)
        return fen_reject(why, "the diagram has more men than the board has squares");

    /*
     * Discard castling rights and an en passant target the diagram does not
     * back, rather than rejecting the whole FEN.
     *
     * Dropping is deliberate where the placement parser rejects. A wrong piece
     * layout means the sender and the engine disagree about the position and
     * there is nothing safe to do with it; a stale castling right or en
     * passant square is routine output from position editors and PGN tools,
     * and the position they describe is perfectly playable once the
     * unsupportable claim is removed. Both must happen before the key is
     * computed - they are part of what it hashes.
     */
    resolve_castling(&p, castlingField);
    if (p.epSquare != SQ_NONE && !ep_target_is_real(&p, p.epSquare))
        p.epSquare = SQ_NONE;

    /* Sticky, never cleared here: the UCI option carries across position
     * changes, and a FEN that proves the board is Chess960 - by its geometry,
     * or just by spelling its rights the Shredder way - latches it on for a
     * GUI that forgot to. Notation is the only thing this controls; the rules
     * come from the geometry resolve_castling() just built either way. */
    p.chess960 = pos->chess960 || shredderSpelling || castling_is_nonstandard(&p);
    p.gamePly  = 0;
    p.key      = board_compute_key(&p);
    set_check_info(&p);

    /* The side that just moved may not have left its own king en prise. Such a
     * diagram is unreachable by legal play, and accepting it would let the
     * search "win" by capturing a king. */
    if (board_square_attacked(&p, king_square(&p, (Color)(p.sideToMove ^ 1)), p.sideToMove,
                              occupied_bb(&p)))
        return fen_reject(why, "the side that just moved has left its own king in check");

    *pos = p;
    return true;
}

bool board_set_fen(Position *pos, const char *fen) { return board_set_fen_reason(pos, fen, NULL); }

/*
 * The ten ways to arrange K, R, N on five squares with the king between the
 * rooks - which is the whole of the Chess960 castling constraint, since the
 * bishops and queen are placed before these five squares are chosen.
 *
 * Table order is Scharnagl's, and it is the numbering, not an implementation
 * choice: permuting these rows renumbers all 960 positions and silently
 * disagrees with every GUI and published table. board_set_chess960_start's
 * SP 518 assertion in the self-test is what pins it down.
 */
static const char *const KrnPatterns[10] = {"NNRKR", "NRNKR", "NRKNR", "NRKRN", "RNNKR",
                                            "RNKNR", "RNKRN", "RKNNR", "RKNRN", "RKRNN"};

bool board_set_chess960_start(Position *pos, int idx) {
    if (idx < 0 || idx >= 960)
        return false;

    char back[9];
    memset(back, ' ', 8);
    back[8] = '\0';

    /* Scharnagl's derivation, in the order the numbering is defined:
     * light-squared bishop, dark-squared bishop, queen into the n-th square
     * still free, then the K/R/N pattern into the five that remain. The two
     * bishop files are 2n+1 and 2n, which is what puts one on each colour. */
    int n                 = idx;
    back[2 * (n % 4) + 1] = 'B';
    n /= 4;
    back[2 * (n % 4)] = 'B';
    n /= 4;

    for (int f = 0, q = n % 6; f < 8; ++f)
        if (back[f] == ' ' && q-- == 0) {
            back[f] = 'Q';
            break;
        }
    n /= 6;

    for (int f = 0, k = 0; f < 8; ++f)
        if (back[f] == ' ')
            back[f] = KrnPatterns[n][k++];

    char front[9];
    for (int f = 0; f < 8; ++f)
        front[f] = (char)tolower((unsigned char)back[f]);
    front[8] = '\0';

    /* Spelled the Shredder way even for SP 518, whose geometry is standard: a
     * Chess960 game is being set up, and its castling should read back as one
     * however ordinary the array happens to look. */
    const char *const first = strchr(back, 'R');
    const char *const last  = strrchr(back, 'R');
    const char aSide        = (char)('A' + (first - back));
    const char hSide        = (char)('A' + (last - back));

    char fen[FEN_MAX_LEN];
    snprintf(fen, sizeof(fen), "%s/pppppppp/8/8/8/8/PPPPPPPP/%s w %c%c%c%c - 0 1", front, back,
             hSide, aSide, (char)tolower((unsigned char)hSide),
             (char)tolower((unsigned char)aSide));

    return board_set_fen(pos, fen);
}

void board_set_startpos(Position *pos) { board_set_fen(pos, FEN_STARTPOS); }

void board_to_fen(const Position *pos, char *buf) {
    char *out = buf;

    for (int r = RANK_8; r >= RANK_1; --r) {
        int empty = 0;
        for (int f = FILE_A; f <= FILE_H; ++f) {
            const Piece pc = piece_on(pos, make_square((File)f, (Rank)r));
            if (pc == NO_PIECE) {
                ++empty;
                continue;
            }
            if (empty) {
                *out++ = (char)('0' + empty);
                empty  = 0;
            }
            *out++ = PieceChars[pc];
        }
        if (empty)
            *out++ = (char)('0' + empty);
        if (r != RANK_1)
            *out++ = '/';
    }

    *out++ = ' ';
    *out++ = pos->sideToMove == WHITE ? 'w' : 'b';
    *out++ = ' ';

    if (pos->castling == NO_CASTLING) {
        *out++ = '-';
    } else {
        /* Shredder spelling - the castling rook's file - for a Chess960
         * position, KQkq otherwise. KQkq can only name the OUTERMOST rook on
         * each side, so on a board with two rooks on one side of the king it
         * cannot describe the position at all; emitting it there would produce
         * a FEN that does not read back as the position it was written from. */
        static const char Standard[CASTLING_NB] = {'K', 'Q', 'k', 'q'};

        for (int i = 0; i < CASTLING_NB; ++i) {
            if (!(pos->castling & (CastlingRights)(1 << i)))
                continue;

            if (!pos->chess960) {
                *out++ = Standard[i];
            } else {
                const char f = (char)('A' + file_of(pos->castlingRook[i]));
                *out++       = i < 2 ? f : (char)(f - 'A' + 'a');
            }
        }
    }

    *out++ = ' ';
    if (pos->epSquare == SQ_NONE) {
        *out++ = '-';
    } else {
        *out++ = (char)('a' + file_of(pos->epSquare));
        *out++ = (char)('1' + rank_of(pos->epSquare));
    }

    sprintf(out, " %d %d", pos->halfmoveClock, pos->fullmoveNumber);
}

void board_print(const Position *pos) {
    char fen[FEN_MAX_LEN];

    for (int r = RANK_8; r >= RANK_1; --r) {
        printf("  +---+---+---+---+---+---+---+---+\n%d ", r + 1);
        for (int f = FILE_A; f <= FILE_H; ++f) {
            const Piece pc = piece_on(pos, make_square((File)f, (Rank)r));
            printf("| %c ", pc == NO_PIECE ? ' ' : PieceChars[pc]);
        }
        printf("|\n");
    }
    printf("  +---+---+---+---+---+---+---+---+\n    a   b   c   d   e   f   g   h\n\n");

    board_to_fen(pos, fen);
    printf("Fen: %s\n", fen);
    printf("Key: %016llX\n", (unsigned long long)pos->key);
}

bool board_is_consistent(const Position *pos) {
    Bitboard occ = BB_EMPTY;

    for (PieceType pt = PAWN; pt <= KING; ++pt) {
        if (pos->byType[pt] & occ)
            return false; /* two piece types claim the same square */
        occ |= pos->byType[pt];
    }
    if (occ != occupied_bb(pos))
        return false;
    if ((pos->byColor[WHITE] & pos->byColor[BLACK]) != BB_EMPTY)
        return false;
    if ((pos->byColor[WHITE] | pos->byColor[BLACK]) != occ)
        return false;

    for (Square s = SQ_A1; s <= SQ_H8; ++s) {
        const Piece pc = piece_on(pos, s);
        if (pc == NO_PIECE) {
            if (bb_test(occ, s))
                return false;
        } else {
            if (!bb_test(pos->byType[type_of(pc)] & pos->byColor[color_of(pc)], s))
                return false;
        }
    }

    return pos->key == board_compute_key(pos) && pos->pawnKey == board_compute_pawn_key(pos);
}

/* ========================================================================== *
 *  Move making
 * ========================================================================== */

void board_do_move(Position *pos, Move m) {
    assert(is_ok_move(m));
    assert(pos->gamePly < MAX_GAME_PLY);

    const Color us    = pos->sideToMove;
    const Color them  = (Color)(us ^ 1);
    const Square from = from_sq(m);
    const Square to   = to_sq(m);
    const MoveType mt = type_of_move(m);
    const Piece pc    = piece_on(pos, from);

    assert(pc != NO_PIECE && color_of(pc) == us);

    Undo *const u    = &pos->history[pos->gamePly++];
    u->key           = pos->key;
    u->castling      = pos->castling;
    u->epSquare      = pos->epSquare;
    u->halfmoveClock = pos->halfmoveClock;
    u->checkers      = pos->checkers;
    u->pinned        = pos->pinned;

    Key k = pos->key ^ ZobristSideToMove;

    /* The en passant right expires after exactly one ply, whatever happens.
     * Only unhash it if it was hashed: `us` is the side that could have taken,
     * which is the side ep_capturable() was asked about when it went in. */
    if (pos->epSquare != SQ_NONE) {
        if (ep_capturable(pos, us, pos->epSquare))
            k ^= ZobristEnPassant[file_of(pos->epSquare)];
        pos->epSquare = SQ_NONE;
    }

    ++pos->halfmoveClock;

    if (mt == MT_CASTLING) {
        Square kingTo, rookTo;
        castling_targets(from, to, &kingTo, &rookTo);
        const Piece rook = make_piece(us, ROOK);

        assert(piece_on(pos, to) == rook);

        /* Both pieces come off before either goes down: in Chess960 a king's
         * destination can be the rook's origin and vice versa, so any
         * move-then-move ordering would clobber a square. */
        board_remove_piece(pos, from);
        board_remove_piece(pos, to);
        board_put_piece(pos, pc, kingTo);
        board_put_piece(pos, rook, rookTo);

        k ^= ZobristPiece[pc][from] ^ ZobristPiece[pc][kingTo] ^ ZobristPiece[rook][to] ^
             ZobristPiece[rook][rookTo];

        u->captured = NO_PIECE;
    } else {
        const Piece captured = mt == MT_EN_PASSANT ? make_piece(them, PAWN) : piece_on(pos, to);
        u->captured          = captured;

        if (captured != NO_PIECE) {
            /* En passant takes the pawn that double-pushed, which sits beside
             * the capturing pawn rather than on its destination square. */
            const Square capsq = mt == MT_EN_PASSANT ? (Square)(to - pawn_push(us)) : to;

            assert(piece_on(pos, capsq) == captured);
            assert(type_of(captured) != KING);

            board_remove_piece(pos, capsq);
            k ^= ZobristPiece[captured][capsq];
            pos->halfmoveClock = 0;
        }

        if (mt == MT_PROMOTION) {
            const Piece promoted = make_piece(us, promotion_type(m));
            board_remove_piece(pos, from);
            board_put_piece(pos, promoted, to);
            k ^= ZobristPiece[pc][from] ^ ZobristPiece[promoted][to];
        } else {
            board_move_piece(pos, from, to);
            k ^= ZobristPiece[pc][from] ^ ZobristPiece[pc][to];
        }

        if (type_of(pc) == PAWN) {
            pos->halfmoveClock = 0;

            /* A double push is the only move that creates an ep target, and
             * the only one whose origin and destination differ by two ranks. */
            if ((from ^ to) == 16) {
                pos->epSquare = (Square)((from + to) / 2);

                /* Recorded either way - the FEN has to show it - but hashed
                 * only when `them` can actually answer it. A double push that
                 * no pawn is standing next to leaves the position identical to
                 * one reached any other way, and the key should say so. */
                if (ep_capturable(pos, them, pos->epSquare))
                    k ^= ZobristEnPassant[file_of(pos->epSquare)];
            }
        }
    }

    /* pos->castlingLoss is per-position because Chess960 moves the king's and
     * rooks' origins off their standard squares; grant_castling() fills it.
     *
     * Only the ORIGIN squares need consulting, never the destinations. The
     * king moves in every castle and its square carries both of its colour's
     * rights, so a Chess960 castle that lands a rook on the other rook's
     * origin has already given up that right through `from`. */
    const uint8_t lost = (uint8_t)(pos->castlingLoss[from] | pos->castlingLoss[to]);
    if (pos->castling & lost) {
        k ^= ZobristCastling[pos->castling];
        pos->castling = (CastlingRights)(pos->castling & ~lost);
        k ^= ZobristCastling[pos->castling];
    }

    pos->key        = k;
    pos->sideToMove = them;
    if (us == BLACK)
        ++pos->fullmoveNumber;

    set_check_info(pos);

    assert(pos->key == board_compute_key(pos));
    assert(board_is_consistent(pos));
}

void board_undo_move(Position *pos, Move m) {
    assert(is_ok_move(m));
    assert(pos->gamePly > 0);

    const Color us    = (Color)(pos->sideToMove ^ 1); /* the side that made the move */
    const Square from = from_sq(m);
    const Square to   = to_sq(m);
    const MoveType mt = type_of_move(m);

    const Undo *const u = &pos->history[--pos->gamePly];

    if (mt == MT_CASTLING) {
        Square kingTo, rookTo;
        castling_targets(from, to, &kingTo, &rookTo);

        board_remove_piece(pos, kingTo);
        board_remove_piece(pos, rookTo);
        board_put_piece(pos, make_piece(us, KING), from);
        board_put_piece(pos, make_piece(us, ROOK), to);
    } else {
        if (mt == MT_PROMOTION) {
            board_remove_piece(pos, to);
            board_put_piece(pos, make_piece(us, PAWN), from);
        } else {
            board_move_piece(pos, to, from);
        }

        if (u->captured != NO_PIECE) {
            const Square capsq = mt == MT_EN_PASSANT ? (Square)(to - pawn_push(us)) : to;
            board_put_piece(pos, u->captured, capsq);
        }
    }

    /* Irreversible state is restored verbatim rather than derived: that is the
     * whole reason it was pushed in the first place. */
    pos->key           = u->key;
    pos->castling      = u->castling;
    pos->epSquare      = u->epSquare;
    pos->halfmoveClock = u->halfmoveClock;
    pos->checkers      = u->checkers;
    pos->pinned        = u->pinned;
    pos->sideToMove    = us;
    if (us == BLACK)
        --pos->fullmoveNumber;

    assert(board_is_consistent(pos));
}

void board_do_null_move(Position *pos) {
    assert(pos->checkers == BB_EMPTY);
    assert(pos->gamePly < MAX_GAME_PLY);

    Undo *const u    = &pos->history[pos->gamePly++];
    u->key           = pos->key;
    u->castling      = pos->castling;
    u->epSquare      = pos->epSquare;
    u->halfmoveClock = pos->halfmoveClock;
    u->captured      = NO_PIECE;
    u->checkers      = pos->checkers;
    u->pinned        = pos->pinned;

    Key k = pos->key ^ ZobristSideToMove;
    if (pos->epSquare != SQ_NONE) {
        if (ep_capturable(pos, pos->sideToMove, pos->epSquare))
            k ^= ZobristEnPassant[file_of(pos->epSquare)];
        pos->epSquare = SQ_NONE;
    }

    pos->key        = k;
    pos->sideToMove = (Color)(pos->sideToMove ^ 1);
    ++pos->halfmoveClock;

    /* The board did not change, but the pins did - they are relative to the
     * king of whoever is now to move. Checkers stay empty: passing cannot give
     * check, and a null move is only legal when not already in check. */
    set_check_info(pos);

    assert(pos->key == board_compute_key(pos));
}

void board_undo_null_move(Position *pos) {
    assert(pos->gamePly > 0);

    const Undo *const u = &pos->history[--pos->gamePly];

    pos->key           = u->key;
    pos->castling      = u->castling;
    pos->epSquare      = u->epSquare;
    pos->halfmoveClock = u->halfmoveClock;
    pos->checkers      = u->checkers;
    pos->pinned        = u->pinned;
    pos->sideToMove    = (Color)(pos->sideToMove ^ 1);
}

/* ========================================================================== *
 *  Draw detection
 * ========================================================================== */

/* Material from which no mate exists at all, so no amount of search can find
 * one. Deliberately conservative: king and two knights is NOT included,
 * because mate is possible there with cooperation - that ending is drawn by
 * the fifty-move count, not by this rule.  */
static bool insufficient_material(const Position *pos) {
    if (pos->byType[PAWN] | pos->byType[ROOK] | pos->byType[QUEEN])
        return false;

    const Bitboard minors = pos->byType[KNIGHT] | pos->byType[BISHOP];

    if (bb_at_most_one(minors))
        return true; /* K v K, and K plus one minor v K */

    /* One bishop each, both on the same colour complex: neither can ever
     * attack the other's squares, so no mating net exists. */
    if (piece_count(pos, WHITE, BISHOP) == 1 && piece_count(pos, BLACK, BISHOP) == 1 &&
        minors == pos->byType[BISHOP])
        return (minors & BB_LIGHT_SQUARES) == minors || (minors & BB_DARK_SQUARES) == minors;

    return false;
}

/*
 * `ply` is the distance from the search root.
 *
 * A position repeated INSIDE the search is scored as a draw on the FIRST
 * repetition: the side to move has demonstrably forced the cycle once and can
 * force it again, so making it prove the point a second time only costs
 * search. A position repeated from the game history before the root still
 * needs the full threefold count, because the opponent had a chance to
 * deviate and did not.
 */
static bool is_repetition(const Position *pos, int ply) {
    /* Nothing before the last irreversible move can repeat, and coming back
     * round to the same position takes at least four plies. */
    const int back = pos->halfmoveClock < pos->gamePly ? pos->halfmoveClock : pos->gamePly;
    int seen       = 0;

    for (int i = 4; i <= back; i += 2) { /* only every second ply has us to move */
        if (pos->history[pos->gamePly - i].key != pos->key)
            continue;
        if (++seen + (ply > i ? 1 : 0) >= 2)
            return true;
    }
    return false;
}

bool board_is_draw(const Position *pos, int ply) {
    if (pos->halfmoveClock > 99) {
        /* Checkmate outranks the fifty-move rule, so the position is only
         * drawn if the side to move actually has a legal reply. */
        if (pos->checkers == BB_EMPTY)
            return true;

        ScoredMove moves[MAX_MOVES];
        const int count = movegen_generate(pos, GEN_EVASIONS, moves);
        for (int i = 0; i < count; ++i)
            if (movegen_is_legal(pos, moves[i].m))
                return true;
        return false;
    }

    return insufficient_material(pos) || is_repetition(pos, ply);
}
