/*
 * evalparams.h - the tunable weights, and the registry that makes them
 * machine-enumerable.
 *
 * EVAL_PARAM_TABLES is the single list, and it generates the externs, the flat
 * index space naming every weight with one integer, and the descriptor array the
 * tuner walks - so the three cannot disagree. To add a term: add a line here, add
 * its defaults to evalparams.c, and apply it with TERM() in eval.c.
 */
#ifndef EVALPARAMS_H
#define EVALPARAMS_H

#include "types.h"

/* A midgame and an endgame weight, interpolated by game phase at the end of the
 * evaluation. int16_t rather than int: the king-relative tables run to 3072 entries
 * each, and halving them keeps the whole weight set inside L2. */
typedef struct {
    int16_t mg, eg;
} Pair;

/* A brace initialiser, not a compound literal: a compound literal is not a constant
 * expression and so cannot initialise a static array. */
#define S(mg, eg) \
    { (int16_t)(mg), (int16_t)(eg) }

/*
 * Piece placement conditioned on where a king stands. A plain piece-square table has
 * to average a knight on f5 over positions with the enemy king on g8 and on a1, and
 * the king is the most informative thing to condition on.
 *
 * Squares are normalised first: rank-flipped so the owner always looks up the board,
 * then file-mirrored when the king sits kingside, which folds away a symmetry the
 * game does not distinguish.
 */
#define KING_BUCKET_NB 8

/* [bucket][piece type][square], piece types PAWN..KING packed into 0..5. Held off
 * clang-format because 15 and 16+ disagree about whether the `*` and `-` in PSQK_IDX
 * are operators or a pointer and a sign, and each rewrites the other's output. */
/* clang-format off */
#define PSQK_SIZE           (KING_BUCKET_NB * 6 * SQUARE_NB)
#define PSQK_IDX(b, pt, sq) ((((b)*6 + ((pt)-PAWN)) * SQUARE_NB) + (sq))
/* clang-format on */

/* X(name, length, columns). `columns` is a pretty-printing hint for when the tuner
 * regenerates evalparams.c; 8 lays a piece-square table out as a board. */
/* clang-format off */
#define EVAL_PARAM_TABLES(X)                                                    \
    /* material and unconditional placement */ \
    X(Material,            PIECE_TYPE_NB,  7)                                   \
    X(PsqPawn,             SQUARE_NB,      8)                                   \
    X(PsqKnight,           SQUARE_NB,      8)                                   \
    X(PsqBishop,           SQUARE_NB,      8)                                   \
    X(PsqRook,             SQUARE_NB,      8)                                   \
    X(PsqQueen,            SQUARE_NB,      8)                                   \
    X(PsqKing,             SQUARE_NB,      8)                                   \
    /* placement conditioned on king position */ \
    X(PsqOwnKing,          PSQK_SIZE,      8)                                   \
    X(PsqEnemyKing,        PSQK_SIZE,      8)                                   \
    /* mobility, indexed by the number of safe squares reached */ \
    X(MobilityKnight,      9,              9)                                   \
    X(MobilityBishop,      14,             7)                                   \
    X(MobilityRook,        15,             8)                                   \
    X(MobilityQueen,       28,             7)                                   \
    /* pawn structure */ \
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
    /* king safety */ \
    X(KingShelter,         32,             8)                                   \
    X(KingStorm,           32,             8)                                   \
    X(KingAttackers,       8,              8)                                   \
    X(KingAttackWeight,    PIECE_TYPE_NB,  7)                                   \
    X(KingRingAttacks,     16,             8)                                   \
    X(KingSafeCheck,       PIECE_TYPE_NB,  7)                                   \
    X(KingOnOpenFile,      3,              3)                                   \
    /* individual pieces */ \
    X(BishopPair,          1,              1)                                   \
    X(BishopBadPawns,      9,              9)                                   \
    X(KnightOutpost,       2,              2)                                   \
    X(BishopOutpost,       2,              2)                                   \
    X(RookOpenFile,        2,              2)                                   \
    X(RookOnSeventh,       1,              1)                                   \
    /* threats */ \
    X(ThreatByPawn,        PIECE_TYPE_NB,  7)                                   \
    X(ThreatByMinor,       PIECE_TYPE_NB,  7)                                   \
    X(ThreatByRook,        PIECE_TYPE_NB,  7)                                   \
    X(ThreatByKing,        PIECE_TYPE_NB,  7)                                   \
    X(Hanging,             1,              1)                                   \
    X(Restricted,          1,              1)                                   \
    /* having the move is worth something on its own */ \
    X(Tempo,               1,              1)
/* clang-format on */

/* The tables themselves. */
#define X(name, len, cols) extern Pair name[len];
EVAL_PARAM_TABLES(X)
#undef X

/* Flat index space: each table starts at PARAM_OFF_<name>, and PARAM_LAST_<name>
 * pins its last weight, so the next offset lands with no arithmetic left to get
 * wrong. The `-1` is disputed between clang-format versions like PSQK_IDX above. */
/* clang-format off */
#define X(name, len, cols) PARAM_OFF_##name, PARAM_LAST_##name = PARAM_OFF_##name + (len)-1,
/* clang-format on */
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

#ifdef TUNE

#include <assert.h>

/* Comfortably above the ~350 distinct weights a single position can touch. */
#define EVAL_TRACE_MAX 1024

/*
 * Under -DTUNE the evaluation records, per flat parameter index, the signed
 * coefficient it was multiplied by - the gradient of the untapered score, and the
 * only thing the tuner needs. `EvalTraceDirty` lists the indices actually touched,
 * so reading the trace out costs one pass over the ~200 weights a position uses
 * rather than over all PARAM_NB.
 */
extern _Thread_local int EvalTrace[PARAM_NB];
extern _Thread_local int EvalTraceDirty[EVAL_TRACE_MAX];
extern _Thread_local int EvalTraceDirtyCount;

/* Overflow must NOT fall through to the accumulate: an index written but never
 * recorded is never cleared either, so it leaks into every position the thread
 * evaluates afterwards. Dropping the coefficient costs one term and cannot
 * propagate; the assertion is what should actually fire. */
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

#endif
