/*
 * movegen.c - move generation.
 *
 * Moves are generated PSEUDO-LEGALLY and filtered by movegen_is_legal() only
 * for the moves the caller actually plays. That split is what makes the cached
 * `pinned` and `checkers` sets in Position worth maintaining: legality then
 * costs a bitboard test rather than an attack scan, and moves that get pruned
 * before they are played never pay for it at all.
 *
 * The one exception is en passant, which removes two pawns from one rank in a
 * single move. No pin set can express that, so it gets a full attack test.
 */
#include "movegen.h"

#include <assert.h>

#include "bitboard.h"

/* ------------------------------------------------------------- emission -- */

/* `score` is deliberately left untouched: move ordering owns that field, and
 * generation writing a zero into it would be a wasted store per move. */
static inline ScoredMove *emit(ScoredMove *out, Square from, Bitboard targets) {
    while (targets)
        (out++)->m = make_move(from, pop_lsb(&targets));
    return out;
}

/* --------------------------------------------------------------- pawns --- */

static inline Bitboard shift_up(Bitboard b, Color c) {
    return c == WHITE ? shift_north(b) : shift_south(b);
}

static inline Bitboard shift_up_east(Bitboard b, Color c) {
    return c == WHITE ? shift_north_east(b) : shift_south_east(b);
}

static inline Bitboard shift_up_west(Bitboard b, Color c) {
    return c == WHITE ? shift_north_west(b) : shift_south_west(b);
}

/*
 * Which promotion pieces to emit, split so that GEN_CAPTURES and GEN_QUIETS
 * partition the promotions exactly once between them:
 *
 *   - a queen promotion is a "capture" for search purposes whether or not it
 *     takes anything, because it swings material by nine pawns;
 *   - an underpromotion is only worth searching in quiescence when it also
 *     captures, so a quiet underpromotion belongs to GEN_QUIETS.
 *
 * Underpromotions matter: a knight promotion with check is the only way out of
 * some positions, and an engine that skips them plays them as blunders.
 */
static ScoredMove *make_promotions(ScoredMove *out, Square from, Square to, GenType type,
                                   bool capture) {
    if (type != GEN_QUIETS)
        (out++)->m = make_promotion(from, to, QUEEN);

    if (type == GEN_EVASIONS || type == GEN_ALL || (type == GEN_CAPTURES) == capture) {
        (out++)->m = make_promotion(from, to, ROOK);
        (out++)->m = make_promotion(from, to, BISHOP);
        (out++)->m = make_promotion(from, to, KNIGHT);
    }
    return out;
}

/*
 * `target` restricts destinations and is only consulted for GEN_EVASIONS -
 * every other generation type expresses its filter through `enemies` and the
 * empty-square set instead.
 */
