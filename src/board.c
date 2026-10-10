/* board.c - position setup, FEN I/O, hashing and move making. */
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

/* A piece's share of the pawn and non-pawn keys, branch-free for the hot mutators. */
static inline Key pawn_key_term(Piece pc, Square s) {
    return ZobristPiece[pc][s] & ZobristPawnSelect[pc];
}

static inline Key non_pawn_key_term(Piece pc, Square s) {
    return ZobristPiece[pc][s] & ~ZobristPawnSelect[pc];
}

void board_put_piece(Position *pos, Piece pc, Square s) {
    pos->board[s] = pc;
    pos->byType[NO_PIECE_TYPE] |= square_bb(s);
    pos->byType[type_of(pc)] |= square_bb(s);
    pos->byColor[color_of(pc)] |= square_bb(s);
    pos->pieceCount[pc]++;
    pos->pawnKey ^= pawn_key_term(pc, s);
    pos->nonPawnKey[color_of(pc)] ^= non_pawn_key_term(pc, s);
}

void board_remove_piece(Position *pos, Square s) {
    const Piece pc = pos->board[s];
    pos->byType[NO_PIECE_TYPE] &= ~square_bb(s);
    pos->byType[type_of(pc)] &= ~square_bb(s);
    pos->byColor[color_of(pc)] &= ~square_bb(s);
    pos->pieceCount[pc]--;
    pos->board[s] = NO_PIECE;
    pos->pawnKey ^= pawn_key_term(pc, s);
    pos->nonPawnKey[color_of(pc)] ^= non_pawn_key_term(pc, s);
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
    pos->nonPawnKey[color_of(pc)] ^= non_pawn_key_term(pc, from) ^ non_pawn_key_term(pc, to);
}

/* The ep file is hashed only when a pawn could capture, so identical positions share a key.
 * Pseudo-legal: what matters is that do_move uses the same test. */
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

/* No side to move: correction history indexes that separately. */
Key board_compute_pawn_key(const Position *pos) {
    Key k = 0;

    Bitboard pawns = pos->byType[PAWN];
    while (pawns) {
        const Square s = pop_lsb(&pawns);
        k ^= ZobristPiece[piece_on(pos, s)][s];
    }

    return k;
}

Key board_compute_non_pawn_key(const Position *pos, Color c) {
    Key k = 0;

    Bitboard pieces = pos->byColor[c] & ~pos->byType[PAWN];
    while (pieces) {
        const Square s = pop_lsb(&pieces);
        k ^= ZobristPiece[piece_on(pos, s)][s];
    }

    return k;
}

/* pawn_attacks(BLACK, s) is where a white pawn attacking s stands: the inversion is right. */
Bitboard board_attackers_to(const Position *pos, Square s, Bitboard occupied) {
    return (pawn_attacks(BLACK, s) & pieces_bb(pos, WHITE, PAWN)) |
           (pawn_attacks(WHITE, s) & pieces_bb(pos, BLACK, PAWN)) |
           (knight_attacks(s) & pos->byType[KNIGHT]) | (king_attacks(s) & pos->byType[KING]) |
           (bishop_attacks(s, occupied) & (pos->byType[BISHOP] | pos->byType[QUEEN])) |
           (rook_attacks(s, occupied) & (pos->byType[ROOK] | pos->byType[QUEEN]));
}

