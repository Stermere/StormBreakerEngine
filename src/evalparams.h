/*
 * evalparams.h - the tunable weights of the evaluation, and the registry that
 * makes them machine-enumerable.
 *
 * Every number the evaluation adds to a score lives in one of the tables
 * declared here, and every table is listed exactly once in EVAL_PARAM_TABLES.
 * That single list generates three things which must never disagree:
 *
 *   1. the extern declarations of the tables themselves,
 *   2. a flat index space (PARAM_OFF_<table> .. PARAM_NB) naming every
 *      individual weight with one integer, and
 *   3. the descriptor array the tuner walks to read weights out and write
 *      tuned ones back.
 *
 * Deriving all three from one list is the whole point. A hand-maintained
 * second copy of the parameter layout is a silent correctness bug waiting to
 * happen: the tuner would optimise one weight while the evaluation applied a
 * different one, and nothing would crash - the engine would simply get quietly
 * worse while the tuner reported that it was getting better.
 *
 * TO ADD A TERM: add a line to EVAL_PARAM_TABLES, add its defaults to
 * evalparams.c, and apply it with TERM() in eval.c. Nothing else changes, and
 * `make tuner` picks it up automatically.
 */
#ifndef EVALPARAMS_H
#define EVALPARAMS_H

#include "types.h"

/*
 * A midgame and an endgame weight, interpolated by game phase at the end of
 * the evaluation. int16_t rather than int: the king-relative tables below run
 * to 3072 entries each, and halving them keeps the whole weight set inside L2,
 * which is what the evaluation's scattered access pattern can afford.
 */
typedef struct {
    int16_t mg, eg;
} Pair;

/* A brace initialiser, not a compound literal: a compound literal is not a
 * constant expression and so cannot initialise a static array. */
#define S(mg, eg) \
    { (int16_t)(mg), (int16_t)(eg) }

/* --------------------------------------------------- king-relative tables -- */

/*
 * The capacity tier: piece placement conditioned on where a king stands.
 *
 * A plain piece-square table has to average a knight on f5 over positions
 * where the enemy king is on g8 and positions where it is on a1, which are not
 * remotely the same square. Conditioning on the king turns one averaged number
 * into eight specialised ones, and the king is by far the most informative
 * thing to condition on: it is what makes an attacking square an attacking
 * square.
 *
 * The feature set is deliberately NNUE-shaped (a factorised HalfKA), for two
 * reasons. It is the cheapest honest way to spend six thousand parameters, and
 * when the network arrives this exact extraction becomes its input layer, so
 * none of the work here is thrown away.
 *
 * Squares are normalised before indexing: rank-flipped so the piece's owner
 * always looks up the board, then file-mirrored when the king sits on the
 * kingside, which folds away a symmetry the game does not distinguish.
 */
#define KING_BUCKET_NB 8

/* [bucket][piece type][square], piece types PAWN..KING packed into 0..5. */
#define PSQK_SIZE           (KING_BUCKET_NB * 6 * SQUARE_NB)
#define PSQK_IDX(b, pt, sq) ((((b)*6 + ((pt)-PAWN)) * SQUARE_NB) + (sq))

/* ---------------------------------------------------------- the registry -- */

/*
 * X(name, length, columns)
 *
 * `columns` is purely a pretty-printing hint for the tuner when it regenerates
 * evalparams.c; 8 lays a piece-square table out as a board.
 */