static ScoredMove *gen_pawn_moves(const Position *pos, Color us, GenType type, Bitboard target,
                                  ScoredMove *out) {
    const Color them       = (Color)(us ^ 1);
    const Bitboard rank7   = us == WHITE ? BB_RANK_7 : BB_RANK_2;
    const Bitboard rank3   = us == WHITE ? BB_RANK_3 : BB_RANK_6;
    const int up           = pawn_push(us);
    const int upEast       = us == WHITE ? 9 : -7;
    const int upWest       = us == WHITE ? 7 : -9;
    const Bitboard pawns   = pieces_bb(pos, us, PAWN);
    const Bitboard onRank7 = pawns & rank7;
    const Bitboard rest    = pawns & ~rank7;
    const Bitboard empty   = ~occupied_bb(pos);

    /* When answering a check, the only capturable piece is the checker itself. */
    const Bitboard enemies = type == GEN_EVASIONS ? pos->checkers : color_bb(pos, them);

    /* --- pushes (never promotions; those are handled below) --- */
    if (type != GEN_CAPTURES) {
        Bitboard single = shift_up(rest, us) & empty;
        Bitboard dbl    = shift_up(single & rank3, us) & empty;

        if (type == GEN_EVASIONS) { /* a push only helps if it blocks the check */
            single &= target;
            dbl &= target;
        }

        while (single) {
            const Square to = pop_lsb(&single);
            (out++)->m      = make_move((Square)(to - up), to);
        }
        while (dbl) {
            const Square to = pop_lsb(&dbl);
            (out++)->m      = make_move((Square)(to - 2 * up), to);
        }
    }

    /* --- promotions, by push and by capture --- */
    if (onRank7) {
        Bitboard east = shift_up_east(onRank7, us) & enemies;
        Bitboard west = shift_up_west(onRank7, us) & enemies;
        Bitboard push = shift_up(onRank7, us) & empty;

        if (type == GEN_EVASIONS)
            push &= target;

        while (east) {
            const Square to = pop_lsb(&east);
            out             = make_promotions(out, (Square)(to - upEast), to, type, true);
        }
        while (west) {
            const Square to = pop_lsb(&west);
            out             = make_promotions(out, (Square)(to - upWest), to, type, true);
        }
        while (push) {
            const Square to = pop_lsb(&push);
            out             = make_promotions(out, (Square)(to - up), to, type, false);
        }
    }

    /* --- ordinary captures --- */
    if (type != GEN_QUIETS) {
        Bitboard east = shift_up_east(rest, us) & enemies;
        Bitboard west = shift_up_west(rest, us) & enemies;

        while (east) {
            const Square to = pop_lsb(&east);
            (out++)->m      = make_move((Square)(to - upEast), to);
        }
        while (west) {
            const Square to = pop_lsb(&west);
            (out++)->m      = make_move((Square)(to - upWest), to);
        }

        if (pos->epSquare != SQ_NONE) {
            const Square capsq = (Square)(pos->epSquare - up);

            /* In check, an en passant capture is only worth generating when it
             * takes the checking pawn or blocks the checking line. Legality
             * (including the discovered-check case) is settled in
             * movegen_is_legal either way. */
            if (type != GEN_EVASIONS || bb_test(pos->checkers, capsq) ||
                bb_test(target, pos->epSquare)) {
                Bitboard from = pawn_attacks(them, pos->epSquare) & rest;
                while (from)
                    (out++)->m = make_move_typed(pop_lsb(&from), pos->epSquare, MT_EN_PASSANT);
            }
        }
    }

    return out;
}

/* ------------------------------------------------------------- castling -- */

/*
 * Standard-chess castling geometry. board_set_fen rejects Shredder-FEN, so the
 * king and rook home squares are fixed and this can be a constant table.
 *
 * `kingPath` includes the king's ORIGIN square, which is what makes
 * movegen_is_legal reject castling out of check without a separate test.
 */
typedef struct {
    CastlingRights right;
    Square kingFrom;
    Square rookFrom;
    Bitboard emptyPath; /* every square that must be unoccupied */
    Bitboard kingPath;  /* every square the king occupies or crosses */
} CastlingSpec;

#define BB_OF(s) (1ULL << (s))

static const CastlingSpec Castlings[COLOR_NB][2] = {
    {{WHITE_OO, SQ_E1, SQ_H1, BB_OF(SQ_F1) | BB_OF(SQ_G1),
      BB_OF(SQ_E1) | BB_OF(SQ_F1) | BB_OF(SQ_G1)},
     {WHITE_OOO, SQ_E1, SQ_A1, BB_OF(SQ_B1) | BB_OF(SQ_C1) | BB_OF(SQ_D1),
      BB_OF(SQ_E1) | BB_OF(SQ_D1) | BB_OF(SQ_C1)}},
    {{BLACK_OO, SQ_E8, SQ_H8, BB_OF(SQ_F8) | BB_OF(SQ_G8),
      BB_OF(SQ_E8) | BB_OF(SQ_F8) | BB_OF(SQ_G8)},
     {BLACK_OOO, SQ_E8, SQ_A8, BB_OF(SQ_B8) | BB_OF(SQ_C8) | BB_OF(SQ_D8),
      BB_OF(SQ_E8) | BB_OF(SQ_D8) | BB_OF(SQ_C8)}}};

#undef BB_OF

/* Kingside is index 0, queenside index 1 - recoverable from a castling move
 * because it is encoded king-captures-own-rook. */
static inline const CastlingSpec *castling_spec(Color us, Square kingFrom, Square rookFrom) {
    return &Castlings[us][rookFrom > kingFrom ? 0 : 1];
}

static ScoredMove *gen_castling(const Position *pos, Color us, ScoredMove *out) {
    for (int i = 0; i < 2; ++i) {
        const CastlingSpec *const cs = &Castlings[us][i];

        if (!(pos->castling & cs->right) || (occupied_bb(pos) & cs->emptyPath))
            continue;

        assert(piece_on(pos, cs->kingFrom) == make_piece(us, KING));
        assert(piece_on(pos, cs->rookFrom) == make_piece(us, ROOK));

        /* Whether the king's path is attacked is settled by movegen_is_legal,
         * so generation stays free of attack scans. */
        (out++)->m = make_move_typed(cs->kingFrom, cs->rookFrom, MT_CASTLING);
    }
    return out;
}

