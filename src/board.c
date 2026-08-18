/*
 * board.c - position setup, FEN I/O and hashing.
 *
 * The move-making half of this module is deliberately left as TODO stubs; see
 * the contract documented in board.h.
 */
#include "board.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "bitboard.h"
#include "zobrist.h"

static const char PieceChars[PIECE_NB] = {[W_PAWN] = 'P', [W_KNIGHT] = 'N', [W_BISHOP] = 'B',
                                          [W_ROOK] = 'R', [W_QUEEN] = 'Q',  [W_KING] = 'K',
                                          [B_PAWN] = 'p', [B_KNIGHT] = 'n', [B_BISHOP] = 'b',
                                          [B_ROOK] = 'r', [B_QUEEN] = 'q',  [B_KING] = 'k'};

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

void board_put_piece(Position *pos, Piece pc, Square s) {
    pos->board[s] = pc;
    pos->byType[NO_PIECE_TYPE] |= square_bb(s);
    pos->byType[type_of(pc)] |= square_bb(s);
    pos->byColor[color_of(pc)] |= square_bb(s);
    pos->pieceCount[pc]++;
}

void board_remove_piece(Position *pos, Square s) {
    const Piece pc = pos->board[s];
    pos->byType[NO_PIECE_TYPE] &= ~square_bb(s);
    pos->byType[type_of(pc)] &= ~square_bb(s);
    pos->byColor[color_of(pc)] &= ~square_bb(s);
    pos->pieceCount[pc]--;
    pos->board[s] = NO_PIECE;
}

void board_move_piece(Position *pos, Square from, Square to) {
    const Piece pc      = pos->board[from];
    const Bitboard mask = square_bb(from) | square_bb(to);
    pos->byType[NO_PIECE_TYPE] ^= mask;
    pos->byType[type_of(pc)] ^= mask;
    pos->byColor[color_of(pc)] ^= mask;
    pos->board[from] = NO_PIECE;
    pos->board[to]   = pc;
}

Key board_compute_key(const Position *pos) {
    Key k = 0;

    Bitboard occ = occupied_bb(pos);
    while (occ) {
        const Square s = pop_lsb(&occ);
        k ^= ZobristPiece[piece_on(pos, s)][s];
    }

    k ^= ZobristCastling[pos->castling];

    /* TODO(perf): only fold in the en passant file when an enemy pawn can
     * actually capture. Hashing an irrelevant ep square splits transposition
     * table entries that ought to be shared. */
    if (pos->epSquare != SQ_NONE)
        k ^= ZobristEnPassant[file_of(pos->epSquare)];

    if (pos->sideToMove == BLACK)
        k ^= ZobristSideToMove;

    return k;
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
                /* TODO(chess960): Shredder-FEN spells rights as the rook's
                 * file (A-H/a-h). Accept those and record the rook files once
                 * castling move generation understands arbitrary rook starts. */
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

    p.chess960 = pos->chess960; /* preserve the UCI option across position changes */
    p.gamePly  = 0;
    p.key      = board_compute_key(&p);

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

    return pos->key == board_compute_key(pos);
}

/* ========================================================================== *
 *  TODO(engine): move making. See the contract in board.h.
 *  Everything below is a placeholder so the module links and the UCI layer,
 *  build system and CI can be exercised today.
 * ========================================================================== */

void board_do_move(Position *pos, Move m) {
    (void)pos;
    (void)m;
    /* TODO(engine): implement make-move. Start here, then `make perft`. */
}

void board_undo_move(Position *pos, Move m) {
    (void)pos;
    (void)m;
    /* TODO(engine): implement unmake-move. */
}

void board_do_null_move(Position *pos) {
    (void)pos;
    /* TODO(engine): needed by null-move pruning. */
}

void board_undo_null_move(Position *pos) {
    (void)pos;
    /* TODO(engine): needed by null-move pruning. */
}

Bitboard board_checkers(const Position *pos) {
    (void)pos;
    /* TODO(engine): pieces of !sideToMove attacking king_square(pos, sideToMove). */
    return BB_EMPTY;
}

bool board_square_attacked(const Position *pos, Square s, Color by, Bitboard occupied) {
    (void)pos;
    (void)s;
    (void)by;
    (void)occupied;
    /* TODO(engine): pawn/knight/king lookups, then bishop_attacks/rook_attacks. */
    return false;
}

bool board_is_draw(const Position *pos) {
    (void)pos;
    /* TODO(engine): fifty-move rule, threefold repetition, insufficient material. */
    return false;
}
