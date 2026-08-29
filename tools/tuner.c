/*
 * tuner.c - offline evaluation fitting. Not part of the engine binary.
 *
 * Two subcommands, which together are the whole pipeline from a database of
 * games to a regenerated src/evalparams.c:
 *
 *     tuner extract <pgn>... -o data.epd     games      -> labelled positions
 *     tuner tune    <data.epd> -o out.c      positions  -> fitted weights
 *
 * WHAT IS BEING FITTED. The evaluation is linear in its weights: every term is
 * a weight multiplied by a coefficient that depends only on the position, so a
 * position's score is a dot product. eval.c already reports those coefficients
 * through EvalTrace, which means the tuner never has to model what the
 * evaluation does - it reads out what the evaluation did. Add a term to eval.c
 * and this file fits it with no changes.
 *
 * WHAT THE LABEL IS. Each position carries the result of the game it came
 * from, 1 / 0.5 / 0 from white's point of view. Fitting a sigmoid of the score
 * to that result is logistic regression on win/draw/loss, and it is the only
 * signal available that does not come from the evaluation being replaced -
 * which is the entire reason it is worth more than a stronger but circular one.
 *
 * The scale K inside the sigmoid is fitted FIRST, on the starting weights, and
 * then frozen. K and the overall magnitude of the weights are the same degree
 * of freedom, so letting both move would leave the score free to drift off the
 * centipawn scale that every margin in search.c is written in.
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bitboard.h"
#include "board.h"
#include "eval.h"
#include "evalparams.h"
#include "movegen.h"
#include "thread.h"
#include "timeman.h"
#include "zobrist.h"

#define MAX_THREADS 64

static int nthreads_option = 0;

static int worker_count(void) {
    if (nthreads_option > 0)
        return nthreads_option < MAX_THREADS ? nthreads_option : MAX_THREADS;
    int n = thread_hardware_concurrency() - 2; /* leave the machine usable */
    if (n < 1)
        n = 1;
    return n < MAX_THREADS ? n : MAX_THREADS;
}

/* Wall clock. Deliberately not clock(): that is wall time on Windows but
 * CPU time on POSIX, so every duration printed by a threaded run would be
 * inflated by the thread count on Linux. */
static double now_seconds(void) { return (double)time_ms() / 1000.0; }

static void die(const char *msg) {
    fprintf(stderr, "tuner: %s\n", msg);
    exit(1);
}

static void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p)
        die("out of memory");
    return p;
}

/* ========================================================================== */
/*  Subcommand: extract                                                       */
/* ========================================================================== */

typedef struct {
    int minPly;     /* skip this many plies of opening theory */
    int maxPly;     /* stop sampling past this ply; 0 for no cap */
    int stride;     /* keep one in this many eligible positions */
    int minElo;     /* skip games below this rating */
    int minBase;    /* skip games whose base time control is below this (s) */
    int maxScore;   /* skip positions already decided by this margin (cp) */
    int qsearchPly; /* depth of the quiescence used by the quiet filter */
} ExtractOpts;

typedef struct {
    ExtractOpts opt;
    FILE *out;
    Position *pos;
    uint64_t games, gamesUsed, positions;
    uint64_t rejectedElo, rejectedTc, rejectedResult, badSan;
    unsigned rng;
} Extractor;

/* xorshift: the sampling stride needs a different phase per game, and a
 * deterministic one so a re-run reproduces the same dataset exactly. */
static unsigned next_rand(unsigned *s) {
    unsigned x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}

/* ------------------------------------------------------------ SAN parsing -- */

/*
 * Turn one SAN token into a move by generating the legal moves and keeping the
 * one that matches every constraint the token expresses. Slower than decoding
 * the notation directly, but it cannot produce a move that is not legal in the
 * position, which is what matters when reading millions of games written by
 * software of varying quality.
 */
static Move parse_san(const Position *pos, const char *san) {
    char buf[32];
    size_t n = 0;

    for (const char *p = san; *p && n < sizeof(buf) - 1; ++p)
        if (*p != '+' && *p != '#' && *p != '!' && *p != '?')
            buf[n++] = *p;
    buf[n] = '\0';
    if (n < 2)
        return MOVE_NONE;

    ScoredMove list[MAX_MOVES];
    const int count = movegen_generate(pos, GEN_ALL, list);

    /* Castling is spelled by destination in SAN but encoded king-captures-rook
     * internally, so it cannot go through the square matcher below. */
    if (!strncmp(buf, "O-O-O", 5) || !strncmp(buf, "0-0-0", 5) || !strncmp(buf, "O-O", 3) ||
        !strncmp(buf, "0-0", 3)) {
        const bool queenside = (buf[3] == '-');
        for (int i = 0; i < count; ++i) {
            const Move m = list[i].m;
            if (type_of_move(m) != MT_CASTLING || !movegen_is_legal(pos, m))
                continue;
            if ((to_sq(m) < from_sq(m)) == queenside)
                return m;
        }
        return MOVE_NONE;
    }

    PieceType pt = PAWN;
    size_t i     = 0;
    if (strchr("NBRQK", buf[0])) {
        pt = buf[0] == 'N'   ? KNIGHT
             : buf[0] == 'B' ? BISHOP
             : buf[0] == 'R' ? ROOK
             : buf[0] == 'Q' ? QUEEN
                             : KING;
        i  = 1;
    }

    PieceType promo = NO_PIECE_TYPE;
    char *eq        = strchr(buf, '=');
    if (eq) {
        promo = eq[1] == 'N' ? KNIGHT : eq[1] == 'B' ? BISHOP : eq[1] == 'R' ? ROOK : QUEEN;
        *eq   = '\0';
        n     = (size_t)(eq - buf);
    }

    if (n < i + 2)
        return MOVE_NONE;

    /* The destination is always the last two characters. */
    const File toFile = (File)(buf[n - 2] - 'a');
    const Rank toRank = (Rank)(buf[n - 1] - '1');
    if (toFile < FILE_A || toFile > FILE_H || toRank < RANK_1 || toRank > RANK_8)
        return MOVE_NONE;
    const Square to = make_square(toFile, toRank);

    /* Anything between the piece letter and the destination disambiguates. */
    int fromFile = -1, fromRank = -1;
    for (size_t k = i; k + 2 < n; ++k) {
        if (buf[k] >= 'a' && buf[k] <= 'h')
            fromFile = buf[k] - 'a';
        else if (buf[k] >= '1' && buf[k] <= '8')
            fromRank = buf[k] - '1';
    }

    for (int k = 0; k < count; ++k) {
        const Move m = list[k].m;

        if (to_sq(m) != to || type_of_move(m) == MT_CASTLING)
            continue;
        if (type_of(piece_on(pos, from_sq(m))) != pt)
            continue;
        if (promo != NO_PIECE_TYPE) {
            if (type_of_move(m) != MT_PROMOTION || promotion_type(m) != promo)
                continue;
        } else if (type_of_move(m) == MT_PROMOTION) {
            continue;
        }
        if (fromFile >= 0 && (int)file_of(from_sq(m)) != fromFile)
            continue;
        if (fromRank >= 0 && (int)rank_of(from_sq(m)) != fromRank)
            continue;
        if (!movegen_is_legal(pos, m))
            continue;

        return m;
    }
    return MOVE_NONE;
}

