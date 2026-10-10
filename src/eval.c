/*
 * eval.c - the classical evaluation: a linear model, tapered between midgame and endgame.
 * Every weight lives in evalparams.c and is applied through TERM() (invariants 9, 10).
 */
#include "eval.h"

#include <stdio.h>
#include <string.h>

#include "bitboard.h"
#include "evalparams.h"

/* Fixed, not the tuned Material[]: tuning must never change move ordering or SEE. */
const Value PieceValues[PIECE_TYPE_NB] = {
    [NO_PIECE_TYPE] = 0, [PAWN] = 100,  [KNIGHT] = 320, [BISHOP] = 330,
    [ROOK] = 500,        [QUEEN] = 900, [KING] = 0,
};

/* A running white-relative score, midgame and endgame accumulated together. */
typedef struct {
    int mg, eg;
} Score;

/* The only way to add to a score: the tuner's trace comes from the same index and
 * coefficient. `table` must be a bare table name; `coeff` is positive for white. */
#define TERM(sc, table, index, coeff)          \
    do {                                       \
        const int i_ = (index);                \
        const int c_ = (coeff);                \
        (sc)->mg += c_ * (table)[i_].mg;       \
        (sc)->eg += c_ * (table)[i_].eg;       \
        TRACE_ADD(PARAM_OFF_##table + i_, c_); \
    } while (0)

#define SIGN(c) ((c) == WHITE ? 1 : -1)

static inline int clamp_int(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static inline int abs_int(int v) { return v < 0 ? -v : v; }

/* Chebyshev distance: king moves between two squares. */
static inline int square_distance(Square a, Square b) {
    const int df = abs_int((int)file_of(a) - (int)file_of(b));
    const int dr = abs_int((int)rank_of(a) - (int)rank_of(b));
    return df > dr ? df : dr;
}

static inline Square relative_square(Color c, Square s) { return c == WHITE ? s : flip_rank(s); }

/* The piece of `b` nearest to `c`'s own back rank. Undefined for empty `b`. */
static inline Square frontmost(Color c, Bitboard b) { return c == WHITE ? lsb(b) : msb(b); }

/* Filled by eval_init(). */
static Bitboard AdjacentFiles[8];
static Bitboard ForwardRanks[COLOR_NB][8];
static Bitboard ForwardFile[COLOR_NB][SQUARE_NB];
static Bitboard PassedPawnSpan[COLOR_NB][SQUARE_NB];
static Bitboard PawnAttackSpan[COLOR_NB][SQUARE_NB];

#define PHASE_MAX 24

/* 24 with all pieces on, 0 with only kings and pawns. */
static const int PhaseWeight[PIECE_TYPE_NB] = {
    [KNIGHT] = 1,
    [BISHOP] = 1,
    [ROOK]   = 2,
    [QUEEN]  = 4,
};

static int game_phase(const Position *pos) {
    int phase = 0;

    for (PieceType pt = KNIGHT; pt <= QUEEN; ++pt)
        phase += PhaseWeight[pt] * (piece_count(pos, WHITE, pt) + piece_count(pos, BLACK, pt));

    /* Promotions can exceed the starting material. */
    return phase > PHASE_MAX ? PHASE_MAX : phase;
}

/* Attack maps and king data, computed once and shared by the terms. */
typedef struct {
    Square ksq[COLOR_NB];
    Bitboard kingRing[COLOR_NB];
    Bitboard mobilityArea[COLOR_NB];

    Bitboard attackedBy[COLOR_NB][PIECE_TYPE_NB];
    Bitboard attackedAll[COLOR_NB];
    Bitboard attackedBy2[COLOR_NB];

    /* Filled while scoring pieces, consumed by the king safety terms. */
    int kingAttackerCount[COLOR_NB];
    int kingRingAttacks[COLOR_NB];
} EvalInfo;

/* 32 normalised king squares onto 8 buckets: more would be too sparse to fit. */
static inline int king_bucket(Square normalisedKing) {
    return (int)(rank_of(normalisedKing) >> 1) * 2 + (int)(file_of(normalisedKing) >> 1);
}

static inline Square mirror_file(Square s) { return (Square)(s ^ 7); }

/* A piece's placement relative to a king, both squares rank-normalised for the owner. The
 * king decides the mirror, and both squares are folded with it. */
#define TERM_PSQK(sc, table, kingSq, pieceSq, pt, coeff)             \
    do {                                                             \
        Square k_ = (kingSq), p_ = (pieceSq);                        \
        if (file_of(k_) >= FILE_E) {                                 \
            k_ = mirror_file(k_);                                    \
            p_ = mirror_file(p_);                                    \
        }                                                            \
        TERM(sc, table, PSQK_IDX(king_bucket(k_), (pt), p_), coeff); \
    } while (0)

void eval_init(void) {
    for (File f = FILE_A; f <= FILE_H; ++f)
        AdjacentFiles[f] = (f > FILE_A ? file_bb((File)(f - 1)) : BB_EMPTY) |
                           (f < FILE_H ? file_bb((File)(f + 1)) : BB_EMPTY);

    for (Rank r = RANK_1; r <= RANK_8; ++r) {
        Bitboard above = BB_EMPTY;
        for (Rank q = (Rank)(r + 1); q <= RANK_8; ++q)
            above |= rank_bb(q);

        ForwardRanks[WHITE][r] = above;
        ForwardRanks[BLACK][r] = ~above & ~rank_bb(r);
    }

    for (Color c = WHITE; c <= BLACK; ++c) {
        for (Square s = SQ_A1; s <= SQ_H8; ++s) {
            const Bitboard ahead = ForwardRanks[c][rank_of(s)];

            ForwardFile[c][s]    = ahead & file_bb(file_of(s));
            PawnAttackSpan[c][s] = ahead & AdjacentFiles[file_of(s)];
            PassedPawnSpan[c][s] = ForwardFile[c][s] | PawnAttackSpan[c][s];
        }
    }
}

static void eval_init_info(const Position *pos, EvalInfo *ei) {
    memset(ei, 0, sizeof(*ei));

    for (Color c = WHITE; c <= BLACK; ++c) {
        const Bitboard pawns = pieces_bb(pos, c, PAWN);

        ei->ksq[c] = king_square(pos, c);

        ei->attackedBy[c][PAWN] = c == WHITE ? shift_north_east(pawns) | shift_north_west(pawns)
                                             : shift_south_east(pawns) | shift_south_west(pawns);
        ei->attackedBy[c][KING] = king_attacks(ei->ksq[c]);

        ei->attackedBy2[c] = ei->attackedBy[c][PAWN] & ei->attackedBy[c][KING];
        ei->attackedAll[c] = ei->attackedBy[c][PAWN] | ei->attackedBy[c][KING];
    }

    for (Color c = WHITE; c <= BLACK; ++c) {
        const Color them = (Color)(c ^ 1);

        /* Widened inward on an edge, or a cornered king would look safer than it is. */
        Bitboard ring = king_attacks(ei->ksq[c]);
        if (file_of(ei->ksq[c]) == FILE_A)
            ring |= shift_east(ring);
        else if (file_of(ei->ksq[c]) == FILE_H)
            ring |= shift_west(ring);
        if (rank_of(ei->ksq[c]) == RANK_1)
            ring |= shift_north(ring);
        else if (rank_of(ei->ksq[c]) == RANK_8)
            ring |= shift_south(ring);
        ei->kingRing[c] = ring | square_bb(ei->ksq[c]);

        /* Not our own men, nor squares enemy pawns cover. */
        ei->mobilityArea[c] = ~(color_bb(pos, c) | ei->attackedBy[them][PAWN]);
    }
}

/* Material and the three placement tables, in one pass. */
static void eval_placement(const Position *pos, const EvalInfo *ei, Score *sc) {
    Bitboard occupied = occupied_bb(pos);

    while (occupied) {
        const Square s     = pop_lsb(&occupied);
        const Piece pc     = piece_on(pos, s);
        const Color c      = color_of(pc);
        const PieceType pt = type_of(pc);
        const int sign     = SIGN(c);

        const Square rs = relative_square(c, s);
        const Square ok = relative_square(c, ei->ksq[c]);
        const Square ek = relative_square(c, ei->ksq[c ^ 1]);

        TERM(sc, Material, pt, sign);

        switch (pt) {
        case PAWN: TERM(sc, PsqPawn, rs, sign); break;
        case KNIGHT: TERM(sc, PsqKnight, rs, sign); break;
        case BISHOP: TERM(sc, PsqBishop, rs, sign); break;
        case ROOK: TERM(sc, PsqRook, rs, sign); break;
        case QUEEN: TERM(sc, PsqQueen, rs, sign); break;
        default: TERM(sc, PsqKing, rs, sign); break;
        }

        TERM_PSQK(sc, PsqOwnKing, ok, rs, pt, sign);
        TERM_PSQK(sc, PsqEnemyKing, ek, rs, pt, sign);
    }
}

static void eval_pawns(const Position *pos, const EvalInfo *ei, Score *sc) {
    for (Color c = WHITE; c <= BLACK; ++c) {
        const Color them      = (Color)(c ^ 1);
        const int sign        = SIGN(c);
        const Bitboard ours   = pieces_bb(pos, c, PAWN);
        const Bitboard theirs = pieces_bb(pos, them, PAWN);
        const int push        = pawn_push(c);

        Bitboard b = ours;
        while (b) {
            const Square s = pop_lsb(&b);
            const File f   = file_of(s);
            const int r    = (int)relative_rank(c, s);

            const Bitboard neighbours = ours & AdjacentFiles[f];
            const Bitboard phalanx    = neighbours & rank_bb(rank_of(s));
            const Bitboard support    = ours & pawn_attacks(them, s);
            const Bitboard stoppers   = theirs & PassedPawnSpan[c][s];
            const Bitboard opposed    = theirs & ForwardFile[c][s];
            const Bitboard lever      = theirs & pawn_attacks(c, s);

            const Square stop        = (Square)(s + push);
            const Bitboard leverPush = theirs & pawn_attacks(c, stop);
            const bool doubled       = (ours & square_bb((Square)(s - push))) != 0;

            if (!neighbours)
                TERM(sc, PawnIsolated, (int)f, sign);
            if (doubled)
                TERM(sc, PawnDoubled, (int)f, sign);

            /* Backward: no neighbour can support its advance, and the stop square is
             * covered by an enemy pawn. */
            if (!(neighbours & ~ForwardRanks[c][rank_of(s)]) && leverPush)
                TERM(sc, PawnBackward, (int)f, sign);

            if (support)
                TERM(sc, PawnConnected, r, sign);
            if (phalanx)
                TERM(sc, PawnPhalanx, r, sign);

            if (!stoppers) {
                TERM(sc, PawnPassed, r, sign);

                if (!is_empty(pos, stop))
                    TERM(sc, PawnPassedBlocked, r, sign);
                if (support)
                    TERM(sc, PawnPassedDefended, r, sign);

                /* King distances to the stop square; meaningful in the endgame. */
                TERM(sc, PawnPassedOwnKing, clamp_int(square_distance(ei->ksq[c], stop), 0, 7),
                     sign);
                TERM(sc, PawnPassedEnemyKing, clamp_int(square_distance(ei->ksq[them], stop), 0, 7),
                     sign);
            } else if (!opposed && popcount(support) >= popcount(lever) &&
                       popcount(phalanx) >= popcount(leverPush)) {
                /* Candidate: unopposed and winning every pawn exchange ahead. */
                TERM(sc, PawnCandidate, r, sign);
            }
        }
    }
}

/* Mobility, outposts, rook files and bishops; also fills the attack maps and king-ring
 * counts that eval_king() and eval_threats() read. */
static void eval_pieces(const Position *pos, EvalInfo *ei, Score *sc) {
    const Bitboard occ = occupied_bb(pos);

    for (Color c = WHITE; c <= BLACK; ++c) {
        const Color them      = (Color)(c ^ 1);
        const int sign        = SIGN(c);
        const Bitboard ours   = pieces_bb(pos, c, PAWN);
        const Bitboard theirs = pieces_bb(pos, them, PAWN);

        for (PieceType pt = KNIGHT; pt <= QUEEN; ++pt) {
            Bitboard b = pieces_bb(pos, c, pt);

            while (b) {
                const Square s     = pop_lsb(&b);
                const Bitboard atk = attacks_bb(pt, s, occ);
                const int mobility = popcount(atk & ei->mobilityArea[c]);

                ei->attackedBy2[c] |= ei->attackedAll[c] & atk;
                ei->attackedBy[c][pt] |= atk;
                ei->attackedAll[c] |= atk;

                switch (pt) {
                case KNIGHT: TERM(sc, MobilityKnight, clamp_int(mobility, 0, 8), sign); break;
                case BISHOP: TERM(sc, MobilityBishop, clamp_int(mobility, 0, 13), sign); break;
                case ROOK: TERM(sc, MobilityRook, clamp_int(mobility, 0, 14), sign); break;
                default: TERM(sc, MobilityQueen, clamp_int(mobility, 0, 27), sign); break;
                }

                /* Scored against the king's owner, hence SIGN(them). */
                if (atk & ei->kingRing[them]) {
                    ei->kingAttackerCount[them]++;
                    ei->kingRingAttacks[them] += popcount(atk & ei->kingRing[them]);

                    TERM(sc, KingAttackWeight, pt, SIGN(them));
                }

                if (pt == KNIGHT || pt == BISHOP) {
                    /* Outpost: ranks 4-6, never attackable by an enemy pawn. */
                    const int rr = (int)relative_rank(c, s);
                    if (rr >= RANK_4 && rr <= RANK_6 && !(PawnAttackSpan[c][s] & theirs)) {
                        const int supported = (ours & pawn_attacks(them, s)) != 0;
                        if (pt == KNIGHT)
                            TERM(sc, KnightOutpost, supported, sign);
                        else
                            TERM(sc, BishopOutpost, supported, sign);
                    }

                    if (pt == BISHOP) {
                        const Bitboard sameColour =
                            (square_bb(s) & BB_LIGHT_SQUARES) ? BB_LIGHT_SQUARES : BB_DARK_SQUARES;
                        TERM(sc, BishopBadPawns, clamp_int(popcount(ours & sameColour), 0, 8),
                             sign);
                    }
                } else if (pt == ROOK) {
                    const Bitboard fileMask = file_bb(file_of(s));
                    if (!(fileMask & ours))
                        TERM(sc, RookOpenFile, (fileMask & theirs) ? 0 : 1, sign);
                    if (relative_rank(c, s) == RANK_7)
                        TERM(sc, RookOnSeventh, 0, sign);
                }
            }
        }

        if (piece_count(pos, c, BISHOP) >= 2)
            TERM(sc, BishopPair, 0, sign);
    }
}

static void eval_king(const Position *pos, const EvalInfo *ei, Score *sc) {
    const Bitboard occ = occupied_bb(pos);

    for (Color c = WHITE; c <= BLACK; ++c) {
        const Color them      = (Color)(c ^ 1);
        const int sign        = SIGN(c);
        const Square ksq      = ei->ksq[c];
        const Bitboard ours   = pieces_bb(pos, c, PAWN);
        const Bitboard theirs = pieces_bb(pos, them, PAWN);

        const Bitboard inFront = ForwardRanks[c][rank_of(ksq)] | rank_bb(rank_of(ksq));

        /* Shelter and storm over three files, the window clamped onto the board. */
        const File kf = (File)clamp_int((int)file_of(ksq), FILE_B, FILE_G);
        for (File f = (File)(kf - 1); f <= (File)(kf + 1); ++f) {
            const int edge      = f < FILE_E ? (int)f : 7 - (int)f;
            const Bitboard mine = ours & file_bb(f) & inFront;
            const Bitboard his  = theirs & file_bb(f) & inFront;

            TERM(sc, KingShelter, edge * 8 + (mine ? (int)relative_rank(c, frontmost(c, mine)) : 0),
                 sign);
            TERM(sc, KingStorm, edge * 8 + (his ? (int)relative_rank(c, frontmost(c, his)) : 0),
                 sign);
        }

        {
            const Bitboard kingFile = file_bb(file_of(ksq));
            TERM(sc, KingOnOpenFile, (kingFile & ours) ? 0 : ((kingFile & theirs) ? 1 : 2), sign);
        }

        TERM(sc, KingAttackers, clamp_int(ei->kingAttackerCount[c], 0, 7), sign);
        TERM(sc, KingRingAttacks, clamp_int(ei->kingRingAttacks[c], 0, 15), sign);

        /* Safe checks: from squares we do not cover. */
        const Bitboard safe = ~color_bb(pos, them) & ~ei->attackedAll[c];

        if (knight_attacks(ksq) & ei->attackedBy[them][KNIGHT] & safe)
            TERM(sc, KingSafeCheck, KNIGHT, sign);
        if (bishop_attacks(ksq, occ) & ei->attackedBy[them][BISHOP] & safe)
            TERM(sc, KingSafeCheck, BISHOP, sign);
        if (rook_attacks(ksq, occ) & ei->attackedBy[them][ROOK] & safe)
            TERM(sc, KingSafeCheck, ROOK, sign);
        if (queen_attacks(ksq, occ) & ei->attackedBy[them][QUEEN] & safe)
            TERM(sc, KingSafeCheck, QUEEN, sign);
    }
}

static void eval_threats(const Position *pos, const EvalInfo *ei, Score *sc) {
    for (Color c = WHITE; c <= BLACK; ++c) {
        const Color them   = (Color)(c ^ 1);
        const int sign     = SIGN(c);
        const Bitboard win = color_bb(pos, them);

        /* Attacked and undefended. */
        const Bitboard weak = win & ei->attackedAll[c] & ~ei->attackedAll[them];

        const Bitboard nonPawn = win & ~pieces_bb(pos, them, PAWN);

        Bitboard b = nonPawn & ei->attackedBy[c][PAWN];
        while (b)
            TERM(sc, ThreatByPawn, type_of(piece_on(pos, pop_lsb(&b))), sign);

        b = nonPawn & (ei->attackedBy[c][KNIGHT] | ei->attackedBy[c][BISHOP]);
        while (b)
            TERM(sc, ThreatByMinor, type_of(piece_on(pos, pop_lsb(&b))), sign);

        b = weak & ei->attackedBy[c][ROOK];
        while (b)
            TERM(sc, ThreatByRook, type_of(piece_on(pos, pop_lsb(&b))), sign);

        b = weak & ei->attackedBy[c][KING];
        while (b)
            TERM(sc, ThreatByKing, type_of(piece_on(pos, pop_lsb(&b))), sign);

        if (weak)
            TERM(sc, Hanging, 0, sign * popcount(weak));

        /* Squares both sides cover that the enemy does not hold firmly. */
        const Bitboard strong     = ei->attackedBy[them][PAWN] | ei->attackedBy2[them];
        const Bitboard restricted = ei->attackedAll[them] & ei->attackedAll[c] & ~strong;
        if (restricted)
            TERM(sc, Restricted, 0, sign * popcount(restricted));
    }
}

Value eval_classical(const Position *pos) {
    EvalInfo ei;
    Score sc = {0, 0};

    eval_init_info(pos, &ei);
    eval_placement(pos, &ei, &sc);
    eval_pawns(pos, &ei, &sc);
    eval_pieces(pos, &ei, &sc);
    eval_king(pos, &ei, &sc);
    eval_threats(pos, &ei, &sc);

    TERM(&sc, Tempo, 0, SIGN(pos->sideToMove));

    const int phase   = game_phase(pos);
    const Value score = (Value)((sc.mg * phase + sc.eg * (PHASE_MAX - phase)) / PHASE_MAX);

    return pos->sideToMove == WHITE ? score : -score;
}

/* src/nnue.c defines this symbol in an NNUE build. eval_classical() exists in every
 * build, for `eval` and the tuner. */
#ifndef EVAL_NNUE
Value eval_evaluate(EvalState *es, const Position *pos) {
    (void)es;
    return eval_classical(pos);
}
#endif

void eval_trace(const Position *pos) {
    static const char *const Names[] = {"Material + placement", "Pawn structure", "Pieces",
                                        "King safety",          "Threats",        "Tempo"};
    EvalInfo ei;
    Score parts[6];

    memset(parts, 0, sizeof(parts));
    eval_init_info(pos, &ei);

    /* The same helpers in the same order, so the parts sum to eval_classical(). */
    eval_placement(pos, &ei, &parts[0]);
    eval_pawns(pos, &ei, &parts[1]);
    eval_pieces(pos, &ei, &parts[2]);
    eval_king(pos, &ei, &parts[3]);
    eval_threats(pos, &ei, &parts[4]);
    TERM(&parts[5], Tempo, 0, SIGN(pos->sideToMove));

    const int phase = game_phase(pos);
    Score total     = {0, 0};

    printf("                          midgame   endgame   tapered\n");
    printf("                          -------   -------   -------\n");

    for (int i = 0; i < 6; ++i) {
        total.mg += parts[i].mg;
        total.eg += parts[i].eg;

        const int tapered = (parts[i].mg * phase + parts[i].eg * (PHASE_MAX - phase)) / PHASE_MAX;
        printf("%-22s   %+7.2f   %+7.2f   %+7.2f\n", Names[i], (double)parts[i].mg / 100.0,
               (double)parts[i].eg / 100.0, (double)tapered / 100.0);
    }

    const int tapered = (total.mg * phase + total.eg * (PHASE_MAX - phase)) / PHASE_MAX;

    printf("                          -------   -------   -------\n");
    printf("%-22s   %+7.2f   %+7.2f   %+7.2f\n", "Total (white)", (double)total.mg / 100.0,
           (double)total.eg / 100.0, (double)tapered / 100.0);

    printf("\nPhase: %d/%d (%d%% midgame)\n", phase, PHASE_MAX, phase * 100 / PHASE_MAX);
    printf("Evaluation (side to move): %+.2f\n",
           (double)(pos->sideToMove == WHITE ? tapered : -tapered) / 100.0);
}
