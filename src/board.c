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
 * The castling rights the diagram can actually support.
 *
 * A FEN is free to claim rights the pieces do not back, and GUIs send such
 * positions - an edited board, a truncated game, a position pasted by hand.
 * Trusting the claim is not a cosmetic error: gen_castling() consults
 * `castling` and nothing else, so an unbacked right emits a castling move,
 * and board_do_move then removes a piece from an empty square and puts a rook
 * down where none stood. The engine invents material out of nothing and every
 * search below that node is scoring a position that cannot exist.
 *
 * Standard chess only, matching the rest of this file - see the
 * TODO(chess960) in the castling field parser.
 */
static CastlingRights supported_castling(const Position *p) {
    CastlingRights rights = NO_CASTLING;

    for (Color c = WHITE; c <= BLACK; ++c) {
        const Rank home = c == WHITE ? RANK_1 : RANK_8;

        /* No king at home, no castling of either kind. */
        if (piece_on(p, make_square(FILE_E, home)) != make_piece(c, KING))
            continue;

        const Piece rook = make_piece(c, ROOK);

        if (piece_on(p, make_square(FILE_H, home)) == rook)
            rights |= c == WHITE ? WHITE_OO : BLACK_OO;
        if (piece_on(p, make_square(FILE_A, home)) == rook)
            rights |= c == WHITE ? WHITE_OOO : BLACK_OOO;
    }
    return rights;
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

bool board_set_fen(Position *pos, const char *fen) {
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
                return false; /* short rank */
            file = FILE_A;
            if (--rank < RANK_1)
                return false; /* too many ranks */
        } else if (*c >= '1' && *c <= '8') {
            file += *c - '0';
            if (file > 8)
                return false;
        } else {
            const Piece pc = piece_from_char(*c);
            if (pc == NO_PIECE || file > 7 || rank < RANK_1)
                return false;
            board_put_piece(&p, pc, make_square((File)file, (Rank)rank));
            ++file;
        }
    }
    if (rank != RANK_1 || file != 8)
        return false;

    /* --- field 2: side to move --- */
    while (*c == ' ')
        ++c;
    if (*c == 'w')
        p.sideToMove = WHITE;
    else if (*c == 'b')
        p.sideToMove = BLACK;
    else
        return false;
    ++c;

    /* --- field 3: castling rights --- */
    while (*c == ' ')
        ++c;
    p.castling = NO_CASTLING;
    if (*c == '-') {
        ++c;
    } else {
        for (; *c && *c != ' '; ++c) {
            switch (*c) {
            case 'K': p.castling |= WHITE_OO; break;
            case 'Q': p.castling |= WHITE_OOO; break;
            case 'k': p.castling |= BLACK_OO; break;
            case 'q': p.castling |= BLACK_OOO; break;
            default:
                /*
                 * TODO(chess960): Shredder-FEN spells rights as the rook's
                 * file (A-H/a-h) instead of KQkq. Rejecting them is the honest
                 * answer for now, because accepting them would produce a
                 * position this engine cannot legally play.
                 *
                 * Supporting Chess960 is three changes, in this order:
                 *   1. store the castling rook's origin square per right on
                 *      Position, parsed from either FEN spelling;
                 *   2. build the CastlingSpec table in movegen.c per position
                 *      from those squares, rather than from the fixed
                 *      standard-chess geometry it hard-codes today;
                 *   3. derive castling_targets() in board.h the same way.
                 * The move encoding is already king-captures-own-rook, so
                 * nothing above movegen has to change.
                 */
                return false;
            }
        }
    }

    /* --- field 4: en passant target --- */
    while (*c == ' ')
        ++c;
    if (*c == '-') {
        ++c;
    } else {
        if (c[0] < 'a' || c[0] > 'h' || c[1] < '1' || c[1] > '8')
            return false;
        p.epSquare = make_square((File)(c[0] - 'a'), (Rank)(c[1] - '1'));
        c += 2;
    }

    /* --- fields 5 and 6: clocks. Both optional; EPD lines routinely omit them. --- */
    p.halfmoveClock  = 0;
    p.fullmoveNumber = 1;
    if (sscanf(c, " %d %d", &p.halfmoveClock, &p.fullmoveNumber) < 2)
        p.fullmoveNumber = 1;
    if (p.halfmoveClock < 0 || p.fullmoveNumber < 1)
        return false;

    /* --- sanity: exactly one king each, or nothing downstream is meaningful --- */
    if (p.pieceCount[W_KING] != 1 || p.pieceCount[B_KING] != 1)
        return false;

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
    p.castling &= supported_castling(&p);
    if (p.epSquare != SQ_NONE && !ep_target_is_real(&p, p.epSquare))
        p.epSquare = SQ_NONE;

    p.chess960 = pos->chess960; /* preserve the UCI option across position changes */
    p.gamePly  = 0;
    p.key      = board_compute_key(&p);
    set_check_info(&p);

    /* The side that just moved may not have left its own king en prise. Such a
     * diagram is unreachable by legal play, and accepting it would let the
     * search "win" by capturing a king. */
    if (board_square_attacked(&p, king_square(&p, (Color)(p.sideToMove ^ 1)), p.sideToMove,
                              occupied_bb(&p)))
        return false;

    *pos = p;
    return true;
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
        if (pos->castling & WHITE_OO)
            *out++ = 'K';
        if (pos->castling & WHITE_OOO)
            *out++ = 'Q';
        if (pos->castling & BLACK_OO)
            *out++ = 'k';
        if (pos->castling & BLACK_OOO)
            *out++ = 'q';
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

/*
 * Castling rights lost when a piece leaves or arrives on a square. A single
 * table handles both halves of the rule at once: the king or rook moving away,
 * and the rook being captured where it stands.
 *
 * Standard chess only - board_set_fen rejects Shredder-FEN, so the king and
 * rook home squares are fixed. Chess960 will need this built per position.
 */
static const uint8_t CastlingLoss[SQUARE_NB] = {
    [SQ_A1] = WHITE_OOO, [SQ_E1] = WHITE_ANY, [SQ_H1] = WHITE_OO,
    [SQ_A8] = BLACK_OOO, [SQ_E8] = BLACK_ANY, [SQ_H8] = BLACK_OO,
};

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

    const uint8_t lost = (uint8_t)(CastlingLoss[from] | CastlingLoss[to]);
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
