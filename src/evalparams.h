/*
 * evalparams.h - the tunable weights. EVAL_PARAM_TABLES generates the externs, the flat
 * index space and the tuner's descriptor array, so they cannot disagree. To add a term:
 * add a line here, its defaults to evalparams.c, and apply it with TERM() in eval.c.
 */
#ifndef EVALPARAMS_H
#define EVALPARAMS_H

#include "types.h"

/* Midgame and endgame weights. int16_t keeps the whole set inside L2. */
typedef struct {
    int16_t mg, eg;
} Pair;

/* A brace initialiser, since a compound literal cannot initialise a static. The
 * clang-format guards here and below exist because versions 15 and 16 disagree. */
/* clang-format off */
#define S(mg, eg) { (int16_t)(mg), (int16_t)(eg) }
/* clang-format on */

/* Piece placement conditioned on a king's square (normalised as in TERM_PSQK). */
#define KING_BUCKET_NB 8

/* [bucket][piece type][square], PAWN..KING packed into 0..5. */
/* clang-format off */
#define PSQK_SIZE           (KING_BUCKET_NB * 6 * SQUARE_NB)
#define PSQK_IDX(b, pt, sq) ((((b)*6 + ((pt)-PAWN)) * SQUARE_NB) + (sq))
/* clang-format on */

/* X(name, length, columns); `columns` lays out the generated evalparams.c. */
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

#define X(name, len, cols) extern Pair name[len];
EVAL_PARAM_TABLES(X)
#undef X

/* Flat index space: each table starts at PARAM_OFF_<name>. */
/* clang-format off */
#define X(name, len, cols) PARAM_OFF_##name, PARAM_LAST_##name = PARAM_OFF_##name + (len)-1,
/* clang-format on */
enum { EVAL_PARAM_TABLES(X) PARAM_NB };
#undef X

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

/* Well above the ~350 weights one position can touch. */
#define EVAL_TRACE_MAX 1024

/* Under -DTUNE, each weight's coefficient in the untapered score (the tuner's gradient).
 * `EvalTraceDirty` lists the indices touched, so reading out is cheap. */
extern _Thread_local int EvalTrace[PARAM_NB];
extern _Thread_local int EvalTraceDirty[EVAL_TRACE_MAX];
extern _Thread_local int EvalTraceDirtyCount;

/* On overflow the term is dropped: an index written but not listed would never be
 * cleared. The assert is what should fire. */
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