/* -------------------------------------------------------- quiet filtering -- */

/*
 * A position is kept only if standing pat already beats every capture - i.e.
 * the static evaluation is not about to be contradicted by a two-move tactic.
 *
 * Fitting on tactical positions teaches the evaluation to predict the outcome
 * of a capture sequence, which is the search's job and which the evaluation has
 * no means to represent. Those positions are pure label noise here.
 */
static Value quiesce(Position *pos, Value alpha, Value beta, int depth) {
    const bool inCheck = board_checkers(pos) != 0;
    Value best         = -VALUE_INFINITE;

    if (!inCheck) {
        best = eval_classical(pos);
        if (best >= beta || depth <= 0)
            return best;
        if (best > alpha)
            alpha = best;
    } else if (depth <= 0) {
        return eval_classical(pos);
    }

    ScoredMove list[MAX_MOVES];
    const int count = movegen_generate(pos, inCheck ? GEN_EVASIONS : GEN_CAPTURES, list);
    int legal       = 0;

    for (int i = 0; i < count; ++i) {
        if (!movegen_is_legal(pos, list[i].m))
            continue;
        ++legal;

        board_do_move(pos, list[i].m);
        const Value v = -quiesce(pos, -beta, -alpha, depth - 1);
        board_undo_move(pos, list[i].m);

        if (v > best)
            best = v;
        if (v >= beta)
            return v;
        if (v > alpha)
            alpha = v;
    }

    if (inCheck && legal == 0)
        return -VALUE_MATE; /* mated; the caller discards these anyway */
    return best;
}

static bool position_is_quiet(Extractor *ex) {
    if (board_checkers(ex->pos))
        return false;

    const Value stat = eval_classical(ex->pos);
    const Value q    = quiesce(ex->pos, -VALUE_INFINITE, VALUE_INFINITE, ex->opt.qsearchPly);

    if (q != stat)
        return false;
    if (ex->opt.maxScore > 0 && (stat > ex->opt.maxScore || stat < -ex->opt.maxScore))
        return false;
    return true;
}

/* -------------------------------------------------------------- PGN games -- */

typedef struct {
    char result[8];
    char fen[FEN_MAX_LEN];
    char timeControl[32];
    int whiteElo, blackElo;
    bool hasFen;
} GameTags;

/* Truncating copy. snprintf would do, but a PGN tag value is declared to the
 * compiler as up to 511 bytes and every fixed-size destination here is
 * smaller, which makes -Wformat-truncation fire on code that is already
 * correct. Doing the clamp explicitly says the same thing without the noise. */