/* clang-format off */
#define EVAL_PARAM_TABLES(X)                                                    \
    /* --- material and unconditional placement --- */                          \
    X(Material,            PIECE_TYPE_NB,  7)                                   \
    X(PsqPawn,             SQUARE_NB,      8)                                   \
    X(PsqKnight,           SQUARE_NB,      8)                                   \
    X(PsqBishop,           SQUARE_NB,      8)                                   \
    X(PsqRook,             SQUARE_NB,      8)                                   \
    X(PsqQueen,            SQUARE_NB,      8)                                   \
    X(PsqKing,             SQUARE_NB,      8)                                   \
    /* --- placement conditioned on king position (see above) --- */            \
    X(PsqOwnKing,          PSQK_SIZE,      8)                                   \
    X(PsqEnemyKing,        PSQK_SIZE,      8)                                   \
    /* --- mobility, indexed by the number of safe squares reached --- */       \
    X(MobilityKnight,      9,              9)                                   \
    X(MobilityBishop,      14,             7)                                   \
    X(MobilityRook,        15,             8)                                   \
    X(MobilityQueen,       28,             7)                                   \
    /* --- pawn structure --- */                                                \
    X(PawnIsolated,        8,              8)                                   \
    X(PawnDoubled,         8,              8)                                   \
    X(PawnBackward,        8,              8)                                   \
    X(PawnConnected,       8,              8)                                   \
    X(PawnPhalanx,         8,              8)                                   \
    X(PawnPassed,          8,              8)                                   \
    X(PawnPassedBlocked,   8,              8)                                   \
    X(PawnPassedDefended,  8,              8)                                   \
    X(PawnPassedOwnKing,   8,              8)                                   \
    X(PawnPassedEnemyKing, 8,              8)                                   \
    X(PawnCandidate,       8,              8)                                   \
    /* --- king safety --- */                                                   \
    X(KingShelter,         32,             8)                                   \
    X(KingStorm,           32,             8)                                   \
    X(KingAttackers,       8,              8)                                   \
    X(KingAttackWeight,    PIECE_TYPE_NB,  7)                                   \
    X(KingRingAttacks,     16,             8)                                   \
    X(KingSafeCheck,       PIECE_TYPE_NB,  7)                                   \
    X(KingOnOpenFile,      3,              3)                                   \
    /* --- individual pieces --- */                                             \
    X(BishopPair,          1,              1)                                   \
    X(BishopBadPawns,      9,              9)                                   \
    X(KnightOutpost,       2,              2)                                   \
    X(BishopOutpost,       2,              2)                                   \
    X(RookOpenFile,        2,              2)                                   \
    X(RookOnSeventh,       1,              1)                                   \
    /* --- threats --- */                                                       \
    X(ThreatByPawn,        PIECE_TYPE_NB,  7)                                   \
    X(ThreatByMinor,       PIECE_TYPE_NB,  7)                                   \
    X(ThreatByRook,        PIECE_TYPE_NB,  7)                                   \
    X(ThreatByKing,        PIECE_TYPE_NB,  7)                                   \
    X(Hanging,             1,              1)                                   \
    X(Restricted,          1,              1)                                   \
    /* --- having the move is worth something on its own --- */                 \
    X(Tempo,               1,              1)
/* clang-format on */

/* The tables themselves. */
#define X(name, len, cols) extern Pair name[len];
EVAL_PARAM_TABLES(X)
#undef X

/*
 * Flat index space. Each table's first weight is PARAM_OFF_<name>, and because
 * PARAM_LAST_<name> pins its last one, the next table's offset lands exactly
 * where it should with no arithmetic left to get wrong. PARAM_NB is the total.
 */
#define X(name, len, cols) PARAM_OFF_##name, PARAM_LAST_##name = PARAM_OFF_##name + (len)-1,
enum { EVAL_PARAM_TABLES(X) PARAM_NB };
#undef X

/* What the tuner walks: name, storage, length, print width. */
typedef struct {
    const char *name;
    Pair *values;
    int length;
    int columns;
} ParamTable;

extern const ParamTable EvalParamTables[];
extern const int EvalParamTableCount;

/* ---------------------------------------------------------------- trace --- */

/*
 * Under -DTUNE the evaluation records, for every flat parameter index, the
 * signed coefficient it was multiplied by (white positive, black negative).
 * That vector is the gradient of the untapered score with respect to the
 * weights, which is all the tuner needs - and because TERM() in eval.c derives
 * the score contribution and the trace entry from the same expression, the two
 * cannot drift apart.
 *
 * `EvalTraceDirty` lists the indices actually touched, so reading the trace out
 * and clearing it afterwards costs one pass over the ~200 weights a position
 * uses rather than over all PARAM_NB of them. At eight million positions per
 * tuning run that difference is the whole runtime.
 *
 * Thread-local: the tuner evaluates positions on every core at once.
 */
#ifdef TUNE

#include <assert.h>

/* Comfortably above the number of distinct weights any one position can
 * touch - see the ceiling estimated beside TRACE_ADD below. */
#define EVAL_TRACE_MAX 1024

extern _Thread_local int EvalTrace[PARAM_NB];
extern _Thread_local int EvalTraceDirty[EVAL_TRACE_MAX];
extern _Thread_local int EvalTraceDirtyCount;

/* An index whose coefficients cancel back to zero stays on the dirty list with
 * value zero; the reader skips zeros, so that is harmless.
 *
 * Overflowing the dirty list must NOT fall through to the accumulate. An index
 * that is written but never recorded is never cleared either, so it leaks into
 * every position the thread evaluates afterwards - a silent, unbounded, and
 * completely invisible corruption of the gradient. Dropping the coefficient
 * instead costs one term in one position and cannot propagate. The assertion
 * is what should actually fire: ~350 distinct weights is the realistic ceiling
 * for a single position, so reaching 1024 means a new term is far larger than
 * anyone intended and EVAL_TRACE_MAX needs raising. */
#define TRACE_ADD(idx, c)                                 \
    do {                                                  \
        const int t_ = (idx);                             \
        if (EvalTrace[t_] == 0) {                         \
            assert(EvalTraceDirtyCount < EVAL_TRACE_MAX); \
            if (EvalTraceDirtyCount >= EVAL_TRACE_MAX)    \
                break;                                    \
            EvalTraceDirty[EvalTraceDirtyCount++] = t_;   \
        }                                                 \
        EvalTrace[t_] += (c);                             \
    } while (0)
#else
#define TRACE_ADD(idx, c) ((void)0)
#endif

#endif /* EVALPARAMS_H */