/* Cheap leaper lookups before the sliders. */
bool board_square_attacked(const Position *pos, Square s, Color by, Bitboard occupied) {
    const Color them = (Color)(by ^ 1);

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

/* Pieces of `us` that are the only blocker between their king and an enemy slider. */
static Bitboard compute_pinned(const Position *pos, Color us, Square ksq) {
    const Color them   = (Color)(us ^ 1);
    const Bitboard occ = occupied_bb(pos);
    Bitboard pinned    = BB_EMPTY;

    Bitboard snipers = (rook_attacks(ksq, BB_EMPTY) & pieces2_bb(pos, them, ROOK, QUEEN)) |
                       (bishop_attacks(ksq, BB_EMPTY) & pieces2_bb(pos, them, BISHOP, QUEEN));

    while (snipers) {
        const Square sniper  = pop_lsb(&snipers);
        const Bitboard block = SquaresBetween[ksq][sniper] & occ;

        if (block && bb_at_most_one(block))
            pinned |= block & color_bb(pos, us);
    }
    return pinned;
}

/* Must be called after every change to the board. */
static void set_check_info(Position *pos) {
    const Color us   = pos->sideToMove;
    const Square ksq = king_square(pos, us);

    pos->checkers = board_attackers_to(pos, ksq, occupied_bb(pos)) & color_bb(pos, (Color)(us ^ 1));
    pos->pinned   = compute_pinned(pos, us, ksq);
}

/* Grants one right and derives its geometry. `kingPath` includes the king's origin, so
 * castling out of check is rejected for free. `emptyPath` is both lanes minus the king's
 * and rook's own squares, which in Chess960 may overlap the lanes. */
static void grant_castling(Position *p, Color c, Square rookSq) {
    const Square ksq    = king_square(p, c);
    const bool kingside = rookSq > ksq;
    const int idx       = castling_index(c, kingside);

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

/* X-FEN's K/Q: the rook furthest from the king on that side. */
static Square outermost_rook(const Position *p, Color c, Square ksq, bool kingside) {
    const Rank home  = rank_of(ksq);
    const Piece rook = make_piece(c, ROOK);
    const int step   = kingside ? -1 : 1;

    for (int f = kingside ? FILE_H : FILE_A; f != (int)file_of(ksq); f += step)
        if (piece_on(p, make_square((File)f, home)) == rook)
            return make_square((File)f, home);

    return SQ_NONE;
}

/* Accepts X-FEN (KQkq) and Shredder (AHah) rights. A right the diagram cannot back is
 * dropped: movegen trusts the rights, and an unbacked one would conjure a rook. */
static void resolve_castling(Position *p, const char *field) {
    for (const char *c = field; *c; ++c) {
        const Color col  = islower((unsigned char)*c) ? BLACK : WHITE;
        const char u     = (char)toupper((unsigned char)*c);
        const Square ksq = king_square(p, col);

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

/* Rights that standard notation cannot express, which switch on Chess960 notation. */
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

/* movegen trusts the ep square, so a FEN's must have a pawn to capture behind it. */
static bool ep_target_is_real(const Position *p, Square ep) {
    const Color us      = p->sideToMove;
    const int up        = pawn_push(us);
    const Square pawnSq = (Square)(ep - up);
    const Square fromSq = (Square)(ep + up);

    if (relative_rank(us, ep) != RANK_6)
        return false;

    return piece_on(p, pawnSq) == make_piece((Color)(us ^ 1), PAWN) && is_empty(p, ep) &&
           is_empty(p, fromSq);
}

/* Rejections describe the diagram ("nine white pawns"), not the parser. */
static bool fen_reject(const char **why, const char *reason) {
    if (why)
        *why = reason;
    return false;
}

bool board_set_fen_reason(Position *pos, const char *fen, const char **why) {
    /* Parse into scratch, so a malformed FEN from a GUI leaves `pos` intact. */
    Position p;
    memset(&p, 0, sizeof(p));
    for (Square s = SQ_A1; s <= SQ_H8; ++s)
        p.board[s] = NO_PIECE;
    p.epSquare = SQ_NONE;

    const char *c = fen;
    while (*c == ' ')
        ++c;

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

    while (*c == ' ')
        ++c;
    if (*c == 'w')
        p.sideToMove = WHITE;
    else if (*c == 'b')
        p.sideToMove = BLACK;
    else
        return fen_reject(why, "the side to move is neither 'w' nor 'b'");
    ++c;

    while (*c == ' ')
        ++c;
    p.castling = NO_CASTLING;
    for (int i = 0; i < CASTLING_NB; ++i)
        p.castlingRook[i] = SQ_NONE;

    /* Resolved later, once the kings are known to exist. */
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

            if (castlingLen + 1 >= sizeof(castlingField))
                return fen_reject(why, "the castling field is longer than the rights it can spell");
            castlingField[castlingLen++] = *c;
        }
    }

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

    /* Both clocks are optional; EPD lines routinely omit them. */
    p.halfmoveClock  = 0;
    p.fullmoveNumber = 1;
    if (sscanf(c, " %d %d", &p.halfmoveClock, &p.fullmoveNumber) < 2)
        p.fullmoveNumber = 1;
    if (p.halfmoveClock < 0 || p.fullmoveNumber < 1)
        return fen_reject(why, "the halfmove or fullmove clock is negative");

    if (p.pieceCount[W_KING] != 1 || p.pieceCount[B_KING] != 1)
        return fen_reject(why, "the diagram does not have exactly one king a side");

    /* Illegal material (nine pawns) is fine, but more than 32 men is not: the net's int16
     * accumulator is proven not to wrap for at most 32 features. Must match
     * ENGINE_MAX_PIECES in tools/export_net.py. */
    if (popcount(occupied_bb(&p)) > 32)
        return fen_reject(why, "the diagram has more men than chess has");

    /* Unbacked rights and ep targets are dropped, not rejected: stale ones are routine
     * editor output. Before the key is computed. */
    resolve_castling(&p, castlingField);
    if (p.epSquare != SQ_NONE && !ep_target_is_real(&p, p.epSquare))
        p.epSquare = SQ_NONE;

    /* Sticky: a FEN can switch Chess960 notation on but never off. Notation only - the
     * rules come from the geometry either way. */
    p.chess960 = pos->chess960 || shredderSpelling || castling_is_nonstandard(&p);
    p.gamePly  = 0;
    p.key      = board_compute_key(&p);
    set_check_info(&p);

    /* The side not to move may not be in check. */
    if (board_square_attacked(&p, king_square(&p, (Color)(p.sideToMove ^ 1)), p.sideToMove,
                              occupied_bb(&p)))
        return fen_reject(why, "the side that just moved has left its own king in check");

    *pos = p;
    return true;
}

bool board_set_fen(Position *pos, const char *fen) { return board_set_fen_reason(pos, fen, NULL); }

/* K, R, N on the five remaining squares, king between the rooks. Scharnagl's order, which
 * defines the standard 0-959 numbering: do not reorder. */
static const char *const KrnPatterns[10] = {"NNRKR", "NRNKR", "NRKNR", "NRKRN", "RNNKR",
                                            "RNKNR", "RNKRN", "RKNNR", "RKNRN", "RKRNN"};

static void chess960_back_rank(int idx, char back[9]) {
    memset(back, ' ', 8);
    back[8] = '\0';

    /* Scharnagl: light bishop, dark bishop, queen into the n-th free square, then K/R/N. */
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
}

bool board_set_dfrc_start(Position *pos, int whiteIdx, int blackIdx) {
    if (whiteIdx < 0 || whiteIdx >= 960 || blackIdx < 0 || blackIdx >= 960)
        return false;

    char back[9], front[9];
    chess960_back_rank(whiteIdx, back);
    chess960_back_rank(blackIdx, front);
    for (int f = 0; f < 8; ++f)
        front[f] = (char)tolower((unsigned char)front[f]);

    const char wA = (char)('A' + (strchr(back, 'R') - back));
    const char wH = (char)('A' + (strrchr(back, 'R') - back));
    const char bA = (char)('a' + (strchr(front, 'r') - front));
    const char bH = (char)('a' + (strrchr(front, 'r') - front));

    /* Shredder spelling even for SP 518, so the position reads back as Chess960. */
    char fen[FEN_MAX_LEN];
    snprintf(fen, sizeof(fen), "%s/pppppppp/8/8/8/8/PPPPPPPP/%s w %c%c%c%c - 0 1", front, back, wH,
             wA, bH, bA);

    return board_set_fen(pos, fen);
}

bool board_set_chess960_start(Position *pos, int idx) {
    return board_set_dfrc_start(pos, idx, idx);
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
        /* Shredder (rook files) for Chess960: KQkq can only name the outermost rook. */
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
            return false;
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

    return pos->key == board_compute_key(pos) && pos->pawnKey == board_compute_pawn_key(pos) &&
           pos->nonPawnKey[WHITE] == board_compute_non_pawn_key(pos, WHITE) &&
           pos->nonPawnKey[BLACK] == board_compute_non_pawn_key(pos, BLACK);
}

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

    /* The ep right expires; unhash it only if it was hashed. */
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

        /* Both off before either goes down: in Chess960 the squares can overlap. */
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

            /* Double push. The ep square is always recorded, but hashed only if capturable. */
            if ((from ^ to) == 16) {
                pos->epSquare = (Square)((from + to) / 2);

                if (ep_capturable(pos, them, pos->epSquare))
                    k ^= ZobristEnPassant[file_of(pos->epSquare)];
            }
        }
    }

    /* castlingLoss is per position because Chess960 origins vary. A castle always moves the
     * king, whose square carries both its rights, so `from` covers it. */
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

    const Color us    = (Color)(pos->sideToMove ^ 1);
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