static void copy_tag(char *dst, size_t cap, const char *src) {
    size_t n = strlen(src);
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void emit_position(Extractor *ex, double label) {
    char fen[FEN_MAX_LEN];
    board_to_fen(ex->pos, fen);
    fprintf(ex->out, "%s [%.1f]\n", fen, label);
    ex->positions++;
}

static void play_game(Extractor *ex, const GameTags *tags, const char *movetext) {
    double label;
    if (!strcmp(tags->result, "1-0"))
        label = 1.0;
    else if (!strcmp(tags->result, "0-1"))
        label = 0.0;
    else if (!strcmp(tags->result, "1/2-1/2"))
        label = 0.5;
    else {
        ex->rejectedResult++;
        return;
    }

    if (ex->opt.minElo > 0 &&
        (tags->whiteElo < ex->opt.minElo || tags->blackElo < ex->opt.minElo)) {
        ex->rejectedElo++;
        return;
    }
    if (ex->opt.minBase > 0) {
        const int base = atoi(tags->timeControl); /* "300+3" -> 300, "-" -> 0 */
        if (base < ex->opt.minBase) {
            ex->rejectedTc++;
            return;
        }
    }

    if (tags->hasFen) {
        if (!board_set_fen(ex->pos, tags->fen))
            return;
    } else {
        board_set_startpos(ex->pos);
    }

    /* Phase the sampling stride per game so we do not always take the same
     * move number, which would bias the set towards one part of the game. */
    int countdown = (int)(next_rand(&ex->rng) % (unsigned)ex->opt.stride);
    int ply       = 0;
    bool used     = false;

    const char *p = movetext;
    char token[32];

    while (*p) {
        /* Past the window there is nothing left to sample, and a PGN is mostly
         * moves this game will never look at: stopping here is what makes
         * `-minply N -maxply N` a cheap pass over a large corpus rather than a
         * full SAN decode of every game in it. */
        if (ex->opt.maxPly > 0 && ply > ex->opt.maxPly)
            break;

        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')
            ++p;
        if (!*p)
            break;

        /* Comments, variations and NAGs carry no moves. */
        if (*p == '{') {
            while (*p && *p != '}')
                ++p;
            if (*p)
                ++p;
            continue;
        }
        if (*p == '(') {
            int depth = 1;
            ++p;
            while (*p && depth) {
                if (*p == '(')
                    ++depth;
                else if (*p == ')')
                    --depth;
                ++p;
            }
            continue;
        }
        if (*p == '$') {
            while (*p && *p != ' ')
                ++p;
            continue;
        }

        size_t n = 0;
        while (*p && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t' && n < sizeof(token) - 1)
            token[n++] = *p++;
        token[n] = '\0';
        if (!n)
            continue;

        /* Move numbers ("12." / "12...") and the result token. */
        if ((token[0] >= '0' && token[0] <= '9') && (strchr(token, '.') || strchr(token, '-')))
            continue;
        if (!strcmp(token, "*"))
            continue;

        const Move m = parse_san(ex->pos, token);
        if (!is_ok_move(m)) {
            ex->badSan++;
            return; /* desynchronised: the rest of this game is unusable */
        }

        /* Sample BEFORE the move, so the position is one a player actually
         * had to evaluate. */
        if (ply >= ex->opt.minPly) {
            if (countdown-- <= 0) {
                countdown = ex->opt.stride - 1;
                if (position_is_quiet(ex)) {
                    emit_position(ex, label);
                    used = true;
                }
            }
        }

        board_do_move(ex->pos, m);
        ++ply;
    }

    ex->games++;
    if (used)
        ex->gamesUsed++;
}

static void extract_file(Extractor *ex, const char *path, int fileIndex) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "tuner: cannot open %s\n", path);
        return;
    }

    /* Seed from the file's index rather than from thread order: the sampling
     * stride is randomised per game, and a dataset that depends on which core
     * happened to pick a file up is not reproducible. */
    ex->rng = 0x2545F491u + 2654435761u * (unsigned)fileIndex;

    GameTags tags;
    memset(&tags, 0, sizeof(tags));

    size_t moveCap = 1 << 16, moveLen = 0;
    char *movetext = xmalloc(moveCap);
    movetext[0]    = '\0';
    bool inMoves   = false;

    char line[8192];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '[') {
            if (inMoves) {
                play_game(ex, &tags, movetext);
                memset(&tags, 0, sizeof(tags));
                moveLen     = 0;
                movetext[0] = '\0';
                inMoves     = false;
            }

            char key[32], value[512];
            if (sscanf(line, "[%31s \"%511[^\"]\"", key, value) == 2) {
                if (!strcmp(key, "Result"))
                    copy_tag(tags.result, sizeof(tags.result), value);
                else if (!strcmp(key, "FEN")) {
                    copy_tag(tags.fen, sizeof(tags.fen), value);
                    tags.hasFen = true;
                } else if (!strcmp(key, "WhiteElo"))
                    tags.whiteElo = atoi(value);
                else if (!strcmp(key, "BlackElo"))
                    tags.blackElo = atoi(value);
                else if (!strcmp(key, "TimeControl"))
                    copy_tag(tags.timeControl, sizeof(tags.timeControl), value);
            }
            continue;
        }

        if (line[0] == '\n' || line[0] == '\r')
            continue;

        inMoves          = true;
        const size_t len = strlen(line);
        if (moveLen + len + 2 > moveCap) {
            moveCap  = (moveLen + len + 2) * 2;
            movetext = realloc(movetext, moveCap);
            if (!movetext)
                die("out of memory");
        }
        memcpy(movetext + moveLen, line, len);
        moveLen           = moveLen + len;
        movetext[moveLen] = '\0';
    }

    if (inMoves)
        play_game(ex, &tags, movetext);

    free(movetext);
    fclose(f);
}

/*
 * One worker per slice of the file list, each writing its own part file. PGN
 * parsing is entirely CPU-bound - the quiescence search deciding whether a
 * position is quiet dominates it - so this scales with cores, and it is the
 * difference between a five-minute extraction and an hour of one.
 */
typedef struct {
    ExtractOpts opt;
    const char **files;
    const int *indices;
    int count;
    char partPath[512];
    uint64_t games, gamesUsed, positions;
    uint64_t rejectedElo, rejectedTc, rejectedResult, badSan;
} ExtractJob;

static void extract_worker(void *arg) {
    ExtractJob *job = (ExtractJob *)arg;

    Extractor ex;
    memset(&ex, 0, sizeof(ex));
    ex.opt = job->opt;
    ex.out = fopen(job->partPath, "wb");
    if (!ex.out)
        die("cannot open a part file");
    ex.pos = xmalloc(sizeof(Position));

    for (int i = 0; i < job->count; ++i) {
        extract_file(&ex, job->files[i], job->indices[i]);
        printf("  %-54s %10llu\n", job->files[i], (unsigned long long)ex.positions);
        fflush(stdout);
    }

    fclose(ex.out);
    free(ex.pos);

    job->games          = ex.games;
    job->gamesUsed      = ex.gamesUsed;
    job->positions      = ex.positions;
    job->rejectedElo    = ex.rejectedElo;
    job->rejectedTc     = ex.rejectedTc;
    job->rejectedResult = ex.rejectedResult;
    job->badSan         = ex.badSan;
}