/* ------------------------------------------------------ generation core -- */

/* Knights, bishops, rooks and queens. Queens are generated by the bishop and
 * rook loops in turn: the two attack sets are disjoint, so no move is emitted
 * twice and the switch on piece type disappears. */
static ScoredMove *gen_piece_moves(const Position *pos, Color us, Bitboard target,
                                   ScoredMove *out) {
    const Bitboard occ = occupied_bb(pos);

    Bitboard b = pieces_bb(pos, us, KNIGHT);
    while (b) {
        const Square from = pop_lsb(&b);
        out               = emit(out, from, knight_attacks(from) & target);
    }

    b = pieces2_bb(pos, us, BISHOP, QUEEN);
    while (b) {
        const Square from = pop_lsb(&b);
        out               = emit(out, from, bishop_attacks(from, occ) & target);
    }

    b = pieces2_bb(pos, us, ROOK, QUEEN);
    while (b) {
        const Square from = pop_lsb(&b);
        out               = emit(out, from, rook_attacks(from, occ) & target);
    }

    return out;
}

/*
 * Only moves that answer an existing check.
 *
 * King steps are computed against an occupancy with the king removed, because
 * a king running straight down a checking slider's line is still in check on
 * every square of it - the classic "king walks backwards along the rook file"
 * bug.
 */
static ScoredMove *gen_evasions(const Position *pos, ScoredMove *out) {
    const Color us   = pos->sideToMove;
    const Square ksq = king_square(pos, us);

    /* Squares still covered by a checking slider once the king steps aside. */
    Bitboard sliderRays = BB_EMPTY;
    Bitboard sliders    = pos->checkers & ~(pos->byType[PAWN] | pos->byType[KNIGHT]);
    while (sliders) {
        const Square checksq = pop_lsb(&sliders);
        sliderRays |= LineThrough[checksq][ksq] & ~square_bb(checksq);
    }

    out = emit(out, ksq, king_attacks(ksq) & ~color_bb(pos, us) & ~sliderRays);

    /* Against two checkers no interposition or capture can help both, so the
     * king moves above are the complete answer. */
    if (bb_more_than_one(pos->checkers))
        return out;

    const Square checksq  = lsb(pos->checkers);
    const Bitboard target = SquaresBetween[checksq][ksq] | square_bb(checksq);

    out = gen_pawn_moves(pos, us, GEN_EVASIONS, target, out);
    out = gen_piece_moves(pos, us, target, out);

    return out;
}

int movegen_generate(const Position *pos, GenType type, ScoredMove *list) {
    if (type == GEN_EVASIONS) {
        assert(pos->checkers != BB_EMPTY);
        return (int)(gen_evasions(pos, list) - list);
    }

    const Color us        = pos->sideToMove;
    const Square ksq      = king_square(pos, us);
    const Bitboard target = type == GEN_CAPTURES ? color_bb(pos, (Color)(us ^ 1))
                            : type == GEN_QUIETS ? ~occupied_bb(pos)
                                                 : ~color_bb(pos, us);

    ScoredMove *out = list;
    out             = gen_pawn_moves(pos, us, type, target, out);
    out             = gen_piece_moves(pos, us, target, out);
    out             = emit(out, ksq, king_attacks(ksq) & target);

    /* Castling is quiet, and is never a legal answer to a check. */
    if (type != GEN_CAPTURES && pos->checkers == BB_EMPTY)
        out = gen_castling(pos, us, out);

    assert(out - list <= MAX_MOVES);
    return (int)(out - list);
}

/* ------------------------------------------------------------- legality -- */

/*
 * The en passant special case.
 *
 * The capture vacates the capturing pawn's square AND the captured pawn's
 * square, which are on the same rank. Two pieces leaving one rank can uncover
 * a rook or queen that neither a pin test nor a check test saw, so the only
 * reliable answer is to build the resulting occupancy and ask directly.
 */