/* Material that cannot mate at all. KNN is excluded: mate is possible with cooperation. */
static bool insufficient_material(const Position *pos) {
    if (pos->byType[PAWN] | pos->byType[ROOK] | pos->byType[QUEEN])
        return false;

    const Bitboard minors = pos->byType[KNIGHT] | pos->byType[BISHOP];

    if (bb_at_most_one(minors))
        return true;

    /* One bishop each, on the same colour. */
    if (piece_count(pos, WHITE, BISHOP) == 1 && piece_count(pos, BLACK, BISHOP) == 1 &&
        minors == pos->byType[BISHOP])
        return (minors & BB_LIGHT_SQUARES) == minors || (minors & BB_DARK_SQUARES) == minors;

    return false;
}

/* A repetition inside the search is a draw at once; one reaching back before the root
 * needs the full threefold count. */
static bool is_repetition(const Position *pos, int ply) {
    const int back = pos->halfmoveClock < pos->gamePly ? pos->halfmoveClock : pos->gamePly;
    int seen       = 0;

    for (int i = 4; i <= back; i += 2) {
        if (pos->history[pos->gamePly - i].key != pos->key)
            continue;
        if (++seen + (ply > i ? 1 : 0) >= 2)
            return true;
    }
    return false;
}

/* Every reversible non-pawn move on an empty board, keyed by its Zobrist difference
 * (side to move included): 3668 moves cuckoo-hashed into 8192 slots, so a lookup is at
 * most two probes. Marcel van Kervinck's method, as Stockfish uses it. */