static int cmd_extract(int argc, char **argv) {
    Extractor ex;
    memset(&ex, 0, sizeof(ex));
    ex.opt.minPly     = 12;
    ex.opt.maxPly     = 0;
    ex.opt.stride     = 6;
    ex.opt.minElo     = 0;
    ex.opt.minBase    = 0;
    ex.opt.maxScore   = 2000;
    ex.opt.qsearchPly = 6;
    ex.rng            = 0x2545F491u;

    const char *out = NULL;
    const char *files[512];
    int fileCount = 0;

    for (int i = 0; i < argc; ++i) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc)
            out = argv[++i];
        else if (!strcmp(argv[i], "-minply") && i + 1 < argc)
            ex.opt.minPly = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-maxply") && i + 1 < argc)
            ex.opt.maxPly = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-stride") && i + 1 < argc)
            ex.opt.stride = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-minelo") && i + 1 < argc)
            ex.opt.minElo = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-minbase") && i + 1 < argc)
            ex.opt.minBase = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-maxscore") && i + 1 < argc)
            ex.opt.maxScore = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-qply") && i + 1 < argc)
            ex.opt.qsearchPly = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-threads") && i + 1 < argc)
            nthreads_option = atoi(argv[++i]);
        else if (argv[i][0] != '-' && fileCount < 512)
            files[fileCount++] = argv[i];
    }

    if (!out || !fileCount)
        die("usage: tuner extract <pgn>... -o <data.epd> [-minply N] [-maxply N]\n"
            "                    [-stride N] [-minelo N] [-minbase SECONDS] [-maxscore CP]\n"
            "                    [-qply N] [-threads N]\n"
            "\n"
            "  -minply N -maxply N together bound the sampling window. Equal values take\n"
            "  at most one position per game, at exactly that ply, which is how an\n"
            "  opening book for `datagen selfplay -book` gets made.");
    if (ex.opt.stride < 1)
        ex.opt.stride = 1;

    int nt = worker_count();
    if (nt > fileCount)
        nt = fileCount;

    printf("extracting: minPly=%d maxPly=%d stride=%d minElo=%d minBase=%ds maxScore=%dcp, "
           "%d threads\n",
           ex.opt.minPly, ex.opt.maxPly, ex.opt.stride, ex.opt.minElo, ex.opt.minBase,
           ex.opt.maxScore, nt);

    ExtractJob *jobs      = xmalloc((size_t)nt * sizeof(ExtractJob));
    ThreadHandle *handles = xmalloc((size_t)nt * sizeof(ThreadHandle));
    const char **slices   = xmalloc((size_t)fileCount * sizeof(char *));
    int *sliceIdx         = xmalloc((size_t)fileCount * sizeof(int));

    /* Deal the files out round-robin, so a worker that happens to draw several
     * large months is not left running alone at the end. */
    int cursor = 0;
    for (int t = 0; t < nt; ++t) {
        jobs[t].opt     = ex.opt;
        jobs[t].files   = slices + cursor;
        jobs[t].indices = sliceIdx + cursor;
        jobs[t].count   = 0;
        for (int i = t; i < fileCount; i += nt) {
            slices[cursor]   = files[i];
            sliceIdx[cursor] = i;
            ++cursor;
            ++jobs[t].count;
        }
        snprintf(jobs[t].partPath, sizeof(jobs[t].partPath), "%s.part%d", out, t);
    }

    const double t0 = now_seconds();
    for (int t = 0; t < nt; ++t)
        if (!thread_create(&handles[t], extract_worker, &jobs[t]))
            die("cannot create an extraction thread");

    Extractor totals;
    memset(&totals, 0, sizeof(totals));
    for (int t = 0; t < nt; ++t) {
        thread_join(handles[t]);
        totals.games += jobs[t].games;
        totals.gamesUsed += jobs[t].gamesUsed;
        totals.positions += jobs[t].positions;
        totals.rejectedElo += jobs[t].rejectedElo;
        totals.rejectedTc += jobs[t].rejectedTc;
        totals.rejectedResult += jobs[t].rejectedResult;
        totals.badSan += jobs[t].badSan;
    }

    /* Stitch the parts together in worker order, which keeps the output a
     * deterministic function of the input file list. */
    printf("\nconcatenating %d part files...\n", nt);
    FILE *final = fopen(out, "wb");
    if (!final)
        die("cannot open output file");

    char *copyBuf = xmalloc(1u << 22);
    for (int t = 0; t < nt; ++t) {
        FILE *part = fopen(jobs[t].partPath, "rb");
        if (!part)
            continue;
        size_t n;
        while ((n = fread(copyBuf, 1, 1u << 22, part)) > 0)
            fwrite(copyBuf, 1, n, final);
        fclose(part);
        remove(jobs[t].partPath);
    }
    free(copyBuf);
    fclose(final);

    printf("\n%llu games read, %llu contributed, %llu positions written in %.1fs\n",
           (unsigned long long)totals.games, (unsigned long long)totals.gamesUsed,
           (unsigned long long)totals.positions, now_seconds() - t0);
    printf("rejected: %llu by rating, %llu by time control, %llu without a result, "
           "%llu with unparsable notation\n",
           (unsigned long long)totals.rejectedElo, (unsigned long long)totals.rejectedTc,
           (unsigned long long)totals.rejectedResult, (unsigned long long)totals.badSan);

    free(jobs);
    free(handles);
    free(slices);
    free(sliceIdx);
    return 0;
}

/* ========================================================================== */
/*  Subcommand: tune                                                          */
/* ========================================================================== */

typedef struct {
    uint16_t index;
    int16_t coeff;
} Feature;