static bool en_passant_is_legal(const Position *pos, Square from, Square to) {
    const Color us     = pos->sideToMove;
    const Color them   = (Color)(us ^ 1);
    const Square ksq   = king_square(pos, us);
    const Square capsq = (Square)(to - pawn_push(us));
    const Bitboard occ = (occupied_bb(pos) ^ square_bb(from) ^ square_bb(capsq)) | square_bb(to);

    /* The captured pawn is gone, so it must not count as an attacker - it may
     * well have been the piece giving check. */
    const Bitboard enemy = color_bb(pos, them) & ~square_bb(capsq);

    return !(pawn_attacks(us, ksq) & enemy & pos->byType[PAWN]) &&
           !(knight_attacks(ksq) & enemy & pos->byType[KNIGHT]) &&
           !(king_attacks(ksq) & enemy & pos->byType[KING]) &&
           !(bishop_attacks(ksq, occ) & enemy & (pos->byType[BISHOP] | pos->byType[QUEEN])) &&
           !(rook_attacks(ksq, occ) & enemy & (pos->byType[ROOK] | pos->byType[QUEEN]));
}

bool movegen_is_legal(const Position *pos, Move m) {
    assert(is_ok_move(m));

    const Color us    = pos->sideToMove;
    const Color them  = (Color)(us ^ 1);
    const Square from = from_sq(m);
    const Square to   = to_sq(m);
    const MoveType mt = type_of_move(m);
    const Square ksq  = king_square(pos, us);

    if (mt == MT_EN_PASSANT)
        return en_passant_is_legal(pos, from, to);

    if (mt == MT_CASTLING) {
        /* kingPath includes the origin square, so this also rejects castling
         * out of check. The king cannot screen its own path in standard chess:
         * any attacker aligned through it would already be giving check. */
        Bitboard path = castling_spec(us, from, to)->kingPath;
        while (path)
            if (board_square_attacked(pos, pop_lsb(&path), them, occupied_bb(pos)))
                return false;
        return true;
    }

    if (from == ksq) {
        /* Remove the king before testing: otherwise it blocks the very slider
         * it is trying to escape and every square on that line looks safe. */
        return !board_square_attacked(pos, to, them, occupied_bb(pos) ^ square_bb(from));
    }

    /* A pinned piece may only travel along the line joining it to its king. */
    if ((pos->pinned & square_bb(from)) && !aligned(from, to, ksq))
        return false;

    if (pos->checkers) {
        if (bb_more_than_one(pos->checkers))
            return false; /* only the king can answer a double check */

        const Square checksq = lsb(pos->checkers);
        if (!bb_test(SquaresBetween[checksq][ksq] | square_bb(checksq), to))
            return false;
    }

    return true;
}

/* --------------------------------------------------------- validation --- */

bool movegen_is_pseudo_legal(const Position *pos, Move m) {
    if (!is_ok_move(m))
        return false;

    const Color us    = pos->sideToMove;
    const Color them  = (Color)(us ^ 1);
    const Square from = from_sq(m);
    const Square to   = to_sq(m);
    const MoveType mt = type_of_move(m);
    const Piece pc    = piece_on(pos, from);

    if (pc == NO_PIECE || color_of(pc) != us)
        return false;

    if (mt == MT_CASTLING) {
        if (pos->checkers || from != king_square(pos, us))
            return false;

        const CastlingSpec *const cs = castling_spec(us, from, to);
        return cs->kingFrom == from && cs->rookFrom == to && (pos->castling & cs->right) &&
               !(occupied_bb(pos) & cs->emptyPath);
    }

    if (mt == MT_EN_PASSANT) {
        return type_of(pc) == PAWN && pos->epSquare != SQ_NONE && to == pos->epSquare &&
               is_empty(pos, to) &&
               piece_on(pos, (Square)(to - pawn_push(us))) == make_piece(them, PAWN) &&
               bb_test(pawn_attacks(us, from), to);
    }

    /* Nothing may capture its own side, and only a pawn may promote. */
    if (bb_test(color_bb(pos, us), to))
        return false;

    if (type_of(pc) != PAWN) {
        return mt == MT_NORMAL && bb_test(attacks_bb(type_of(pc), from, occupied_bb(pos)), to);
    }

    if ((mt == MT_PROMOTION) != (relative_rank(us, to) == RANK_8))
        return false;
    if (mt != MT_NORMAL && mt != MT_PROMOTION)
        return false;

    const int up = pawn_push(us);

    if (bb_test(pawn_attacks(us, from), to))
        return bb_test(color_bb(pos, them), to); /* a capture needs something to take */
    if (to == from + up)
        return is_empty(pos, to);
    if (to == from + 2 * up)
        return relative_rank(us, from) == RANK_2 && is_empty(pos, to) &&
               is_empty(pos, (Square)(from + up));

    return false;
}