static Key CuckooKey[8192];
static Move CuckooMove[8192];

static inline int cuckoo_h1(Key h) { return (int)(h & 0x1fff); }
static inline int cuckoo_h2(Key h) { return (int)((h >> 16) & 0x1fff); }

void board_cuckoo_init(void) {
    memset(CuckooKey, 0, sizeof(CuckooKey));
    memset(CuckooMove, 0, sizeof(CuckooMove));
    int count = 0;

    for (int c = WHITE; c <= BLACK; ++c)
        for (int pt = KNIGHT; pt <= KING; ++pt) {
            const Piece pc = make_piece((Color)c, (PieceType)pt);
            for (int s1 = 0; s1 < SQUARE_NB; ++s1)
                for (int s2 = s1 + 1; s2 < SQUARE_NB; ++s2) {
                    if (!(attacks_bb((PieceType)pt, (Square)s1, BB_EMPTY) & square_bb((Square)s2)))
                        continue;

                    Move move = make_move((Square)s1, (Square)s2);
                    Key key   = ZobristPiece[pc][s1] ^ ZobristPiece[pc][s2] ^ ZobristSideToMove;
                    int i     = cuckoo_h1(key);

                    for (;;) {
                        const Key tk  = CuckooKey[i];
                        CuckooKey[i]  = key;
                        key           = tk;
                        const Move tm = CuckooMove[i];
                        CuckooMove[i] = move;
                        move          = tm;
                        if (move == MOVE_NONE)
                            break;
                        i = i == cuckoo_h1(key) ? cuckoo_h2(key) : cuckoo_h1(key);
                    }
                    ++count;
                }
        }
    assert(count == 3668);
    (void)count;
}

/* Whether the side to move can repeat a position with one move. `other` accumulates the
 * opponent's changes; when it cancels, the difference to the position `i` plies up is one
 * of our moves, and if that move is reversible with a clear path it repeats. */
bool board_upcoming_repetition(const Position *pos, int ply, int limit) {
    int end = pos->halfmoveClock < pos->gamePly ? pos->halfmoveClock : pos->gamePly;
    if (limit < end)
        end = limit;
    if (end < 3)
        return false;

    const Key original = pos->key;
    Key other          = original ^ pos->history[pos->gamePly - 1].key ^ ZobristSideToMove;

    for (int i = 3; i <= end; i += 2) {
        other ^= pos->history[pos->gamePly - (i - 1)].key ^ pos->history[pos->gamePly - i].key ^
                 ZobristSideToMove;
        if (other != 0)
            continue;

        const Key moveKey = original ^ pos->history[pos->gamePly - i].key;
        int j             = cuckoo_h1(moveKey);
        if (CuckooKey[j] != moveKey) {
            j = cuckoo_h2(moveKey);
            if (CuckooKey[j] != moveKey)
                continue;
        }

        /* Inside the search only; before the root needs the threefold count. */
        const Move move = CuckooMove[j];
        if (!(SquaresBetween[from_sq(move)][to_sq(move)] & occupied_bb(pos)) && ply > i)
            return true;
    }
    return false;
}

bool board_is_draw(const Position *pos, int ply) {
    if (pos->halfmoveClock > 99) {
        /* Checkmate outranks the fifty-move rule. */
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