typedef struct {
    uint32_t offset;
    uint16_t count;
    uint8_t pool;
    uint8_t phase;
    float result;
} Sample;

typedef struct {
    Feature *data;
    size_t len, cap;
} Pool;

#define PHASE_MAX 24

static Sample *Samples;
static size_t SampleCount;
static Pool Pools[MAX_THREADS];

/* Weights under optimisation. Kept in double: the int16 tables they come from
 * and go back to cannot represent a gradient step. */
static double *Wmg, *Weg;
static double *Mmg, *Meg, *Vmg, *Veg; /* Adam moments */
static double SigmoidK = 200.0;

/* ------------------------------------------------------------- dataset ----- */

static char *read_whole_file(const char *path, size_t *outLen) {
    FILE *f = fopen(path, "rb");
    if (!f)
        die("cannot open dataset");

    size_t cap = 1u << 26, len = 0;
    char *buf = xmalloc(cap);

    for (;;) {
        if (len + (1u << 22) + 1 > cap) {
            cap *= 2;
            buf = realloc(buf, cap);
            if (!buf)
                die("out of memory reading dataset");
        }
        const size_t n = fread(buf + len, 1, 1u << 22, f);
        len += n;
        if (n < (1u << 22))
            break;
    }
    fclose(f);
    buf[len] = '\0';
    *outLen  = len;
    return buf;
}

typedef struct {
    int id;
    char **lines;
    size_t first, last;
    size_t bad;
} LoadJob;

static void pool_push(Pool *p, uint16_t index, int16_t coeff) {
    /* Sample.offset is 32-bit. One thread would need ~4.3 billion features -
     * roughly 60 million positions on a single thread - to reach this, but a
     * silent wrap would corrupt the dataset in a way nothing downstream could
     * detect, so it is worth one compare per feature. */
    if (p->len >= 0xFFFFFFFFu)
        die("feature pool overflowed 32-bit offsets; use more threads or -max");

    if (p->len == p->cap) {
        p->cap  = p->cap ? p->cap * 2 : (1u << 16);
        p->data = realloc(p->data, p->cap * sizeof(Feature));
        if (!p->data)
            die("out of memory building the feature pool");
    }
    p->data[p->len].index = index;
    p->data[p->len].coeff = coeff;
    p->len++;
}

static int phase_of(const Position *pos) {
    static const int weight[PIECE_TYPE_NB] = {[KNIGHT] = 1, [BISHOP] = 1, [ROOK] = 2, [QUEEN] = 4};
    int phase                              = 0;
    for (PieceType pt = KNIGHT; pt <= QUEEN; ++pt)
        phase += weight[pt] * (piece_count(pos, WHITE, pt) + piece_count(pos, BLACK, pt));
    return phase > PHASE_MAX ? PHASE_MAX : phase;
}

static void load_worker(void *arg) {
    LoadJob *job  = (LoadJob *)arg;
    Pool *pool    = &Pools[job->id];
    Position *pos = xmalloc(sizeof(Position));

    for (size_t i = job->first; i < job->last; ++i) {
        char *line = job->lines[i];

        /* "<fen> [<result>]" */
        char *bracket = strrchr(line, '[');
        if (!bracket) {
            job->bad++;
            continue;
        }
        const double result = atof(bracket + 1);
        *bracket            = '\0';

        if (!board_set_fen(pos, line)) {
            job->bad++;
            continue;
        }

        EvalTraceDirtyCount = 0;
        eval_classical(pos);

        Sample *s = &Samples[i];
        s->offset = (uint32_t)pool->len;
        s->pool   = (uint8_t)job->id;
        s->phase  = (uint8_t)phase_of(pos);
        s->result = (float)result;

        /*
         * The dirty list is what makes reading the trace out cheap, but if a
         * position ever touched more indices than it can hold, the ones it
         * could not record would never be cleared and would leak into every
         * later position. A position uses about 200 of the 1024 slots, so this
         * should never fire - and if it does, a full clear is correct rather
         * than merely safe.
         */
        const bool overflowed = EvalTraceDirtyCount >= EVAL_TRACE_MAX;

        int kept = 0;
        for (int d = 0; d < EvalTraceDirtyCount; ++d) {
            const int idx  = EvalTraceDirty[d];
            const int c    = EvalTrace[idx];
            EvalTrace[idx] = 0;
            if (c) {
                pool_push(pool, (uint16_t)idx, (int16_t)c);
                ++kept;
            }
        }
        if (overflowed)
            memset(EvalTrace, 0, sizeof(EvalTrace));

        EvalTraceDirtyCount = 0;
        s->count            = (uint16_t)kept;
    }

    free(pos);
}

static void load_dataset(const char *path, size_t maxPositions) {
    size_t len;
    char *raw = read_whole_file(path, &len);

    /* Index the lines in place. */
    size_t capacity = 1u << 20, count = 0;
    char **lines = xmalloc(capacity * sizeof(char *));
    char *p      = raw;

    while (p < raw + len && count < maxPositions) {
        char *nl = strchr(p, '\n');
        if (!nl)
            nl = raw + len;
        else
            *nl = '\0';

        if (nl > p && nl[-1] == '\r')
            nl[-1] = '\0';

        if (*p) {
            if (count == capacity) {
                capacity *= 2;
                lines = realloc(lines, capacity * sizeof(char *));
                if (!lines)
                    die("out of memory indexing the dataset");
            }
            lines[count++] = p;
        }
        p = nl + 1;
    }

    SampleCount = count;
    Samples     = xmalloc(count * sizeof(Sample));

    const int nt = worker_count();
    LoadJob jobs[MAX_THREADS];
    ThreadHandle handles[MAX_THREADS];

    printf("loading %llu positions on %d threads...\n", (unsigned long long)count, nt);
    const double t0 = now_seconds();

    for (int i = 0; i < nt; ++i) {
        jobs[i].id    = i;
        jobs[i].lines = lines;
        jobs[i].first = count * (size_t)i / (size_t)nt;
        jobs[i].last  = count * (size_t)(i + 1) / (size_t)nt;
        jobs[i].bad   = 0;
        if (!thread_create(&handles[i], load_worker, &jobs[i]))
            die("cannot create a loader thread");
    }

    size_t bad = 0, features = 0;
    for (int i = 0; i < nt; ++i) {
        thread_join(handles[i]);
        bad += jobs[i].bad;
        features += Pools[i].len;
    }

    printf("  %llu positions, %llu features (%.1f per position), %llu unusable, %.1fs\n",
           (unsigned long long)count, (unsigned long long)features,
           count ? (double)features / (double)count : 0.0, (unsigned long long)bad,
           now_seconds() - t0);
    printf("  feature pool: %.2f GB\n", (double)(features * sizeof(Feature)) / 1e9);

    free(lines);
    /* `raw` is deliberately leaked: the FEN strings are no longer needed, but
     * freeing it here would only matter if this were a long-running process. */
}

/* --------------------------------------------------------------- fitting -- */

static inline double sample_score(const Sample *s, const double *wmg, const double *weg) {
    const Feature *f = Pools[s->pool].data + s->offset;
    double mg = 0.0, eg = 0.0;

    for (int i = 0; i < s->count; ++i) {
        mg += (double)f[i].coeff * wmg[f[i].index];
        eg += (double)f[i].coeff * weg[f[i].index];
    }
    return (mg * s->phase + eg * (PHASE_MAX - s->phase)) / PHASE_MAX;
}

static inline double sigmoid(double score) { return 1.0 / (1.0 + exp(-score / SigmoidK)); }

typedef struct {
    size_t first, last;
    double *gmg, *geg;
    double loss;
    bool gradient;
} FitJob;

static void fit_worker(void *arg) {
    FitJob *job  = (FitJob *)arg;
    double total = 0.0;

    if (job->gradient) {
        memset(job->gmg, 0, PARAM_NB * sizeof(double));
        memset(job->geg, 0, PARAM_NB * sizeof(double));
    }

    for (size_t i = job->first; i < job->last; ++i) {
        const Sample *s   = &Samples[i];
        const double sc   = sample_score(s, Wmg, Weg);
        const double pred = sigmoid(sc);
        const double err  = (double)s->result - pred;

        total += err * err;

        if (!job->gradient)
            continue;

        /* d/dscore of (result - sigmoid(score/K))^2 */
        const double base = -2.0 * err * pred * (1.0 - pred) / SigmoidK;
        const double amg  = base * s->phase / PHASE_MAX;
        const double aeg  = base * (PHASE_MAX - s->phase) / PHASE_MAX;

        const Feature *f = Pools[s->pool].data + s->offset;
        for (int k = 0; k < s->count; ++k) {
            job->gmg[f[k].index] += amg * f[k].coeff;
            job->geg[f[k].index] += aeg * f[k].coeff;
        }
    }
    job->loss = total;
}

static double run_jobs(size_t first, size_t last, bool gradient, FitJob *jobs,
                       ThreadHandle *handles, int nt) {
    for (int i = 0; i < nt; ++i) {
        jobs[i].first    = first + (last - first) * (size_t)i / (size_t)nt;
        jobs[i].last     = first + (last - first) * (size_t)(i + 1) / (size_t)nt;
        jobs[i].gradient = gradient;
        if (!thread_create(&handles[i], fit_worker, &jobs[i]))
            die("cannot create a fitting thread");
    }
    double loss = 0.0;
    for (int i = 0; i < nt; ++i) {
        thread_join(handles[i]);
        loss += jobs[i].loss;
    }
    return (last > first) ? loss / (double)(last - first) : 0.0;
}

/* True for the two king-relative tables, the only ones weight decay applies
 * to - see the note where it is used. */
static bool is_king_relative(int index) {
    return (index >= PARAM_OFF_PsqOwnKing && index <= PARAM_LAST_PsqOwnKing) ||
           (index >= PARAM_OFF_PsqEnemyKing && index <= PARAM_LAST_PsqEnemyKing);
}

static void weights_from_tables(void) {
    for (int t = 0; t < EvalParamTableCount; ++t) {
        const ParamTable *pt = &EvalParamTables[t];
        int base             = 0;
        for (int u = 0; u < t; ++u)
            base += EvalParamTables[u].length;
        for (int i = 0; i < pt->length; ++i) {
            Wmg[base + i] = pt->values[i].mg;
            Weg[base + i] = pt->values[i].eg;
        }
    }
}

static void weights_to_tables(const double *wmg, const double *weg) {
    int base = 0;
    for (int t = 0; t < EvalParamTableCount; ++t) {
        const ParamTable *pt = &EvalParamTables[t];
        for (int i = 0; i < pt->length; ++i) {
            pt->values[i].mg = (int16_t)lround(wmg[base + i]);
            pt->values[i].eg = (int16_t)lround(weg[base + i]);
        }
        base += pt->length;
    }
}

/* ---------------------------------------------------------------- output -- */

static void write_params(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f)
        die("cannot open the output source file");

    fprintf(f, "/*\n"
               " * evalparams.c - the evaluation's weights.\n"
               " *\n"
               " * GENERATED DATA - do not edit by hand. What each term MEANS is\n"
               " * documented in evalparams.h (the registry) and eval.c (where the terms\n"
               " * are applied); this file is only the numbers. Regenerate it with:\n"
               " *\n"
               " *     make tuner\n"
               " *     ./tuner tune <data.epd> -o src/evalparams.c\n"
               " *\n"
               " * See docs/TUNING.md for how the data is produced and what the fit\n"
               " * actually optimises.\n"
               " *\n"
               " * Piece-square tables are in BOARD ORDER: index 0 is A1, so the first\n"
               " * row printed is rank 1. They are read from the moving side's point of\n"
               " * view, so black looks them up rank-flipped.\n"
               " */\n"
               "#include \"evalparams.h\"\n\n");

    for (int t = 0; t < EvalParamTableCount; ++t) {
        const ParamTable *pt = &EvalParamTables[t];

        fprintf(f, "/* clang-format off */\n");
        fprintf(f, "Pair %s[%d] = {\n", pt->name, pt->length);
        for (int i = 0; i < pt->length; ++i) {
            if (i % pt->columns == 0)
                fprintf(f, "    ");
            fprintf(f, "S(%4d,%4d),", pt->values[i].mg, pt->values[i].eg);
            if ((i + 1) % pt->columns == 0 || i + 1 == pt->length)
                fprintf(f, "\n");
            else
                fprintf(f, " ");
        }
        fprintf(f, "};\n/* clang-format on */\n\n");
    }

    fprintf(f, "/*\n"
               " * The descriptor list the tuner walks. Generated from the same registry\n"
               " * as the flat offsets, so a table cannot appear in one and not the other.\n"
               " */\n"
               "/* clang-format off */\n"
               "const ParamTable EvalParamTables[] = {\n"
               "#define X(name, len, cols) {#name, name, len, cols},\n"
               "    EVAL_PARAM_TABLES(X)\n"
               "#undef X\n"
               "};\n"
               "/* clang-format on */\n\n"
               "const int EvalParamTableCount = (int)(sizeof(EvalParamTables) / "
               "sizeof(EvalParamTables[0]));\n\n"
               "#ifdef TUNE\n"
               "_Thread_local int EvalTrace[PARAM_NB];\n"
               "_Thread_local int EvalTraceDirty[EVAL_TRACE_MAX];\n"
               "_Thread_local int EvalTraceDirtyCount;\n"
               "#endif\n");

    fclose(f);
}

static void report_notable(void) {
    static const char *const names[] = {"", "pawn", "knight", "bishop", "rook", "queen", ""};

    printf("\nfitted highlights\n");
    printf("  material (mg/eg):");
    for (PieceType pt = PAWN; pt <= QUEEN; ++pt)
        printf("  %s %d/%d", names[pt], Material[pt].mg, Material[pt].eg);
    printf("\n  bishop pair %d/%d   tempo %d/%d   rook open file %d/%d\n", BishopPair[0].mg,
           BishopPair[0].eg, Tempo[0].mg, Tempo[0].eg, RookOpenFile[1].mg, RookOpenFile[1].eg);
    printf("  passed pawn by rank (eg):");
    for (int r = 1; r <= 6; ++r)
        printf(" %d", PawnPassed[r].eg);
    printf("\n  king attackers (mg):");
    for (int i = 1; i <= 5; ++i)
        printf(" %d", KingAttackers[i].mg);
    printf("\n");
}

static int cmd_tune(int argc, char **argv) {
    const char *data = NULL, *out = "src/evalparams.c";
    int epochs     = 3000;
    double lr      = 1.0;
    double decay   = 5e-5;
    double valFrac = 0.05;
    int patience   = 250;
    size_t maxPos  = (size_t)-1;

    for (int i = 0; i < argc; ++i) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc)
            out = argv[++i];
        else if (!strcmp(argv[i], "-epochs") && i + 1 < argc)
            epochs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-lr") && i + 1 < argc)
            lr = atof(argv[++i]);
        else if (!strcmp(argv[i], "-decay") && i + 1 < argc)
            decay = atof(argv[++i]);
        else if (!strcmp(argv[i], "-val") && i + 1 < argc)
            valFrac = atof(argv[++i]);
        else if (!strcmp(argv[i], "-patience") && i + 1 < argc)
            patience = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-threads") && i + 1 < argc)
            nthreads_option = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-max") && i + 1 < argc)
            maxPos = (size_t)atoll(argv[++i]);
        else if (argv[i][0] != '-')
            data = argv[i];
    }

    if (!data)
        die("usage: tuner tune <data.epd> [-o out.c] [-epochs N] [-lr X] [-decay X]\n"
            "                  [-val FRACTION] [-patience N] [-threads N] [-max N]");

    load_dataset(data, maxPos);
    if (SampleCount < 1000)
        die("not enough positions to fit anything");

    Wmg = xmalloc(PARAM_NB * sizeof(double));
    Weg = xmalloc(PARAM_NB * sizeof(double));
    Mmg = xmalloc(PARAM_NB * sizeof(double));
    Meg = xmalloc(PARAM_NB * sizeof(double));
    Vmg = xmalloc(PARAM_NB * sizeof(double));
    Veg = xmalloc(PARAM_NB * sizeof(double));
    memset(Mmg, 0, PARAM_NB * sizeof(double));
    memset(Meg, 0, PARAM_NB * sizeof(double));
    memset(Vmg, 0, PARAM_NB * sizeof(double));
    memset(Veg, 0, PARAM_NB * sizeof(double));

    weights_from_tables();

    const size_t nval   = (size_t)(SampleCount * valFrac);
    const size_t ntrain = SampleCount - nval;
    const int nt        = worker_count();

    FitJob jobs[MAX_THREADS];
    ThreadHandle handles[MAX_THREADS];
    for (int i = 0; i < nt; ++i) {
        jobs[i].gmg = xmalloc(PARAM_NB * sizeof(double));
        jobs[i].geg = xmalloc(PARAM_NB * sizeof(double));
    }

    /*
     * Fit K first, on the starting weights, then leave it alone. K and the
     * overall scale of the weights are one degree of freedom between them, so
     * pinning K is what keeps the fitted evaluation on the same centipawn
     * scale the search's margins are written in.
     */
    printf("\nfitting the sigmoid scale...\n");
    double bestK = SigmoidK, bestKLoss = 1e30;
    for (double k = 60.0; k <= 600.0; k += 10.0) {
        SigmoidK = k;
        const double loss =
            run_jobs(0, ntrain < 200000 ? ntrain : 200000, false, jobs, handles, nt);
        if (loss < bestKLoss) {
            bestKLoss = loss;
            bestK     = k;
        }
    }
    SigmoidK = bestK;
    printf("  K = %.0f centipawns (loss %.6f)\n", SigmoidK, bestKLoss);

    double *bmg = xmalloc(PARAM_NB * sizeof(double));
    double *beg = xmalloc(PARAM_NB * sizeof(double));
    memcpy(bmg, Wmg, PARAM_NB * sizeof(double));
    memcpy(beg, Weg, PARAM_NB * sizeof(double));

    double bestVal    = nval ? run_jobs(ntrain, SampleCount, false, jobs, handles, nt)
                             : run_jobs(0, ntrain, false, jobs, handles, nt);
    const double val0 = bestVal;
    int sinceBest     = 0;

    printf("\ntraining: %llu positions (%llu held out), %d parameters, %d threads\n",
           (unsigned long long)ntrain, (unsigned long long)nval, PARAM_NB * 2, nt);
    printf("  epoch        train         validation\n");

    const double beta1 = 0.9, beta2 = 0.999, eps = 1e-8;
    const double t0 = now_seconds();

    for (int epoch = 1; epoch <= epochs; ++epoch) {
        const double trainLoss = run_jobs(0, ntrain, true, jobs, handles, nt);

        const double bc1 = 1.0 - pow(beta1, epoch);
        const double bc2 = 1.0 - pow(beta2, epoch);

        for (int i = 0; i < PARAM_NB; ++i) {
            double gmg = 0.0, geg = 0.0;
            for (int j = 0; j < nt; ++j) {
                gmg += jobs[j].gmg[i];
                geg += jobs[j].geg[i];
            }
            gmg /= (double)ntrain;
            geg /= (double)ntrain;

            Mmg[i] = beta1 * Mmg[i] + (1 - beta1) * gmg;
            Meg[i] = beta1 * Meg[i] + (1 - beta1) * geg;
            Vmg[i] = beta2 * Vmg[i] + (1 - beta2) * gmg * gmg;
            Veg[i] = beta2 * Veg[i] + (1 - beta2) * geg * geg;

            Wmg[i] -= lr * (Mmg[i] / bc1) / (sqrt(Vmg[i] / bc2) + eps);
            Weg[i] -= lr * (Meg[i] / bc1) / (sqrt(Veg[i] / bc2) + eps);

            /*
             * Weight decay, on the king-relative tables only.
             *
             * Those two tables are collinear with the base piece-square tables
             * by construction - anything they can say, the base tables can say
             * on average. Decay is what resolves that: it makes the base tables
             * carry the average and leaves the buckets holding only the
             * deviation from it, which is both the interpretation we want and
             * the one that generalises. The hand-written terms are left
             * undecayed because there is nothing for them to be shrunk towards.
             */
            if (decay > 0.0 && is_king_relative(i)) {
                Wmg[i] -= lr * decay * Wmg[i];
                Weg[i] -= lr * decay * Weg[i];
            }
        }

        if (epoch % 10 == 0 || epoch == 1) {
            const double valLoss =
                nval ? run_jobs(ntrain, SampleCount, false, jobs, handles, nt) : trainLoss;
            if (valLoss < bestVal) {
                bestVal   = valLoss;
                sinceBest = 0;
                memcpy(bmg, Wmg, PARAM_NB * sizeof(double));
                memcpy(beg, Weg, PARAM_NB * sizeof(double));
            } else {
                sinceBest += 10;
            }

            if (epoch % 100 == 0 || epoch == 1)
                printf("  %5d     %.7f      %.7f%s\n", epoch, trainLoss, valLoss,
                       sinceBest == 0 ? "  *" : "");
            fflush(stdout);

            if (sinceBest >= patience) {
                printf("  stopping: no validation improvement in %d epochs\n", patience);
                break;
            }
        }
    }

    printf("\nvalidation loss %.7f -> %.7f (%.2f%% lower) in %.1fs\n", val0, bestVal,
           100.0 * (val0 - bestVal) / val0, now_seconds() - t0);

    weights_to_tables(bmg, beg);

    /* Report the cost of rounding the fitted doubles back to int16. */
    weights_from_tables();
    const double rounded = nval ? run_jobs(ntrain, SampleCount, false, jobs, handles, nt)
                                : run_jobs(0, ntrain, false, jobs, handles, nt);
    printf("after rounding to int16: %.7f\n", rounded);

    report_notable();

    write_params(out);
    printf("\nwrote %s\n", out);
    return 0;
}

/* ========================================================================== */

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);

    bb_init();
    zobrist_init();
    eval_init();

    if (argc < 2) {
        fprintf(stderr, "usage:\n"
                        "  tuner extract <pgn>... -o <data.epd>   games -> labelled positions\n"
                        "  tuner tune <data.epd> [-o out.c]       positions -> fitted weights\n");
        return 1;
    }

    if (!strcmp(argv[1], "extract"))
        return cmd_extract(argc - 2, argv + 2);
    if (!strcmp(argv[1], "tune"))
        return cmd_tune(argc - 2, argv + 2);

    fprintf(stderr, "tuner: unknown subcommand '%s'\n", argv[1]);
    return 1;
}
