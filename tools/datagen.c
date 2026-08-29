/*
 * datagen.c - training data for the network. Not part of the engine binary.
 *
 * Seven subcommands, which together are the whole pipeline from an empty
 * directory to a shard a trainer can memory-map:
 *
 *     datagen selfplay -o shard%02d.cnn -games N   play games, label positions
 *     datagen label <in.epd>... -o out.cnn         label positions that exist
 *     datagen shuffle <in.cnn>... -o out.cnn       on-disk shuffle
 *     datagen filter <in.cnn>... -o out.cnn        keep/drop by source tag
 *     datagen verify <shard.cnn>...                the Task 1 acceptance gate
 *     datagen stats <shard.cnn>...                 histograms, to be eyeballed
 *     datagen dump <shard.cnn>                     records as text
 *
 * It links every engine source except main.c, exactly as tools/tuner.c does,
 * so it plays and searches with the engine in the working tree rather than
 * with a copy of it. docs/NNUE.md is why each choice below is the choice.
 *
 * WHAT A LABEL IS, and why it is the only thing in here that matters.
 *
 * One record is one position, one score and one game result. The score is a
 * fixed-NODE search - never fixed time, which would make labels depend on the
 * machine - run from a position that has been round-tripped through its own
 * FEN, with the transposition table and every history table cleared first.
 *
 * That last part is not fastidiousness. A search inherits the tables left by
 * the searches before it, so the score of a position reached mid-game is a
 * function of the game that reached it. Label the same position tomorrow from
 * a different game and the number differs, and the network spends its capacity
 * learning the difference. Clearing everything and re-parsing the FEN makes the
 * label a function of the position ALONE. That is what `datagen verify
 * -relabel` proves, and it is what makes the four position sources mixable at
 * all: a self-play position and a CCRL position mean the same thing only if
 * they were labelled the same way.
 *
 * WHY WORKERS ARE PROCESSES, NOT THREADS. search.c's state is file-scope and
 * the transposition table is one global allocation. Two searches in one process
 * would share both, and a shared table is exactly the contamination the
 * paragraph above exists to prevent. `-threads N` therefore forks (POSIX) or
 * re-launches (Windows) N processes, each writing its own shard. Shards
 * concatenate, so this costs nothing downstream.
 */
#if !defined(_WIN32)
/* fork/waitpid and the 64-bit file offsets below are hidden by -std=c17's
 * __STRICT_ANSI__ unless the POSIX level is asked for explicitly. */
#define _POSIX_C_SOURCE   200809L
#define _FILE_OFFSET_BITS 64
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif
#endif

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bitboard.h"
#include "board.h"
#include "eval.h"
#include "movegen.h"
#include "nnue.h"
#include "search.h"
#include "thread.h"
#include "timeman.h"
#include "tt.h"
#include "uci.h"
#include "zobrist.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <direct.h>
#include <windows.h>
#define fseek64(f, o, w) _fseeki64((f), (o), (w))
#define ftell64(f)       _ftelli64(f)
#define make_dir(p)      _mkdir(p)
#else
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#define fseek64(f, o, w) fseeko((f), (o), (w))
#define ftell64(f)       ftello(f)
#define make_dir(p)      mkdir((p), 0777)
#endif

#ifndef DATAGEN
#error "build with -DDATAGEN (use `make datagen`): the tree sampler needs the search hook"
#endif

/* Stamped in by the Makefile so a shard can name the engine that made it. A
 * dataset whose provenance is unknown cannot be compared to another one. */
#ifndef DATAGEN_COMMIT
#define DATAGEN_COMMIT "unknown"
#endif

enum { MAX_WORKERS = 64, PATH_CAP = 1024 };

static void die(const char *msg) {
    fprintf(stderr, "datagen: %s\n", msg);
    exit(1);
}

/* die(), for the messages that have to name the file and the numbers. A
 * failure that says which shard and how many bytes is a fix; "bad shard" is an
 * afternoon. */
#if defined(__GNUC__)
__attribute__((format(printf, 1, 2)))
#endif
static void
dief(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "datagen: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p)
        die("out of memory");
    return p;
}

static FILE *xfopen(const char *path, const char *mode) {
    FILE *f = fopen(path, mode);
    if (!f) {
        fprintf(stderr, "datagen: cannot open '%s' (mode %s): %s\n", path, mode, strerror(errno));
        exit(1);
    }
    return f;
}

/*
 * Creates the directory an output path lives in, and its parents.
 *
 * fopen(..., "wb") does not, and a run that fails on that fails once per
 * worker - fourteen identical lines, after the user has waited for the first
 * game to finish. Creating it is what every other tool in this repository's
 * pipeline already does by hand in the Makefile.
 *
 * A directory that already exists is not an error, which is also what makes
 * this safe against every worker racing to create the same one. Anything that
 * genuinely cannot be created is left for the fopen below to report, with the
 * errno that explains it.
 */
static void ensure_parent_dir(const char *path) {
    char dir[PATH_CAP];
    if (strlen(path) >= sizeof(dir))
        die("output path too long");
    strcpy(dir, path);

    char *cut = NULL;
    for (char *c = dir; *c; ++c)
        if (*c == '/' || *c == '\\')
            cut = c;
    if (!cut)
        return; /* writing into the current directory */
    *cut = '\0';

    /* Each component in turn, so a path several levels deep works. Starting at
     * dir + 1 steps over a leading separator and over a drive letter's colon,
     * neither of which is a directory to create. */
    for (char *c = dir + 1; *c; ++c) {
        if (*c != '/' && *c != '\\')
            continue;
        const char saved = *c;
        *c               = '\0';
        make_dir(dir);
        *c = saved;
    }
    make_dir(dir);
}

/* Wall clock. Deliberately not clock(), which is CPU time on POSIX. */
static double now_seconds(void) { return (double)time_ms() / 1000.0; }

static int iabs(int v) { return v < 0 ? -v : v; }

/* ========================================================================== *
 *  The record format
 *
 *  32 bytes, little-endian, no file header. The absent header is deliberate:
 *  shards have to concatenate with `cat`, and a header breaks that. Everything
 *  a header would have carried lives in the .json sidecar instead, and a shard
 *  whose sidecar is missing is a shard that cannot be trusted.
 *
 *      offset size  field
 *      0      8     occupied bitboard
 *      8      16    piece nibbles, one per occupied square, LSB-first
 *      24     1     bit 7 side to move; bits 0-6 en passant square (127 none)
 *      25     1     halfmove clock
 *      26     2     fullmove number
 *      28     2     score, centipawns, side-to-move relative (int16)
 *      30     1     WDL from the side to move: 0 loss, 1 draw, 2 win, 3 unknown
 *      31     1     flags: bits 0-2 source tag, bit 3 in check, 4-7 reserved
 *
 *  Castling rights ride in the nibbles rather than costing a byte: a piece code
 *  is `type | color << 3` with types 0-5, and a rook that still has a castling
 *  right is type 6. Three codes spare, and Chess960 castling encodes correctly
 *  for free - which matters, because a data format is expensive to change once
 *  a hundred million records exist.
 * ========================================================================== */

enum {
    REC_BYTES      = 32,
    POL_BYTES      = 4,
    REC_MAX_PIECES = 32,
    REC_EP_NONE    = 127,

    NIB_ROOK_CASTLE = 6, /* a rook that still carries a castling right */
    NIB_BLACK       = 8,

    REC_SRC_MASK = 7,
    REC_IN_CHECK = 8,

    WDL_LOSS    = 0,
    WDL_DRAW    = 1,
    WDL_WIN     = 2,
    WDL_UNKNOWN = 3,
    WDL_NB      = 4
};

/*
 * Flags bits 0-2: where the position came from. The mixture of sources is an
 * experiment rather than a decision, and the tag is what lets a trainer
 * re-weight it without regenerating anything.
 */
typedef enum {
    SRC_SELFPLAY = 0, /* a position the engine actually played into */
    SRC_TREE     = 1, /* an interior node of one of those searches */
    SRC_HUMAN    = 2, /* external/training/human.epd */
    SRC_ENGINE   = 3, /* external/training/ccrl_*.epd */
    SRC_BOOK     = 4, /* an opening book or randomised start position */
    SRC_OTHER    = 5,
    SRC_NB       = 6
} SourceTag;

static const char *const SourceNames[SRC_NB] = {"selfplay", "tree", "human",
                                                "engine",   "book", "other"};

static bool source_from_name(const char *name, SourceTag *out) {
    for (int i = 0; i < SRC_NB; ++i)
        if (!strcmp(name, SourceNames[i])) {
            *out = (SourceTag)i;
            return true;
        }
    return false;
}

typedef struct {
    Bitboard occupied;
    uint8_t nibbles[16];
    uint8_t stmEp;
    uint8_t halfmove;
    uint16_t fullmove;
    int16_t score;
    uint8_t wdl;
    uint8_t flags;
} Record;

/*
 * Policy sidecar: 4 bytes per record, in the same order, in `shardNN.pol`.
 * Optional to read, cheap to write, and impossible to backfill - which is why
 * it is written now for a Task 5 that has not started.
 *
 *   best   the best move of THIS record's label search. Always present.
 *   cutoff the move that caused the beta cutoff at the tree node this position
 *          was sampled from, or MOVE_NONE for a game-line record and for a
 *          node that failed low, where no move is a label.
 */
typedef struct {
    uint16_t best;
    uint16_t cutoff;
} PolicyRecord;

static void record_encode(const Record *r, uint8_t *out) {
    for (int i = 0; i < 8; ++i)
        out[i] = (uint8_t)(r->occupied >> (8 * i));
    memcpy(out + 8, r->nibbles, 16);
    out[24] = r->stmEp;
    out[25] = r->halfmove;
    out[26] = (uint8_t)r->fullmove;
    out[27] = (uint8_t)(r->fullmove >> 8);
    out[28] = (uint8_t)(uint16_t)r->score;
    out[29] = (uint8_t)((uint16_t)r->score >> 8);
    out[30] = r->wdl;
    out[31] = r->flags;
}

/* Two's complement spelled out rather than left to an implementation-defined
 * narrowing conversion: this format is on disk and outlives any one compiler. */
static int16_t from_le16_signed(uint16_t v) {
    return v < 0x8000u ? (int16_t)v : (int16_t)((int32_t)v - 65536);
}

static void record_decode(const uint8_t *in, Record *r) {
    r->occupied = 0;
    for (int i = 0; i < 8; ++i)
        r->occupied |= (Bitboard)in[i] << (8 * i);
    memcpy(r->nibbles, in + 8, 16);
    r->stmEp    = in[24];
    r->halfmove = in[25];
    r->fullmove = (uint16_t)(in[26] | (in[27] << 8));
    r->score    = from_le16_signed((uint16_t)(in[28] | (in[29] << 8)));
    r->wdl      = in[30];
    r->flags    = in[31];
}

static void policy_encode(const PolicyRecord *p, uint8_t *out) {
    out[0] = (uint8_t)p->best;
    out[1] = (uint8_t)(p->best >> 8);
    out[2] = (uint8_t)p->cutoff;
    out[3] = (uint8_t)(p->cutoff >> 8);
}

static void policy_decode(const uint8_t *in, PolicyRecord *p) {
    p->best   = (uint16_t)(in[0] | (in[1] << 8));
    p->cutoff = (uint16_t)(in[2] | (in[3] << 8));
}

static SourceTag record_source(const Record *r) { return (SourceTag)(r->flags & REC_SRC_MASK); }

/*
 * The rook a castling right refers to: the outermost one on the king's side of
 * the king. That is what KQkq means, it is what board_set_fen assumes, and it
 * is the X-FEN disambiguation rule, so it stays correct if Chess960 lands.
 */
static Square castling_rook(const Position *pos, Color c, bool kingside) {
    const Square ksq = king_square(pos, c);
    Bitboard rooks   = pieces_bb(pos, c, ROOK) & rank_bb(rank_of(ksq));
    Square best      = SQ_NONE;

    while (rooks) {
        const Square s = pop_lsb(&rooks);
        if (kingside ? file_of(s) > file_of(ksq) : file_of(s) < file_of(ksq)) {
            /* pop_lsb ascends, so the LAST match is the outermost kingside rook
             * and the FIRST is the outermost queenside one. */
            if (best == SQ_NONE || kingside)
                best = s;
        }
    }
    return best;
}

/* Packs `pos` plus its label. Every writer in this file goes through here, so
 * if the encoding is wrong it is wrong in exactly one place. */
static void record_from_position(Record *r, const Position *pos, int score, int wdl,
                                 SourceTag src) {
    memset(r, 0, sizeof(*r));

    Square castleRooks[4] = {SQ_NONE, SQ_NONE, SQ_NONE, SQ_NONE};
    int rookCount         = 0;
    if (pos->castling & WHITE_OO)
        castleRooks[rookCount++] = castling_rook(pos, WHITE, true);
    if (pos->castling & WHITE_OOO)
        castleRooks[rookCount++] = castling_rook(pos, WHITE, false);
    if (pos->castling & BLACK_OO)
        castleRooks[rookCount++] = castling_rook(pos, BLACK, true);
    if (pos->castling & BLACK_OOO)
        castleRooks[rookCount++] = castling_rook(pos, BLACK, false);

    r->occupied = occupied_bb(pos);

    Bitboard occ = r->occupied;
    int n        = 0;
    while (occ) {
        const Square s = pop_lsb(&occ);
        const Piece pc = piece_on(pos, s);

        unsigned code = (unsigned)type_of(pc) - 1; /* PAWN..KING -> 0..5 */
        if (type_of(pc) == ROOK)
            for (int i = 0; i < rookCount; ++i)
                if (castleRooks[i] == s)
                    code = NIB_ROOK_CASTLE;
        code |= (unsigned)color_of(pc) << 3;

        r->nibbles[n >> 1] |= (uint8_t)(code << ((n & 1) * 4));
        ++n;
    }

    const unsigned ep = pos->epSquare == SQ_NONE ? (unsigned)REC_EP_NONE : (unsigned)pos->epSquare;
    r->stmEp          = (uint8_t)(ep | (pos->sideToMove == BLACK ? 0x80u : 0u));
    r->halfmove       = (uint8_t)(pos->halfmoveClock < 255 ? pos->halfmoveClock : 255);
    r->fullmove       = (uint16_t)(pos->fullmoveNumber < 65535 ? pos->fullmoveNumber : 65535);

    const int clamped = score > 32000 ? 32000 : (score < -32000 ? -32000 : score);
    r->score          = (int16_t)clamped;
    r->wdl            = (uint8_t)wdl;
    r->flags          = (uint8_t)((unsigned)src & REC_SRC_MASK);
    if (board_checkers(pos))
        r->flags |= REC_IN_CHECK;
}

/*
 * Unpacks straight to a FEN.
 *
 * The scratch Position below is filled far enough for board_to_fen and NOT ONE
 * FIELD FURTHER - no key, no check info, no history - so it must never escape
 * this function. Going through board_to_fen rather than spelling out a FEN here
 * is the point: there is then exactly one FEN writer in the repository, and the
 * round-trip test in `verify` tests the format rather than a second
 * implementation of it.
 *
 * Returns false for a record that cannot be a position at all.
 */
static bool record_to_fen(const Record *r, char *fen) {
    Position p;
    memset(&p, 0, sizeof(p));

    if (popcount(r->occupied) > REC_MAX_PIECES)
        return false;

    Square rookSquares[4];
    int rookCount = 0;

    Bitboard occ = r->occupied;
    int n        = 0;
    while (occ) {
        const Square s      = pop_lsb(&occ);
        const unsigned code = (r->nibbles[n >> 1] >> ((n & 1) * 4)) & 0xF;
        ++n;

        const unsigned raw = code & 7;
        if (raw > NIB_ROOK_CASTLE)
            return false; /* codes 7 and 15 are unused */

        const Color c      = (code & NIB_BLACK) ? BLACK : WHITE;
        const PieceType pt = raw == NIB_ROOK_CASTLE ? ROOK : (PieceType)(raw + 1);
        board_put_piece(&p, make_piece(c, pt), s);

        if (raw == NIB_ROOK_CASTLE) {
            if (rookCount == 4)
                return false;
            rookSquares[rookCount++] = s;
        }
    }

    if (piece_count(&p, WHITE, KING) != 1 || piece_count(&p, BLACK, KING) != 1)
        return false;

    p.sideToMove = (r->stmEp & 0x80) ? BLACK : WHITE;

    const unsigned ep = r->stmEp & 0x7Fu;
    if (ep == REC_EP_NONE)
        p.epSquare = SQ_NONE;
    else if (ep < SQUARE_NB)
        p.epSquare = (Square)ep;
    else
        return false;

    for (int i = 0; i < rookCount; ++i) {
        const Color c       = color_of(piece_on(&p, rookSquares[i]));
        const bool kingside = file_of(rookSquares[i]) > file_of(king_square(&p, c));
        p.castling |= (CastlingRights)(c == WHITE ? (kingside ? WHITE_OO : WHITE_OOO)
                                                  : (kingside ? BLACK_OO : BLACK_OOO));
    }

    p.halfmoveClock  = r->halfmove;
    p.fullmoveNumber = r->fullmove;

    board_to_fen(&p, fen);
    return true;
}

/* ========================================================================== *
 *  Randomness
 *
 *  splitmix64: one multiply-xor chain, no state beyond a uint64, and good
 *  enough for uniform sampling. It is seeded from the command line and never
 *  from the clock, so a run is repeatable - which matters when a shard turns
 *  out to be wrong and the question is what produced it.
 * ========================================================================== */

typedef uint64_t Rng;

static uint64_t rng_next(Rng *s) {
    uint64_t z = (*s += 0x9E3779B97F4A7C15ULL);
    z          = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z          = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* Uniform on [0, n), rejecting the tail that modulo would over-represent. */
static uint64_t rng_below(Rng *s, uint64_t n) {
    if (n <= 1)
        return 0;
    const uint64_t limit = UINT64_MAX - (UINT64_MAX % n);
    uint64_t v;
    do {
        v = rng_next(s);
    } while (v >= limit);
    return v % n;
}

/* ========================================================================== *
 *  EPD input, and the opening book
 *
 *  Both subcommands read the same line format, so it is parsed in one place:
 *  `label` consumes a corpus of them, and `selfplay` draws start positions
 *  from one.
 * ========================================================================== */

/*
 * One line of a tuner-style EPD: a FEN, optionally followed by `[1.0]`,
 * `[0.5]` or `[0.0]` - the game result from WHITE's point of view, which is
 * what tools/tuner.c writes. A line without one still labels fine; its WDL is
 * simply unknown, and the trainer drops the game-result term for it.
 */
static bool parse_epd_line(char *line, char **fenOut, int *whiteResult) {
    char *bracket = strchr(line, '[');
    *whiteResult  = -1;

    if (bracket) {
        *whiteResult = (int)(strtod(bracket + 1, NULL) * 2.0 + 0.5); /* 0, 1 or 2 */
        if (*whiteResult < 0 || *whiteResult > 2)
            *whiteResult = -1;
        *bracket = '\0';
    }

    char *c = line;
    while (*c == ' ' || *c == '\t')
        ++c;
    size_t n = strlen(c);
    while (n && (c[n - 1] == ' ' || c[n - 1] == '\t' || c[n - 1] == '\r' || c[n - 1] == '\n'))
        c[--n] = '\0';

    *fenOut = c;
    return n > 0;
}

/*
 * An opening book is an EPD, so `tuner extract` writes one directly and the
 * usual test books need no conversion.
 *
 * Only the line offsets are held in memory. A deep book is a couple of hundred
 * MB of text, there is one worker PROCESS per core, and a game reads exactly
 * one line before spending seconds searching it - so holding the text would
 * cost gigabytes per box to save a seek nothing can measure.
 */
typedef struct {
    FILE *f;
    uint64_t *offset; /* start of every line that is not blank or a comment */
    size_t count;
} Book;

static void book_open(Book *b, const char *path) {
    memset(b, 0, sizeof(*b));
    b->f = xfopen(path, "rb");

    size_t cap = 1u << 16;
    b->offset  = xmalloc(cap * sizeof(uint64_t));

    char buf[1 << 16];
    uint64_t base  = 0;
    bool lineStart = true;
    size_t n;

    while ((n = fread(buf, 1, sizeof(buf), b->f)) > 0) {
        for (size_t i = 0; i < n; ++i) {
            const char c = buf[i];
            if (lineStart && c != '\n' && c != '\r' && c != '#') {
                if (b->count == cap) {
                    cap *= 2;
                    uint64_t *grown = realloc(b->offset, cap * sizeof(uint64_t));
                    if (!grown)
                        die("out of memory indexing the book");
                    b->offset = grown;
                }
                b->offset[b->count++] = base + i;
            }
            lineStart = c == '\n';
        }
        base += n;
    }

    if (ferror(b->f))
        dief("cannot read book '%s': %s", path, strerror(errno));
    if (!b->count)
        dief("book '%s' has no positions in it", path);
    /* fseek takes a long, which is 32-bit on Windows. Refuse a book that
     * cannot be seeked rather than silently drawing from its first 2 GB. */
    if (base > (uint64_t)LONG_MAX)
        dief("book '%s' is %llu bytes, past the %ld this build can seek", path,
             (unsigned long long)base, LONG_MAX);
}

/*
 * Entry `index` as a FEN. False for a line this build cannot use, which costs
 * the game that drew it and nothing else: a book is somebody else's file, and
 * one unreadable line in it is not a reason to throw away a fleet's work.
 */
static bool book_fen(Book *b, size_t index, char *out, size_t cap) {
    if (fseek(b->f, (long)b->offset[index], SEEK_SET) != 0)
        return false;

    char line[512];
    if (!fgets(line, sizeof(line), b->f))
        return false;

    char *fen;
    int ignored;
    if (!parse_epd_line(line, &fen, &ignored))
        return false;

    const size_t n = strlen(fen);
    if (n >= cap)
        return false;
    memcpy(out, fen, n + 1);
    return true;
}

static void book_close(Book *b) {
    if (b->f)
        fclose(b->f);
    free(b->offset);
    memset(b, 0, sizeof(*b));
}

/* ========================================================================== *
 *  Deduplication
 *
 *  Siblings inside a subtree differ by one piece, and a game line repeats
 *  structure for dozens of plies. Without a dedup pass a large fraction of a
 *  shard is the same position wearing a hat: training loss looks excellent and
 *  Elo does not move, which is the single hardest failure here to notice.
 *
 *  Open addressing over Zobrist keys. Key 0 marks an empty slot, so the one
 *  position whose key really is zero is remapped; the cost of that collision is
 *  one dropped record every few billion.
 * ========================================================================== */

typedef struct {
    uint64_t *slots;
    size_t mask;
    size_t used;
} KeySet;

static void keyset_init(KeySet *ks, int bits) {
    const size_t cap = (size_t)1 << bits;
    ks->slots        = xmalloc(cap * sizeof(uint64_t));
    memset(ks->slots, 0, cap * sizeof(uint64_t));
    ks->mask = cap - 1;
    ks->used = 0;
}

static void keyset_free(KeySet *ks) {
    free(ks->slots);
    ks->slots = NULL;
}

static void keyset_place(uint64_t *slots, size_t mask, uint64_t k) {
    size_t i = (size_t)(k >> 32) & mask;
    while (slots[i]) {
        if (slots[i] == k)
            return;
        i = (i + 1) & mask;
    }
    slots[i] = k;
}

static void keyset_grow(KeySet *ks) {
    const size_t oldCap = ks->mask + 1;
    const size_t newCap = oldCap * 2;
    uint64_t *slots     = xmalloc(newCap * sizeof(uint64_t));
    memset(slots, 0, newCap * sizeof(uint64_t));

    for (size_t i = 0; i < oldCap; ++i)
        if (ks->slots[i])
            keyset_place(slots, newCap - 1, ks->slots[i]);

    free(ks->slots);
    ks->slots = slots;
    ks->mask  = newCap - 1;
}

/* True if `k` was not already present. */
static bool keyset_insert(KeySet *ks, Key k) {
    uint64_t v = k ? (uint64_t)k : 1;

    if ((ks->used + 1) * 10 >= (ks->mask + 1) * 7)
        keyset_grow(ks);

    size_t i = (size_t)(v >> 32) & ks->mask;
    while (ks->slots[i]) {
        if (ks->slots[i] == v)
            return false;
        i = (i + 1) & ks->mask;
    }
    ks->slots[i] = v;
    ++ks->used;
    return true;
}

/* ========================================================================== *
 *  Paths and manifests
 * ========================================================================== */

/*
 * Substitutes a worker index into an output pattern. At most one `%d` or
 * `%0Nd` is accepted and the number is formatted here rather than by handing a
 * command-line string to snprintf as a format - a pattern with two conversions
 * would otherwise read an argument that was never passed.
 *
 * With no conversion and more than one worker, `%02d` is inserted before the
 * extension, so `shard.cnn` becomes `shard00.cnn` without the caller having to
 * remember the syntax.
 */
static void expand_path(char *dst, size_t cap, const char *pattern, int index, int workers) {
    const char *pct = NULL;
    for (const char *c = pattern; *c; ++c)
        if (*c == '%') {
            if (pct)
                die("output pattern has more than one % conversion");
            pct = c;
        }

    char pattern2[PATH_CAP];
    if (!pct && workers > 1) {
        /* Insert %02d before the extension of the last path component. */
        const char *slash = strrchr(pattern, '/');
        const char *back  = strrchr(pattern, '\\');
        if (back && (!slash || back > slash))
            slash = back;
        const char *dot = strrchr(pattern, '.');
        if (dot && (!slash || dot > slash)) {
            const size_t head = (size_t)(dot - pattern);
            if (head + 5 + strlen(dot) >= sizeof(pattern2))
                die("output path too long");
            memcpy(pattern2, pattern, head);
            memcpy(pattern2 + head, "%02d", 4);
            strcpy(pattern2 + head + 4, dot);
        } else {
            if (snprintf(pattern2, sizeof(pattern2), "%s%%02d", pattern) >= (int)sizeof(pattern2))
                die("output path too long");
        }
        pattern = pattern2;
        pct     = strchr(pattern, '%');
    }

    if (!pct) {
        if (strlen(pattern) >= cap)
            die("output path too long");
        strcpy(dst, pattern);
        return;
    }

    /* Parse %0<width>d, the only form allowed. */
    const char *c = pct + 1;
    int width     = 0;
    bool zeroPad  = false;
    if (*c == '0') {
        zeroPad = true;
        ++c;
    }
    while (*c >= '0' && *c <= '9')
        width = width * 10 + (*c++ - '0');
    if (*c != 'd')
        die("output pattern accepts only a %d or %0Nd conversion");

    char number[32];
    if (zeroPad)
        snprintf(number, sizeof(number), "%0*d", width, index);
    else
        snprintf(number, sizeof(number), "%*d", width, index);

    const size_t head = (size_t)(pct - pattern);
    const size_t tail = strlen(c + 1);
    if (head + strlen(number) + tail + 1 > cap)
        die("output path too long");

    memcpy(dst, pattern, head);
    strcpy(dst + head, number);
    strcpy(dst + head + strlen(number), c + 1);
}

/* `shard00.cnn` -> `shard00.pol`, `shard00.json`. */
static void replace_ext(char *dst, size_t cap, const char *path, const char *ext) {
    const char *slash = strrchr(path, '/');
    const char *back  = strrchr(path, '\\');
    if (back && (!slash || back > slash))
        slash = back;
    const char *dot = strrchr(path, '.');

    const size_t head = (dot && (!slash || dot > slash)) ? (size_t)(dot - path) : strlen(path);
    if (head + strlen(ext) + 1 > cap)
        die("path too long");

    memcpy(dst, path, head);
    strcpy(dst + head, ext);
}

static void json_string(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; ++s) {
        if (*s == '"' || *s == '\\')
            fprintf(f, "\\%c", *s);
        else if ((unsigned char)*s < 0x20)
            fprintf(f, "\\u%04x", (unsigned)(unsigned char)*s);
        else
            fputc(*s, f);
    }
    fputc('"', f);
}

/*
 * Everything a file header would have carried. NNUE.md: a shard whose manifest
 * is missing is a shard that cannot be trusted, and that is the correct
 * default - a node count, a loss curve or an Elo result that cannot be
 * attributed to a specific dataset is not a measurement.
 */
typedef struct {
    const char *command;
    const char *inputs; /* space-joined source files, or NULL */
    int nodes;
    int hashMb;
    uint64_t seed;
    bool dedup;
    /* selfplay only; ignored when `games` is zero */
    int games;
    const char *book;
    uint64_t bookEntries;
    int openingPlies;
    int openingMaxScore;
    int treeSamples;
    int treeMinDepth;
    int maxPlies;
    int winScore;
    int winPlies;
    int drawScore;
    int drawPlies;
    int drawMinMove;
    /* filters */
    int maxScore;
    bool quietFilter;
} Manifest;

static void manifest_write(const char *shardPath, const Manifest *m, uint64_t records,
                           const uint64_t *bySource, bool policy) {
    char path[PATH_CAP];
    replace_ext(path, sizeof(path), shardPath, ".json");

    FILE *f = xfopen(path, "wb");
    fprintf(f, "{\n");
    fprintf(f, "  \"format\": \"cnn-1\",\n");
    fprintf(f, "  \"record_bytes\": %d,\n", REC_BYTES);
    fprintf(f, "  \"records\": %llu,\n", (unsigned long long)records);
    fprintf(f, "  \"shard\": ");
    json_string(f, shardPath);
    fprintf(f, ",\n");
    fprintf(f, "  \"policy_sidecar\": ");
    if (policy) {
        char pol[PATH_CAP];
        replace_ext(pol, sizeof(pol), shardPath, ".pol");
        json_string(f, pol);
    } else {
        fprintf(f, "null");
    }
    fprintf(f, ",\n");
    fprintf(f, "  \"engine\": \"%s %s\",\n", ENGINE_NAME, ENGINE_VERSION);
    fprintf(f, "  \"commit\": \"%s\",\n", DATAGEN_COMMIT);
    fprintf(f, "  \"command\": ");
    json_string(f, m->command);
    fprintf(f, ",\n");
    if (m->inputs) {
        fprintf(f, "  \"inputs\": ");
        json_string(f, m->inputs);
        fprintf(f, ",\n");
    }
    fprintf(f, "  \"nodes\": %d,\n", m->nodes);
    fprintf(f, "  \"hash_mb\": %d,\n", m->hashMb);
    fprintf(f, "  \"seed\": %llu,\n", (unsigned long long)m->seed);
    fprintf(f, "  \"dedup\": %s,\n", m->dedup ? "true" : "false");
    fprintf(f, "  \"max_score\": %d,\n", m->maxScore);
    fprintf(f, "  \"quiet_filter\": %s,\n", m->quietFilter ? "true" : "false");

    if (m->games) {
        fprintf(f, "  \"selfplay\": {\n");
        fprintf(f, "    \"games\": %d,\n", m->games);
        /* Which book, and how many entries it indexed. The name alone does not
         * pin a dataset: a book that grew between two generations is a
         * different sampler wearing the same filename. */
        fprintf(f, "    \"book\": ");
        if (m->book)
            json_string(f, m->book);
        else
            fprintf(f, "null");
        fprintf(f, ",\n");
        fprintf(f, "    \"book_entries\": %llu,\n", (unsigned long long)m->bookEntries);
        fprintf(f, "    \"opening_plies\": %d,\n", m->openingPlies);
        fprintf(f, "    \"opening_max_score\": %d,\n", m->openingMaxScore);
        fprintf(f, "    \"tree_samples_per_move\": %d,\n", m->treeSamples);
        fprintf(f, "    \"tree_min_depth\": %d,\n", m->treeMinDepth);
        fprintf(f, "    \"max_plies\": %d,\n", m->maxPlies);
        fprintf(f, "    \"adjudicate_win_score\": %d,\n", m->winScore);
        fprintf(f, "    \"adjudicate_win_plies\": %d,\n", m->winPlies);
        fprintf(f, "    \"adjudicate_draw_score\": %d,\n", m->drawScore);
        fprintf(f, "    \"adjudicate_draw_plies\": %d,\n", m->drawPlies);
        fprintf(f, "    \"adjudicate_draw_from_move\": %d\n", m->drawMinMove);
        fprintf(f, "  },\n");
    }

    fprintf(f, "  \"sources\": {");
    bool first = true;
    for (int i = 0; i < SRC_NB; ++i) {
        if (!bySource[i])
            continue;
        fprintf(f, "%s\n    \"%s\": %llu", first ? "" : ",", SourceNames[i],
                (unsigned long long)bySource[i]);
        first = false;
    }
    fprintf(f, "%s}\n", first ? "" : "\n  ");
    fprintf(f, "}\n");
    fclose(f);
}

/*
 * Reads one integer field back out of a shard's manifest.
 *
 * Deliberately a scanner and not a parser: the fields `verify` needs are the
 * flat integers manifest_write emits one per line, and a JSON parser here
 * would be a dependency earned by two call sites. The pattern matched is
 * exactly what is written above - `"key": <int>` - so a nested key of the same
 * name is unreachable, which is the one ambiguity worth caring about.
 *
 * False when there is no manifest or no such field. That is not an error: a
 * shard from an older datagen, or one whose .json did not travel with it,
 * still verifies - just against the caller's defaults.
 */
static bool manifest_int(const char *shardPath, const char *key, int *out) {
    char path[PATH_CAP];
    replace_ext(path, sizeof(path), shardPath, ".json");

    FILE *f = fopen(path, "rb");
    if (!f)
        return false;

    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);

    char line[512];
    bool found = false;
    while (!found && fgets(line, sizeof(line), f)) {
        const char *at = strstr(line, pattern);
        if (!at)
            continue;
        at += strlen(pattern);
        char *end    = NULL;
        const long v = strtol(at, &end, 10);
        if (end != at && v > 0 && v <= INT_MAX) {
            *out  = (int)v;
            found = true;
        }
    }

    fclose(f);
    return found;
}

/* ========================================================================== *
 *  Shard writer
 * ========================================================================== */

typedef struct {
    FILE *rec;
    FILE *pol; /* NULL when the policy sidecar is switched off */
    char recPath[PATH_CAP];
    uint64_t count;
    uint64_t bySource[SRC_NB];
} ShardWriter;

static void shard_open(ShardWriter *w, const char *path, bool policy) {
    memset(w, 0, sizeof(*w));
    if (strlen(path) >= sizeof(w->recPath))
        die("output path too long");
    strcpy(w->recPath, path);

    ensure_parent_dir(path);
    w->rec = xfopen(path, "wb");
    if (policy) {
        char polPath[PATH_CAP];
        replace_ext(polPath, sizeof(polPath), path, ".pol");
        w->pol = xfopen(polPath, "wb");
    }
}

static void shard_write(ShardWriter *w, const Record *r, const PolicyRecord *p) {
    uint8_t buf[REC_BYTES];
    record_encode(r, buf);
    if (fwrite(buf, 1, REC_BYTES, w->rec) != REC_BYTES)
        die("short write on the shard");

    if (w->pol) {
        uint8_t pbuf[POL_BYTES];
        PolicyRecord empty = {0, 0};
        policy_encode(p ? p : &empty, pbuf);
        if (fwrite(pbuf, 1, POL_BYTES, w->pol) != POL_BYTES)
            die("short write on the policy sidecar");
    }

    ++w->count;
    ++w->bySource[record_source(r)];
}

static void shard_close(ShardWriter *w, const Manifest *m) {
    fclose(w->rec);
    if (w->pol)
        fclose(w->pol);
    manifest_write(w->recPath, m, w->count, w->bySource, w->pol != NULL);

    /* Unconditional, so that "a manifest exists" and "a checkpoint exists" can
     * never both be true: the first means the shard is finished, the second
     * that it is not. remove() on a shard that was never checkpointed fails
     * harmlessly. */
    char resumePath[PATH_CAP];
    replace_ext(resumePath, sizeof(resumePath), w->recPath, ".resume");
    remove(resumePath);
}

/* ========================================================================== *
 *  Resuming an interrupted run
 * ========================================================================== */

/*
 * Labelling 22.6M positions at 10,000 nodes is a five-hour run, and without
 * this a machine that reboots four hours in has produced nothing: shard_open()
 * opens with "wb", so the obvious response - run the same command again -
 * starts from the first line.
 *
 * The checkpoint is a file of its own rather than a field in the manifest, for
 * two reasons. The manifest is JSON, and reading it back would need a parser
 * this file does not have. And the manifest already MEANS something: it is
 * written once, at the end, and its presence is what makes a shard
 * trustworthy. A `.resume` file present means precisely the opposite, so the
 * two are never consulted together and never disagree.
 *
 * What makes this safe is that the checkpoint is a LOWER bound. The shard on
 * disk may hold more records than the checkpoint claims - the process died
 * between a write and the next checkpoint - so resuming TRUNCATES back to the
 * recorded count and re-labels from the recorded line. Records are never
 * appended after a point the checkpoint did not cover, which is what stops a
 * resumed shard from holding a torn record or a duplicated one.
 */

/* The most re-labelling an interruption can cost, against one flush and a
 * 64-byte write per interval. */
#define CHECKPOINT_SECONDS 30.0

#define RESUME_MAGIC   "CKRESUME"
#define RESUME_VERSION 1
#define RESUME_BYTES                                        \
    64 /* fixed, and written in one fwrite, so a checkpoint \
          is never half-updated */

#if defined(_WIN32)
#include <io.h>
#define truncate_file(f, n) _chsize_s(_fileno(f), (__int64)(n))
#else
#include <unistd.h>
#define truncate_file(f, n) ftruncate(fileno(f), (off_t)(n))
#endif

static bool file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    fclose(f);
    return true;
}

/* The checkpoint, or false when there is none. A checkpoint that exists and
 * cannot be read is fatal rather than ignored: carrying on would append to a
 * shard whose contents are unknown, and silently produce duplicates. */
static bool resume_read(const char *path, uint64_t *records, long *nextLine) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;

    char buf[RESUME_BYTES + 1];
    const size_t got = fread(buf, 1, RESUME_BYTES, f);
    fclose(f);
    buf[got] = '\0';

    char magic[16];
    int version;
    unsigned long long count;
    long line;
    if (got < 16 || sscanf(buf, "%15s %d %llu %ld", magic, &version, &count, &line) != 4 ||
        strcmp(magic, RESUME_MAGIC) != 0 || version != RESUME_VERSION)
        dief("%s is not a readable checkpoint. Delete it to start this shard over, or "
             "keep it and report it - it should never happen.",
             path);

    *records  = (uint64_t)count;
    *nextLine = line;
    return true;
}

static void resume_write(const char *path, uint64_t records, long nextLine) {
    char buf[RESUME_BYTES];
    memset(buf, ' ', sizeof(buf));
    const int n = snprintf(buf, sizeof(buf), "%s %d %llu %ld", RESUME_MAGIC, RESUME_VERSION,
                           (unsigned long long)records, nextLine);
    if (n < 0 || (size_t)n >= sizeof(buf))
        dief("checkpoint does not fit in %d bytes", RESUME_BYTES);
    buf[n]                = '\n';
    buf[RESUME_BYTES - 1] = '\n';

    FILE *f = xfopen(path, "wb");
    if (fwrite(buf, 1, sizeof(buf), f) != sizeof(buf))
        dief("short write on %s", path);
    fclose(f);
}

/*
 * Restores what the in-memory writer state would have been: the per-source
 * counts the manifest reports, and - when dedup is on - the key set.
 *
 * Rebuilding the key set rather than starting it empty is what makes a resumed
 * shard byte-identical to an uninterrupted one. Without it the second half of
 * the run cannot see the first half's positions, and the shard quietly holds
 * duplicates that no test looks for.
 */
static void shard_rescan(ShardWriter *w, KeySet *seen) {
    if (fseek64(w->rec, 0, SEEK_SET) != 0)
        dief("cannot rewind %s to rescan it", w->recPath);

    uint8_t buf[REC_BYTES];
    for (uint64_t i = 0; i < w->count; ++i) {
        if (fread(buf, 1, REC_BYTES, w->rec) != REC_BYTES)
            dief("%s ended after %llu of %llu records while rescanning", w->recPath,
                 (unsigned long long)i, (unsigned long long)w->count);

        Record r;
        record_decode(buf, &r);
        ++w->bySource[record_source(&r)];

        if (seen) {
            char fen[FEN_MAX_LEN];
            Position p;
            if (record_to_fen(&r, fen) && board_set_fen(&p, fen))
                keyset_insert(seen, p.key);
        }
    }
}

typedef enum {
    SHARD_FRESH,    /* nothing on disk; opened truncating */
    SHARD_RESUMED,  /* opened at a checkpoint, *resumeFrom is where to pick up */
    SHARD_COMPLETE, /* a finished shard is already there; nothing opened */
} ShardOpen;

/*
 * shard_open(), plus the three states a shard can already be in. `seen` may be
 * NULL when dedup is off.
 */
static ShardOpen shard_open_resumable(ShardWriter *w, const char *path, bool policy,
                                      long *resumeFrom, KeySet *seen) {
    memset(w, 0, sizeof(*w));
    if (strlen(path) >= sizeof(w->recPath))
        die("output path too long");
    strcpy(w->recPath, path);
    ensure_parent_dir(path);
    *resumeFrom = 0;

    char resumePath[PATH_CAP], manifestPath[PATH_CAP], polPath[PATH_CAP];
    replace_ext(resumePath, sizeof(resumePath), path, ".resume");
    replace_ext(manifestPath, sizeof(manifestPath), path, ".json");
    replace_ext(polPath, sizeof(polPath), path, ".pol");

    uint64_t records = 0;
    long nextLine    = 0;
    if (!resume_read(resumePath, &records, &nextLine)) {
        if (file_exists(manifestPath))
            return SHARD_COMPLETE;
        w->rec = xfopen(path, "wb");
        if (policy)
            w->pol = xfopen(polPath, "wb");
        return SHARD_FRESH;
    }

    /* "r+b", not "ab": the file has to be truncated back to the checkpoint
     * before anything is appended, and append mode cannot be positioned. */
    w->rec      = xfopen(path, "r+b");
    w->count    = records;
    *resumeFrom = nextLine;

    if (fseek64(w->rec, 0, SEEK_END) != 0)
        dief("cannot seek %s", path);
    const long long onDisk = ftell64(w->rec);
    const long long want   = (long long)records * REC_BYTES;
    if (onDisk < want)
        dief("%s holds %lld bytes but its checkpoint claims %llu records (%lld bytes). The "
             "shard and the checkpoint are not from the same run; delete both and start over.",
             path, onDisk, (unsigned long long)records, want);
    if (truncate_file(w->rec, want) != 0)
        dief("cannot truncate %s to %lld bytes", path, want);

    if (policy) {
        w->pol = xfopen(polPath, "r+b");
        if (fseek64(w->pol, 0, SEEK_END) != 0)
            dief("cannot seek %s", polPath);
        const long long polWant = (long long)records * POL_BYTES;
        if (ftell64(w->pol) < polWant)
            dief("%s is shorter than its %llu records need", polPath, (unsigned long long)records);
        if (truncate_file(w->pol, polWant) != 0)
            dief("cannot truncate %s to %lld bytes", polPath, polWant);
    }

    shard_rescan(w, seen);

    if (fseek64(w->rec, want, SEEK_SET) != 0)
        dief("cannot seek %s to its checkpoint", path);
    if (w->pol && fseek64(w->pol, (long long)records * POL_BYTES, SEEK_SET) != 0)
        dief("cannot seek %s to its checkpoint", polPath);

    return SHARD_RESUMED;
}

/* Makes everything written so far durable, then records how far the INPUT got.
 * The flush has to come first: a checkpoint ahead of the data it describes is
 * the one ordering that cannot be recovered from. */
static void shard_checkpoint(ShardWriter *w, long nextLine) {
    if (fflush(w->rec) != 0)
        dief("cannot flush %s", w->recPath);
    if (w->pol && fflush(w->pol) != 0)
        die("cannot flush the policy sidecar");

    char resumePath[PATH_CAP];
    replace_ext(resumePath, sizeof(resumePath), w->recPath, ".resume");
    resume_write(resumePath, w->count, nextLine);
}

/* ========================================================================== *
 *  Labelling - the one definition, used by every subcommand that writes
 * ========================================================================== */

/*
 * Scores `pos` with a fixed-node search from a cleared engine.
 *
 * `pos` MUST have come from board_set_fen. A position carried in from a game
 * brings its repetition history with it, and board_is_draw then scores it
 * differently from the same FEN parsed on its own - which would make the label
 * a function of the game rather than of the position, and `verify -relabel`
 * would (correctly) fail.
 */
static void label_position(const Position *pos, int nodes, SearchResult *out) {
    /* search_clear drops the transposition table, the history tables, the
     * killers and the continuation history: everything that carries from one
     * search to the next. */
    search_clear();

    SearchLimits limits;
    search_limits_clear(&limits);
    limits.nodes = (uint64_t)nodes;

    search_run_sync(pos, &limits, out);
}

/* Returns false for a FEN that will not parse, and for a terminal position -
 * a checkmate or stalemate has no score worth training on. */
static bool label_fen(const char *fen, int nodes, Position *pos, SearchResult *out) {
    if (!board_set_fen(pos, fen))
        return false;
    label_position(pos, nodes, out);
    return out->score != VALUE_NONE && is_ok_move(out->best);
}

/* The side-to-move field of a FEN, without parsing the rest of it. A Position
 * carries its whole game history and is expensive to build for one bit. */
static Color stm_from_fen(const char *fen) {
    const char *space = strchr(fen, ' ');
    return (space && space[1] == 'b') ? BLACK : WHITE;
}

/* Castling is encoded king-takes-own-rook, so its destination square is
 * occupied by our own rook and must not be read as a capture. */
static bool move_is_tactical(const Position *pos, Move m) {
    const MoveType mt = type_of_move(m);
    if (mt == MT_PROMOTION || mt == MT_EN_PASSANT)
        return true;
    return mt != MT_CASTLING && piece_on(pos, to_sq(m)) != NO_PIECE;
}

/* ========================================================================== *
 *  Worker processes
 * ========================================================================== */

static bool is_help(const char *arg) {
    return !strcmp(arg, "-h") || !strcmp(arg, "-help") || !strcmp(arg, "--help");
}

/* True if any argument asks for help, so a parser can answer before it starts
 * rejecting things. */
static bool wants_help(int argc, char **argv) {
    for (int i = 0; i < argc; ++i)
        if (is_help(argv[i]))
            return true;
    return false;
}

typedef int (*WorkerFn)(int index, int workers, void *ctx);

/* Set from `--worker N`: this process is a child and must do exactly one
 * worker's share rather than spawning any more. */
static int WorkerOverride = -1;
static bool Quiet         = false;

#if defined(_WIN32)
static int spawn_children(int workers) {
    HANDLE handles[MAX_WORKERS];
    char cmdline[8192];

    for (int i = 0; i < workers; ++i) {
        if (snprintf(cmdline, sizeof(cmdline), "%s --worker %d", GetCommandLineA(), i) >=
            (int)sizeof(cmdline))
            die("command line too long to re-launch a worker");

        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        memset(&si, 0, sizeof(si));
        memset(&pi, 0, sizeof(pi));
        si.cb = sizeof(si);

        /* Children inherit stdout, so their progress lines interleave with
         * ours; each is written with a single fprintf to keep them whole. */
        if (!CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
            fprintf(stderr, "datagen: CreateProcess failed for worker %d (error %lu)\n", i,
                    (unsigned long)GetLastError());
            for (int j = 0; j < i; ++j)
                CloseHandle(handles[j]);
            return 1;
        }
        CloseHandle(pi.hThread);
        handles[i] = pi.hProcess;
    }

    int rc = 0;
    for (int i = 0; i < workers; ++i) {
        WaitForSingleObject(handles[i], INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(handles[i], &code);
        if (code)
            rc = (int)code;
        CloseHandle(handles[i]);
    }
    return rc;
}
#endif

/*
 * Runs `fn` in `workers` separate processes and returns the worst exit code.
 *
 * POSIX forks and keeps going in the child; Windows has no fork, so the parent
 * re-launches its own command line with `--worker N` appended and the child
 * lands back here with WorkerOverride set.
 */
static int run_workers(int workers, WorkerFn fn, void *ctx) {
    if (WorkerOverride >= 0)
        return fn(WorkerOverride, workers, ctx);

    if (workers <= 1)
        return fn(0, 1, ctx);

#if defined(_WIN32)
    return spawn_children(workers);
#else
    pid_t pids[MAX_WORKERS];
    for (int i = 0; i < workers; ++i) {
        const pid_t pid = fork();
        if (pid < 0) {
            fprintf(stderr, "datagen: fork failed for worker %d\n", i);
            for (int j = 0; j < i; ++j)
                waitpid(pids[j], NULL, 0);
            return 1;
        }
        if (pid == 0) {
            /* The child inherits an initialised engine and its own copy of the
             * transposition table; nothing is shared, which is the point. */
            _exit(fn(i, workers, ctx) ? 1 : 0);
        }
        pids[i] = pid;
    }

    int rc = 0;
    for (int i = 0; i < workers; ++i) {
        int status = 0;
        waitpid(pids[i], &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status))
            rc = 1;
    }
    return rc;
#endif
}

/* ========================================================================== *
 *  Subcommand: selfplay
 * ========================================================================== */

typedef struct {
    const char *out;
    int games;
    int nodes;
    int hashMb;
    int threads;
    uint64_t seed;

    const char *book; /* EPD of start positions, or NULL for the start position */
    int openingPlies;
    int openingMaxScore;

    int treeSamples;  /* reservoir size per played move */
    int treeMinDepth; /* remaining depth a sampled node must still have */

    int maxPlies;
    int winScore, winPlies;
    int drawScore, drawPlies, drawMinMove;

    int maxScore;
    bool quietFilter;
    bool dedup;
    int dedupBits;
    bool policy;
} SelfplayOpts;

enum { MAX_RESERVOIR = 8 };

/*
 * Reservoir sampler over the interior of one search tree.
 *
 * An alpha-beta tree is overwhelmingly leaves, so any sampler that does not
 * stratify returns almost nothing but quiescence positions. Requiring a
 * minimum remaining depth is the stratification; Algorithm R over what is left
 * gives every eligible node the same chance without knowing how many there
 * will be.
 *
 * A candidate is materialised as a FEN only when it is actually selected -
 * about R * (1 + ln N) times per search rather than N - so the sampler costs
 * almost nothing in the node loop.
 */
typedef struct {
    Rng *rng;
    int minDepth;
    int want;
    int seen;
    int count;
    struct {
        char fen[FEN_MAX_LEN];
        Move cutoff;
    } slot[MAX_RESERVOIR];
} TreeSampler;

static void tree_visitor(const Position *pos, const SearchNodeInfo *info, void *ctx) {
    TreeSampler *ts = (TreeSampler *)ctx;

    /* Never inside quiescence (which never calls this), never in check, and
     * never from the leaf shell: those are not the positions a game-line
     * dataset is missing. */
    if (info->depth < ts->minDepth || info->inCheck)
        return;

    int slot;
    if (ts->count < ts->want) {
        slot = ts->count++;
        ++ts->seen;
    } else {
        ++ts->seen;
        const uint64_t j = rng_below(ts->rng, (uint64_t)ts->seen);
        if (j >= (uint64_t)ts->want)
            return;
        slot = (int)j;
    }

    board_to_fen(pos, ts->slot[slot].fen);
    /* A fail-low node proves only that nothing reached alpha, so its best move
     * is a hint rather than a label and no policy target exists for it. */
    ts->slot[slot].cutoff = info->kind == NODE_FAIL_LOW ? MOVE_NONE : info->best;
}

typedef struct {
    char fen[FEN_MAX_LEN];
    Color stm;
    SourceTag src;
    Move cutoff;
} Candidate;

typedef struct {
    Candidate *items;
    size_t count, cap;
} CandidateList;

static void candidates_push(CandidateList *cl, const char *fen, Color stm, SourceTag src,
                            Move cutoff) {
    if (cl->count == cl->cap) {
        cl->cap   = cl->cap ? cl->cap * 2 : 256;
        cl->items = realloc(cl->items, cl->cap * sizeof(Candidate));
        if (!cl->items)
            die("out of memory");
    }
    Candidate *c = &cl->items[cl->count++];
    snprintf(c->fen, sizeof(c->fen), "%s", fen);
    c->stm    = stm;
    c->src    = src;
    c->cutoff = cutoff;
}

/* Game result, always from white's point of view. */
typedef enum { RES_WHITE, RES_DRAW, RES_BLACK } GameResult;

static int wdl_for(GameResult r, Color stm) {
    if (r == RES_DRAW)
        return WDL_DRAW;
    const bool whiteWon = r == RES_WHITE;
    return (whiteWon == (stm == WHITE)) ? WDL_WIN : WDL_LOSS;
}

/*
 * Plays `plies` uniformly random legal moves from `pos` as it already stands.
 * Everything after this is deterministic - fixed nodes, one worker thread, no
 * randomisation in move choice - so these plies are the ONLY variation two
 * games from the same start position ever get.
 *
 * Which is why the count wants to be small when a book supplies the start: two
 * games sharing a book line and differing by a couple of random plies then play
 * out deterministically from near-identical positions, and the difference in
 * their results is attributable to the perturbation rather than to noise.
 */
static bool random_opening(Position *pos, int plies, Rng *rng) {
    for (int i = 0; i < plies; ++i) {
        ScoredMove list[MAX_MOVES];
        const int n = movegen_generate(pos, board_checkers(pos) ? GEN_EVASIONS : GEN_ALL, list);

        Move legal[MAX_MOVES];
        int legalCount = 0;
        for (int j = 0; j < n; ++j)
            if (movegen_is_legal(pos, list[j].m))
                legal[legalCount++] = list[j].m;

        if (legalCount == 0)
            return false;

        board_do_move(pos, legal[rng_below(rng, (uint64_t)legalCount)]);
    }

    return board_checkers(pos) == BB_EMPTY;
}

static int selfplay_worker(int index, int workers, void *ctx) {
    const SelfplayOpts *o = (const SelfplayOpts *)ctx;

    char path[PATH_CAP];
    expand_path(path, sizeof(path), o->out, index, workers);

    /* Each worker gets a share of the games and a stream of its own. The
     * stride keeps a run with N workers reproducible for any N. */
    const int games = o->games / workers + (index < o->games % workers ? 1 : 0);
    Rng rng         = o->seed + (uint64_t)index * 0x9E3779B97F4A7C15ULL;

    tt_resize((size_t)o->hashMb);

    Book book;
    memset(&book, 0, sizeof(book));
    if (o->book)
        book_open(&book, o->book);

    ShardWriter writer;
    shard_open(&writer, path, o->policy);

    KeySet seen;
    if (o->dedup)
        keyset_init(&seen, o->dedupBits);

    CandidateList cands = {NULL, 0, 0};
    Position pos;
    TreeSampler sampler;
    memset(&sampler, 0, sizeof(sampler));
    sampler.rng      = &rng;
    sampler.minDepth = o->treeMinDepth;
    sampler.want     = o->treeSamples < MAX_RESERVOIR ? o->treeSamples : MAX_RESERVOIR;

    const double start = now_seconds();
    uint64_t labelled  = 0;
    int rejected       = 0;
    double lastReport  = start;

    for (int game = 0; game < games; ++game) {
        int attempts = 0;
        GameResult result;
        cands.count = 0;

        for (;;) {
            /* Reached by a book line this build cannot set up, and by an
             * opening the search says is already decided. Both are normal a few
             * times; a hundred in a row means -openingscore is tighter than the
             * book is balanced, or the file is not a book at all. */
            if (++attempts > 100)
                dief("no playable opening in 100 attempts (%s, -openingscore %d)",
                     o->book ? o->book : "start position", o->openingMaxScore);

            if (o->book) {
                char bookFen[FEN_MAX_LEN];
                const uint64_t entry = rng_below(&rng, (uint64_t)book.count);
                if (!book_fen(&book, (size_t)entry, bookFen, sizeof(bookFen)) ||
                    !board_set_fen(&pos, bookFen))
                    continue;
            } else {
                board_set_startpos(&pos);
            }

            if (!random_opening(&pos, o->openingPlies, &rng))
                continue;

            /* A fresh set of tables per game, so a game cannot be steered by
             * the game before it. Inside the game the table stays warm: this
             * is play, not labelling. */
            search_clear();
            cands.count = 0;

            bool restart   = false;
            int winStreak  = 0;
            int lossStreak = 0;
            int drawStreak = 0;
            result         = RES_DRAW;

            for (;;) {
                ScoredMove list[MAX_MOVES];
                const int n =
                    movegen_generate(&pos, board_checkers(&pos) ? GEN_EVASIONS : GEN_ALL, list);
                int legalCount = 0;
                for (int j = 0; j < n; ++j)
                    if (movegen_is_legal(&pos, list[j].m))
                        ++legalCount;

                if (legalCount == 0) {
                    if (board_checkers(&pos))
                        result = pos.sideToMove == WHITE ? RES_BLACK : RES_WHITE;
                    break;
                }
                if (board_is_draw(&pos, 0) || pos.gamePly >= o->maxPlies)
                    break;

                sampler.seen  = 0;
                sampler.count = 0;
                if (sampler.want > 0)
                    search_set_node_visitor(tree_visitor, &sampler);

                SearchLimits limits;
                search_limits_clear(&limits);
                limits.nodes = (uint64_t)o->nodes;

                SearchResult res;
                search_run_sync(&pos, &limits, &res);
                search_set_node_visitor(NULL, NULL);

                if (!is_ok_move(res.best) || res.score == VALUE_NONE)
                    break;

                /* A randomised opening that is already decided teaches the net
                 * nothing about the middlegame, so the game is thrown away
                 * rather than played out. */
                if (pos.gamePly == o->openingPlies && iabs(res.score) > o->openingMaxScore) {
                    ++rejected;
                    restart = true;
                    break;
                }

                char fen[FEN_MAX_LEN];
                if (board_checkers(&pos) == BB_EMPTY) {
                    board_to_fen(&pos, fen);
                    candidates_push(&cands, fen, pos.sideToMove, SRC_SELFPLAY, MOVE_NONE);
                }
                for (int s = 0; s < sampler.count; ++s)
                    candidates_push(&cands, sampler.slot[s].fen, stm_from_fen(sampler.slot[s].fen),
                                    SRC_TREE, sampler.slot[s].cutoff);

                const int white = pos.sideToMove == WHITE ? res.score : -res.score;

                if (white >= o->winScore) {
                    ++winStreak;
                    lossStreak = 0;
                } else if (white <= -o->winScore) {
                    ++lossStreak;
                    winStreak = 0;
                } else {
                    winStreak = lossStreak = 0;
                }
                drawStreak = (pos.fullmoveNumber >= o->drawMinMove && iabs(white) <= o->drawScore)
                                 ? drawStreak + 1
                                 : 0;

                if (winStreak >= o->winPlies) {
                    result = RES_WHITE;
                    break;
                }
                if (lossStreak >= o->winPlies) {
                    result = RES_BLACK;
                    break;
                }
                if (drawStreak >= o->drawPlies) {
                    result = RES_DRAW;
                    break;
                }

                board_do_move(&pos, res.best);
            }

            if (!restart)
                break;
        }

        /*
         * Every candidate is re-searched from scratch, whether it came off the
         * game line or out of the tree. Identical label definitions across the
         * whole dataset are worth more than the searches this costs; see the
         * header.
         */
        for (size_t i = 0; i < cands.count; ++i) {
            const Candidate *c = &cands.items[i];

            Position p;
            if (!board_set_fen(&p, c->fen))
                continue;
            if (o->dedup && !keyset_insert(&seen, p.key))
                continue;

            SearchResult res;
            label_position(&p, o->nodes, &res);
            ++labelled;
            if (res.score == VALUE_NONE || !is_ok_move(res.best))
                continue;

            if (o->maxScore > 0 && iabs(res.score) > o->maxScore)
                continue;
            /* NNUE.md's quiet filter, applied to the game line only: the whole
             * point of a tree sample is that it is mid-tactic, and filtering
             * that out would leave the sampler generating game-line positions
             * by a slower route. Promotions join captures here - a promotion
             * moves as much material as one does. */
            if (o->quietFilter && c->src == SRC_SELFPLAY && move_is_tactical(&p, res.best))
                continue;

            Record rec;
            record_from_position(&rec, &p, res.score, wdl_for(result, c->stm), c->src);

            PolicyRecord pol;
            pol.best   = (uint16_t)res.best;
            pol.cutoff = (uint16_t)c->cutoff;
            shard_write(&writer, &rec, &pol);
        }

        const double now = now_seconds();
        if (!Quiet && (now - lastReport > 5.0 || game + 1 == games)) {
            const double elapsed = now - start > 0.001 ? now - start : 0.001;
            fprintf(stdout, "[w%02d] games %d/%d  records %llu  labels %llu  %.0f labels/s%s\n",
                    index, game + 1, games, (unsigned long long)writer.count,
                    (unsigned long long)labelled, (double)labelled / elapsed,
                    rejected ? "  (openings rejected)" : "");
            fflush(stdout);
            lastReport = now;
        }
    }

    Manifest man;
    memset(&man, 0, sizeof(man));
    man.command         = "selfplay";
    man.nodes           = o->nodes;
    man.hashMb          = o->hashMb;
    man.seed            = (uint64_t)(o->seed + (uint64_t)index * 0x9E3779B97F4A7C15ULL);
    man.dedup           = o->dedup;
    man.games           = games;
    man.book            = o->book;
    man.bookEntries     = (uint64_t)book.count;
    man.openingPlies    = o->openingPlies;
    man.openingMaxScore = o->openingMaxScore;
    man.treeSamples     = sampler.want;
    man.treeMinDepth    = o->treeMinDepth;
    man.maxPlies        = o->maxPlies;
    man.winScore        = o->winScore;
    man.winPlies        = o->winPlies;
    man.drawScore       = o->drawScore;
    man.drawPlies       = o->drawPlies;
    man.drawMinMove     = o->drawMinMove;
    man.maxScore        = o->maxScore;
    man.quietFilter     = o->quietFilter;
    shard_close(&writer, &man);

    if (o->dedup)
        keyset_free(&seen);
    free(cands.items);
    book_close(&book);

    if (!Quiet)
        fprintf(stdout, "[w%02d] wrote %llu records to %s\n", index,
                (unsigned long long)writer.count, path);
    return 0;
}

static void usage_selfplay(void) {
    printf("datagen selfplay -o <shard%%02d.cnn> [options]\n"
           "\n"
           "Plays games from book or randomised openings, samples positions from the\n"
           "game line AND from inside the search trees, then re-searches every one of\n"
           "them from a cleared engine to produce the label. Writes <shard>.cnn,\n"
           "<shard>.pol and <shard>.json.\n"
           "\n"
           "output and scale\n"
           "  -o <pattern>       output shard. %%02d is replaced by the worker index; with\n"
           "                     -threads > 1 and no conversion, %%02d is inserted before\n"
           "                     the extension.                          (required)\n"
           "  -games N           games IN TOTAL, split across workers.    (100)\n"
           "  -nodes N           fixed nodes per search. Never fixed time: time would\n"
           "                     make labels depend on the machine.       (10000)\n"
           "  -threads N         worker PROCESSES, each with its own private hash. (1)\n"
           "  -hash MB           hash per worker.                        (64)\n"
           "  -seed N            base RNG seed; worker k derives its own.  (1)\n"
           "\n"
           "tree sampling  (off by default - when on, it is most of the dataset)\n"
           "  -tree N            interior nodes reservoir-sampled per played move, 0 to\n"
           "                     switch tree sampling off entirely.       (0, max 8)\n"
           "  -treedepth N       remaining depth a node must still have to be eligible.\n"
           "                     Lower reaches the leaf shell, which is mostly\n"
           "                     quiescence positions.                    (4)\n"
           "\n"
           "openings\n"
           "  -book <file.epd>   draw each game's start position uniformly from this EPD\n"
           "                     (a FEN per line, anything after it ignored - what\n"
           "                     `tuner extract` writes). Default: the start position.\n"
           "  -opening N         random plies played before the game starts, and the only\n"
           "                     variation there is: everything after them is\n"
           "                     deterministic. Out of a book this is a perturbation and\n"
           "                     wants to be small.        (2 with -book, otherwise 8)\n"
           "  -openingscore N    discard the game if the opening is already decided by\n"
           "                     more than this many centipawns.          (800)\n"
           "\n"
           "adjudication  (endgame technique is not what we are training)\n"
           "  -winscore N        |score| that counts as won.              (2000)\n"
           "  -winplies N        consecutive plies it must hold.          (4)\n"
           "  -drawscore N       |score| that counts as drawn.            (0)\n"
           "  -drawplies N       consecutive plies it must hold.          (8)\n"
           "  -drawmove N        first full move the draw rule applies from. (60)\n"
           "  -maxplies N        hard cap on game length.                 (400)\n"
           "\n"
           "filters\n"
           "  -maxscore N        drop records whose label exceeds this; 0 disables. (2000)\n"
           "  -noquiet           keep game-line positions whose best move is a capture\n"
           "                     or promotion. Tree samples are never quiet-filtered.\n"
           "  -nodedup           keep positions already seen in this shard.\n"
           "  -dedupbits N       initial log2 size of the dedup table; it grows. (22)\n"
           "  -nopolicy          do not write the .pol sidecar.\n"
           "  -quiet             progress lines off.\n");
}

static int cmd_selfplay(int argc, char **argv) {
    if (wants_help(argc, argv)) {
        usage_selfplay();
        return 0;
    }

    SelfplayOpts o;
    memset(&o, 0, sizeof(o));
    o.out     = NULL;
    o.games   = 100;
    o.nodes   = 10000;
    o.hashMb  = 64;
    o.threads = 1;
    o.seed    = 1;
    o.book    = NULL;
    /* Resolved below, once it is known whether a book was given: 8 random plies
     * are how a game from the start position gets anywhere at all, and are far
     * too many to play on top of a book line that was chosen for its depth. */
    o.openingPlies    = -1;
    o.openingMaxScore = 800;
    o.treeSamples     = 0;
    o.treeMinDepth    = 4;
    o.maxPlies        = 400;
    o.winScore        = 2000;
    o.winPlies        = 4;
    o.drawScore       = 0;
    o.drawPlies       = 8;
    o.drawMinMove     = 60;
    o.maxScore        = 2000;
    o.quietFilter     = true;
    o.dedup           = true;
    o.dedupBits       = 22;
    o.policy          = true;

    for (int i = 0; i < argc; ++i) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc)
            o.out = argv[++i];
        else if (!strcmp(argv[i], "-games") && i + 1 < argc)
            o.games = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-nodes") && i + 1 < argc)
            o.nodes = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-hash") && i + 1 < argc)
            o.hashMb = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-threads") && i + 1 < argc)
            o.threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-seed") && i + 1 < argc)
            o.seed = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "-book") && i + 1 < argc)
            o.book = argv[++i];
        else if (!strcmp(argv[i], "-opening") && i + 1 < argc)
            o.openingPlies = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-openingscore") && i + 1 < argc)
            o.openingMaxScore = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-tree") && i + 1 < argc)
            o.treeSamples = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-treedepth") && i + 1 < argc)
            o.treeMinDepth = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-maxplies") && i + 1 < argc)
            o.maxPlies = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-winscore") && i + 1 < argc)
            o.winScore = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-winplies") && i + 1 < argc)
            o.winPlies = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-drawscore") && i + 1 < argc)
            o.drawScore = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-drawplies") && i + 1 < argc)
            o.drawPlies = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-drawmove") && i + 1 < argc)
            o.drawMinMove = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-maxscore") && i + 1 < argc)
            o.maxScore = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-dedupbits") && i + 1 < argc)
            o.dedupBits = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-noquiet"))
            o.quietFilter = false;
        else if (!strcmp(argv[i], "-nodedup"))
            o.dedup = false;
        else if (!strcmp(argv[i], "-nopolicy"))
            o.policy = false;
        else if (!strcmp(argv[i], "-quiet"))
            Quiet = true;
        else {
            fprintf(stderr, "datagen: unknown selfplay option '%s'\n", argv[i]);
            return 1;
        }
    }

    if (!o.out)
        die("selfplay needs -o <shard%02d.cnn>");
    if (o.games < 1 || o.nodes < 1)
        die("selfplay needs -games and -nodes above zero");
    if (o.threads < 1 || o.threads > MAX_WORKERS)
        die("-threads must be between 1 and 64");
    if (o.dedupBits < 10 || o.dedupBits > 30)
        die("-dedupbits must be between 10 and 30");
    if (o.openingPlies < 0)
        o.openingPlies = o.book ? 2 : 8;
    /* Every worker opens the book for itself, so an unreadable one is N
     * identical deaths several seconds in. Say it once, here, first. */
    if (o.book) {
        FILE *probe = xfopen(o.book, "rb");
        fclose(probe);
    }

    /* Once, here, rather than N times inside N workers. */
    char probe[PATH_CAP];
    expand_path(probe, sizeof(probe), o.out, 0, o.threads);
    ensure_parent_dir(probe);

    return run_workers(o.threads, selfplay_worker, &o);
}

/* ========================================================================== *
 *  Subcommand: label
 * ========================================================================== */

typedef struct {
    const char *out;
    char **inputs;
    int inputCount;
    int nodes;
    int hashMb;
    int threads;
    int stride;
    /* PER WORKER, not in total: with -threads 4 -max 1000 each of the four
     * shards holds 1000 records. Capping the total instead would need the
     * workers to agree on a count, and they are separate processes precisely
     * so that they share nothing. */
    long max;
    long skip;
    SourceTag source;
    int maxScore;
    bool quietFilter;
    bool dedup;
    int dedupBits;
    bool policy;
    bool resume;
} LabelOpts;

static int label_worker(int index, int workers, void *ctx) {
    const LabelOpts *o = (const LabelOpts *)ctx;

    char path[PATH_CAP];
    expand_path(path, sizeof(path), o->out, index, workers);

    tt_resize((size_t)o->hashMb);

    /* The key set comes first: a resumed shard replays its own records into it,
     * so that the second half of a run can see the first half's positions. */
    KeySet seen;
    if (o->dedup)
        keyset_init(&seen, o->dedupBits);

    ShardWriter writer;
    long resumeFrom = 0;

    if (o->resume) {
        const ShardOpen state =
            shard_open_resumable(&writer, path, o->policy, &resumeFrom, o->dedup ? &seen : NULL);
        if (state == SHARD_COMPLETE) {
            if (!Quiet)
                fprintf(stdout, "[w%02d] %s is already complete\n", index, path);
            if (o->dedup)
                keyset_free(&seen);
            return 0;
        }
        if (state == SHARD_RESUMED && !Quiet)
            fprintf(stdout, "[w%02d] resuming %s at %llu records, line %ld\n", index, path,
                    (unsigned long long)writer.count, resumeFrom);
    } else {
        shard_open(&writer, path, o->policy);
    }

    const double start    = now_seconds();
    double lastReport     = start;
    double lastCheckpoint = start;
    long eligible         = 0; /* lines that passed -skip and -stride, all workers */
    uint64_t labelled     = 0;

    char line[512];
    for (int f = 0; f < o->inputCount && (o->max <= 0 || (long)writer.count < o->max); ++f) {
        FILE *in = xfopen(o->inputs[f], "rb");

        while (fgets(line, sizeof(line), in)) {
            if (o->max > 0 && (long)writer.count >= o->max)
                break;

            char *fen;
            int whiteResult;
            if (!parse_epd_line(line, &fen, &whiteResult))
                continue;

            const long ordinal = eligible++;
            if (ordinal < o->skip)
                continue;
            if (o->stride > 1 && (ordinal - o->skip) % o->stride)
                continue;
            /* Workers split the file by line rather than by offset, so any
             * worker count reads the same lines in the same order. */
            if (((ordinal - o->skip) / (o->stride > 1 ? o->stride : 1)) % workers != index)
                continue;

            /* Applied AFTER the worker split, never before: the split is
             * `(ordinal - skip) / stride % workers`, so anything that shifted
             * the ordinals would hand this worker a different set of lines
             * than the run being resumed gave it. */
            if (ordinal < resumeFrom)
                continue;

            /* This line is ours and not yet done, so everything before it is.
             * Checkpointing here rather than after a successful write also
             * covers a long run of lines the filters all rejected - the
             * filters run AFTER the search, so replaying them is not free. */
            if (o->resume) {
                const double now = now_seconds();
                if (now - lastCheckpoint > CHECKPOINT_SECONDS) {
                    shard_checkpoint(&writer, ordinal);
                    lastCheckpoint = now;
                }
            }

            Position p;
            SearchResult res;
            if (!label_fen(fen, o->nodes, &p, &res))
                continue;
            ++labelled;

            if (o->dedup && !keyset_insert(&seen, p.key))
                continue;
            if (o->maxScore > 0 && iabs(res.score) > o->maxScore)
                continue;
            if (o->quietFilter && move_is_tactical(&p, res.best))
                continue;

            int wdl = WDL_UNKNOWN;
            if (whiteResult >= 0) {
                const GameResult g =
                    whiteResult == 2 ? RES_WHITE : (whiteResult == 0 ? RES_BLACK : RES_DRAW);
                wdl = wdl_for(g, p.sideToMove);
            }

            Record rec;
            record_from_position(&rec, &p, res.score, wdl, o->source);

            PolicyRecord pol;
            pol.best   = (uint16_t)res.best;
            pol.cutoff = MOVE_NONE;
            shard_write(&writer, &rec, &pol);

            const double now = now_seconds();
            if (!Quiet && now - lastReport > 5.0) {
                const double elapsed = now - start > 0.001 ? now - start : 0.001;
                fprintf(stdout, "[w%02d] records %llu  labels %llu  %.0f labels/s\n", index,
                        (unsigned long long)writer.count, (unsigned long long)labelled,
                        (double)labelled / elapsed);
                fflush(stdout);
                lastReport = now;
            }
        }
        fclose(in);
    }

    char joined[PATH_CAP];
    joined[0] = '\0';
    for (int f = 0; f < o->inputCount; ++f) {
        if (strlen(joined) + strlen(o->inputs[f]) + 2 >= sizeof(joined))
            break;
        if (joined[0])
            strcat(joined, " ");
        strcat(joined, o->inputs[f]);
    }

    Manifest man;
    memset(&man, 0, sizeof(man));
    man.command     = "label";
    man.inputs      = joined;
    man.nodes       = o->nodes;
    man.hashMb      = o->hashMb;
    man.dedup       = o->dedup;
    man.maxScore    = o->maxScore;
    man.quietFilter = o->quietFilter;
    shard_close(&writer, &man);

    if (o->dedup)
        keyset_free(&seen);

    if (!Quiet)
        fprintf(stdout, "[w%02d] wrote %llu records to %s\n", index,
                (unsigned long long)writer.count, path);
    return 0;
}

static void usage_label(void) {
    printf("datagen label <in.epd>... -o <shard.cnn> [options]\n"
           "\n"
           "Labels positions that already exist. Each line is a FEN, optionally followed\n"
           "by [1.0] / [0.5] / [0.0] - the game result from WHITE's point of view, which\n"
           "is what `tuner extract` writes. A line without one still labels; its WDL is\n"
           "recorded as unknown and the trainer drops the game-result term for it.\n"
           "\n"
           "  -o <pattern>       output shard, as for selfplay.           (required)\n"
           "  -source <name>     source tag: selfplay tree human engine book other.\n"
           "                     Set it - the mixture is an experiment, and the tag is\n"
           "                     what lets a trainer re-weight without regenerating.\n"
           "                                                              (other)\n"
           "  -nodes N           fixed nodes per search.                  (10000)\n"
           "  -threads N         worker processes; they split the file by line, so any\n"
           "                     worker count reads the same lines.       (1)\n"
           "  -hash MB           hash per worker.                         (8)\n"
           "  -max N             stop after N records PER WORKER, not in total. (all)\n"
           "  -skip N            skip the first N eligible lines.         (0)\n"
           "  -stride N          keep one line in N.                      (1)\n"
           "  -maxscore N        drop records whose label exceeds this; 0 disables. (0)\n"
           "  -quiet-filter      drop positions whose best move is a capture or\n"
           "                     promotion. Off by default: the extractor already\n"
           "                     filtered the EPDs in external/training.\n"
           "  -nodedup           keep positions already seen in this shard.\n"
           "  -dedupbits N       initial log2 size of the dedup table.    (22)\n"
           "  -nopolicy          do not write the .pol sidecar.\n"
           "  -resume            checkpoint every 30s, and pick up where an interrupted\n"
           "                     run of the SAME command stopped. Without it a rerun\n"
           "                     truncates the shard and starts from the first line,\n"
           "                     which on a five-hour labelling run is the whole run.\n"
           "                     A shard that already finished is left alone, so the\n"
           "                     command is safe to repeat until it says complete.\n"
           "                     Keep -threads the same: workers split the input by\n"
           "                     line, so a different count reads different lines.\n"
           "  -quiet             progress lines off.\n");
}

static int cmd_label(int argc, char **argv) {
    if (wants_help(argc, argv)) {
        usage_label();
        return 0;
    }

    LabelOpts o;
    memset(&o, 0, sizeof(o));
    o.nodes       = 10000;
    o.hashMb      = 8;
    o.threads     = 1;
    o.stride      = 1;
    o.source      = SRC_OTHER;
    o.maxScore    = 0; /* the EPDs were already filtered by the extractor */
    o.quietFilter = false;
    o.dedup       = true;
    o.dedupBits   = 22;
    o.policy      = true;

    o.inputs = (char **)xmalloc((size_t)argc * sizeof(char *));

    for (int i = 0; i < argc; ++i) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc)
            o.out = argv[++i];
        else if (!strcmp(argv[i], "-nodes") && i + 1 < argc)
            o.nodes = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-hash") && i + 1 < argc)
            o.hashMb = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-threads") && i + 1 < argc)
            o.threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-stride") && i + 1 < argc)
            o.stride = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-max") && i + 1 < argc)
            o.max = atol(argv[++i]);
        else if (!strcmp(argv[i], "-skip") && i + 1 < argc)
            o.skip = atol(argv[++i]);
        else if (!strcmp(argv[i], "-maxscore") && i + 1 < argc)
            o.maxScore = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-dedupbits") && i + 1 < argc)
            o.dedupBits = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-source") && i + 1 < argc) {
            if (!source_from_name(argv[++i], &o.source))
                die("-source must be one of: selfplay tree human engine book other");
        } else if (!strcmp(argv[i], "-quiet-filter"))
            o.quietFilter = true;
        else if (!strcmp(argv[i], "-nodedup"))
            o.dedup = false;
        else if (!strcmp(argv[i], "-nopolicy"))
            o.policy = false;
        else if (!strcmp(argv[i], "-resume"))
            o.resume = true;
        else if (!strcmp(argv[i], "-quiet"))
            Quiet = true;
        else if (argv[i][0] == '-') {
            fprintf(stderr, "datagen: unknown label option '%s'\n", argv[i]);
            return 1;
        } else
            o.inputs[o.inputCount++] = argv[i];
    }

    if (!o.out)
        die("label needs -o <shard.cnn>");
    if (o.inputCount == 0)
        die("label needs at least one input .epd");
    if (o.threads < 1 || o.threads > MAX_WORKERS)
        die("-threads must be between 1 and 64");
    if (o.stride < 1)
        die("-stride must be at least 1");
    if (o.nodes < 1)
        die("-nodes must be at least 1");

    char probe[PATH_CAP];
    expand_path(probe, sizeof(probe), o.out, 0, o.threads);
    ensure_parent_dir(probe);

    const int rc = run_workers(o.threads, label_worker, &o);
    free(o.inputs);
    return rc;
}

/* ========================================================================== *
 *  Subcommand: shuffle
 *
 *  Positions from one game are correlated, and a training batch drawn from one
 *  region of the file is a batch of near-duplicates. A DataLoader shuffle
 *  buffer does not fix that: a 100k-record window over a file whose
 *  consecutive records come from the same game is not a shuffle.
 *
 *  Two passes. Scatter every record to a random bucket, then shuffle each
 *  bucket in memory and concatenate. Bucket count is chosen so a bucket fits in
 *  RAM; after this the trainer can read sequentially forever.
 * ========================================================================== */

static void usage_shuffle(void) {
    printf("datagen shuffle <in.cnn>... -o <out.cnn> [options]\n"
           "\n"
           "Two passes: scatter every record to a random bucket, then shuffle each\n"
           "bucket in memory and concatenate. Do this before training. A DataLoader\n"
           "shuffle buffer is not a substitute - a 100k-record window over a file whose\n"
           "consecutive records come from the same game is not a shuffle.\n"
           "\n"
           "Policy sidecars ride along: a record and its four policy bytes move as one\n"
           "unit. Either every input has a .pol or none may.\n"
           "\n"
           "  -o <path>          output shard.                            (required)\n"
           "  -buckets K         bucket count; by default chosen so one bucket fits in\n"
           "                     -memory.                                 (auto, max 256)\n"
           "  -memory MB         how much RAM one bucket may take.        (512)\n"
           "  -tmp <dir>         where bucket files go.                   (beside -o)\n"
           "  -seed N            RNG seed.                                (1)\n"
           "  -quiet             progress lines off.\n");
}

static int cmd_shuffle(int argc, char **argv) {
    if (wants_help(argc, argv)) {
        usage_shuffle();
        return 0;
    }

    const char *out = NULL;
    const char *tmp = NULL;
    char *inputs[256];
    int inputCount   = 0;
    int buckets      = 0;
    uint64_t seed    = 1;
    size_t bucketCap = 512u << 20; /* bytes of one in-memory bucket */

    for (int i = 0; i < argc; ++i) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc)
            out = argv[++i];
        else if (!strcmp(argv[i], "-buckets") && i + 1 < argc)
            buckets = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-seed") && i + 1 < argc)
            seed = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "-tmp") && i + 1 < argc)
            tmp = argv[++i];
        else if (!strcmp(argv[i], "-memory") && i + 1 < argc)
            bucketCap = (size_t)atol(argv[++i]) << 20;
        else if (!strcmp(argv[i], "-quiet"))
            Quiet = true;
        else if (argv[i][0] == '-') {
            fprintf(stderr, "datagen: unknown shuffle option '%s'\n", argv[i]);
            return 1;
        } else if (inputCount < (int)(sizeof(inputs) / sizeof(inputs[0])))
            inputs[inputCount++] = argv[i];
        else
            die("too many input shards");
    }

    if (!out || inputCount == 0)
        die("usage: datagen shuffle <in.cnn>... -o <out.cnn> [-buckets K] [-seed S]");

    /*
     * The policy sidecar has to survive the permutation, so a record and its
     * four policy bytes travel as one unit. All inputs must agree about whether
     * a sidecar exists - a half-populated .pol would silently misalign every
     * record after the join.
     */
    bool policy = true;
    for (int i = 0; i < inputCount; ++i) {
        char pol[PATH_CAP];
        replace_ext(pol, sizeof(pol), inputs[i], ".pol");
        FILE *f = fopen(pol, "rb");
        if (f)
            fclose(f);
        else
            policy = false;
    }

    const size_t unit = REC_BYTES + (policy ? POL_BYTES : 0);

    uint64_t total = 0;
    for (int i = 0; i < inputCount; ++i) {
        FILE *f = xfopen(inputs[i], "rb");
        fseek64(f, 0, SEEK_END);
        const long long size = (long long)ftell64(f);
        fclose(f);
        if (size % REC_BYTES)
            die("input shard size is not a multiple of the record size");
        total += (uint64_t)(size / REC_BYTES);
    }

    if (total == 0)
        die("nothing to shuffle");

    if (buckets <= 0) {
        buckets = (int)((total * unit + bucketCap - 1) / bucketCap);
        if (buckets < 1)
            buckets = 1;
        if (buckets > 256)
            buckets = 256;
    }

    char(*bucketPath)[PATH_CAP] = xmalloc((size_t)buckets * PATH_CAP);
    FILE *bucketFile[256];
    uint64_t bucketCount[256];

    ensure_parent_dir(out);

    for (int b = 0; b < buckets; ++b) {
        if (tmp)
            snprintf(bucketPath[b], PATH_CAP, "%s/datagen-bucket%03d.tmp", tmp, b);
        else
            snprintf(bucketPath[b], PATH_CAP, "%s.bucket%03d.tmp", out, b);
        if (b == 0)
            ensure_parent_dir(bucketPath[b]);
        bucketFile[b]  = xfopen(bucketPath[b], "wb");
        bucketCount[b] = 0;
    }

    Rng rng = seed;

    if (!Quiet)
        fprintf(stdout, "pass 1: scattering %llu records into %d buckets%s\n",
                (unsigned long long)total, buckets, policy ? " (with policy sidecar)" : "");

    uint64_t scattered = 0;
    for (int i = 0; i < inputCount; ++i) {
        FILE *rf = xfopen(inputs[i], "rb");
        FILE *pf = NULL;
        if (policy) {
            char pol[PATH_CAP];
            replace_ext(pol, sizeof(pol), inputs[i], ".pol");
            pf = xfopen(pol, "rb");
        }

        uint8_t unitBuf[REC_BYTES + POL_BYTES];
        while (fread(unitBuf, 1, REC_BYTES, rf) == REC_BYTES) {
            if (pf && fread(unitBuf + REC_BYTES, 1, POL_BYTES, pf) != POL_BYTES)
                die("policy sidecar is shorter than its shard");

            const int b = (int)rng_below(&rng, (uint64_t)buckets);
            if (fwrite(unitBuf, 1, unit, bucketFile[b]) != unit)
                die("short write to a bucket file");
            ++bucketCount[b];
            ++scattered;
        }

        fclose(rf);
        if (pf)
            fclose(pf);
    }

    for (int b = 0; b < buckets; ++b)
        fclose(bucketFile[b]);

    if (scattered != total)
        die("scattered a different number of records than the inputs contain");

    char outPol[PATH_CAP];
    replace_ext(outPol, sizeof(outPol), out, ".pol");
    FILE *of  = xfopen(out, "wb");
    FILE *opf = policy ? xfopen(outPol, "wb") : NULL;

    uint64_t written = 0;
    uint64_t bySource[SRC_NB];
    memset(bySource, 0, sizeof(bySource));

    if (!Quiet)
        fprintf(stdout, "pass 2: shuffling each bucket in memory\n");

    for (int b = 0; b < buckets; ++b) {
        const size_t n = (size_t)bucketCount[b];
        if (n) {
            uint8_t *data = xmalloc(n * unit);
            FILE *f       = xfopen(bucketPath[b], "rb");
            if (fread(data, unit, n, f) != n)
                die("short read from a bucket file");
            fclose(f);

            /* Fisher-Yates, back to front. */
            uint8_t swap[REC_BYTES + POL_BYTES];
            for (size_t i = n - 1; i > 0; --i) {
                const size_t j = (size_t)rng_below(&rng, (uint64_t)(i + 1));
                if (i == j)
                    continue;
                memcpy(swap, data + i * unit, unit);
                memcpy(data + i * unit, data + j * unit, unit);
                memcpy(data + j * unit, swap, unit);
            }

            for (size_t i = 0; i < n; ++i) {
                if (fwrite(data + i * unit, 1, REC_BYTES, of) != REC_BYTES)
                    die("short write to the output shard");
                if (opf && fwrite(data + i * unit + REC_BYTES, 1, POL_BYTES, opf) != POL_BYTES)
                    die("short write to the output policy sidecar");

                Record rec;
                record_decode(data + i * unit, &rec);
                ++bySource[record_source(&rec)];
                ++written;
            }
            free(data);
        }
        remove(bucketPath[b]);
    }
    free(bucketPath);

    fclose(of);
    if (opf)
        fclose(opf);

    char joined[PATH_CAP];
    joined[0] = '\0';
    for (int i = 0; i < inputCount; ++i) {
        if (strlen(joined) + strlen(inputs[i]) + 2 >= sizeof(joined))
            break;
        if (joined[0])
            strcat(joined, " ");
        strcat(joined, inputs[i]);
    }

    Manifest man;
    memset(&man, 0, sizeof(man));
    man.command = "shuffle";
    man.inputs  = joined;
    man.seed    = seed;
    /* Carry the labelling conditions forward when every input agrees on them.
     * Shuffling reorders records without relabelling any, so the output was
     * labelled at the same nodes and hash as its inputs - and `verify -relabel`
     * needs both to reproduce a score. Left at zero when the inputs disagree or
     * do not say, which readers take as "not recorded". */
    for (int i = 0; i < inputCount; ++i) {
        int n = 0, h = 0;
        const bool haveNodes = manifest_int(inputs[i], "nodes", &n);
        const bool haveHash  = manifest_int(inputs[i], "hash_mb", &h);
        if (i == 0) {
            man.nodes  = haveNodes ? n : 0;
            man.hashMb = haveHash ? h : 0;
        } else {
            if (!haveNodes || n != man.nodes)
                man.nodes = 0;
            if (!haveHash || h != man.hashMb)
                man.hashMb = 0;
        }
    }

    manifest_write(out, &man, written, bySource, policy);

    fprintf(stdout, "shuffled %llu records into %s\n", (unsigned long long)written, out);
    return 0;
}

/* ========================================================================== *
 *  Subcommand: filter
 *
 *  Selects records by source tag. The tag exists so a mixture generated once
 *  can be re-weighted without regenerating anything, and trainer/nnue already
 *  does that at load time with --sources - so this subcommand buys nothing a
 *  trainer flag does not, except the bytes. A shard that will never be trained
 *  on with its tree nodes is cheaper to shrink once than to skip on every
 *  epoch of every run.
 *
 *  The .pol sidecar is read in lockstep and written for the kept records only.
 *  Dropping a record from one file but not the other is the one mistake in
 *  here that leaves a shard of a plausible length which teaches the wrong move
 *  for every position after the first drop, so the input sidecars are
 *  length-checked before a byte is written.
 * ========================================================================== */

static void usage_filter(void) {
    printf("datagen filter <in.cnn>... -o <out.cnn> [-keep NAME]... [-drop NAME]...\n"
           "\n"
           "Copies through only the records whose source tag survives the selection,\n"
           "carrying the .pol sidecar with them. The sources are selfplay, tree, human,\n"
           "engine, book and other.\n"
           "\n"
           "  -o <path>          output shard.                            (required)\n"
           "  -keep NAME         keep only these sources; repeatable.\n"
           "  -drop NAME         drop these sources; repeatable.\n"
           "  -quiet             progress lines off.\n"
           "\n"
           "The first -keep makes the selection a whitelist; -drop subtracts from\n"
           "whatever is selected at that point. With neither, every record is kept.\n"
           "Either every input has a .pol or none may.\n");
}

static int cmd_filter(int argc, char **argv) {
    if (wants_help(argc, argv)) {
        usage_filter();
        return 0;
    }

    const char *out = NULL;
    char *inputs[256];
    int inputCount = 0;
    bool keep[SRC_NB];
    bool whitelist = false;

    for (int i = 0; i < SRC_NB; ++i)
        keep[i] = true;

    for (int i = 0; i < argc; ++i) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) {
            out = argv[++i];
        } else if ((!strcmp(argv[i], "-keep") || !strcmp(argv[i], "-drop")) && i + 1 < argc) {
            const bool dropping = argv[i][1] == 'd';
            SourceTag tag;
            if (!source_from_name(argv[i + 1], &tag))
                dief("unknown source '%s'", argv[i + 1]);
            ++i;
            if (!dropping && !whitelist) {
                for (int s = 0; s < SRC_NB; ++s)
                    keep[s] = false;
                whitelist = true;
            }
            keep[tag] = !dropping;
        } else if (!strcmp(argv[i], "-quiet")) {
            Quiet = true;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "datagen: unknown filter option '%s'\n", argv[i]);
            return 1;
        } else if (inputCount < (int)(sizeof(inputs) / sizeof(inputs[0])))
            inputs[inputCount++] = argv[i];
        else
            die("too many input shards");
    }

    if (!out || inputCount == 0)
        die("usage: datagen filter <in.cnn>... -o <out.cnn> [-keep NAME] [-drop NAME]");

    bool anyKept = false;
    for (int i = 0; i < SRC_NB; ++i)
        anyKept = anyKept || keep[i];
    if (!anyKept)
        die("the selection keeps no source at all");

    /* shard_open() truncates, so an output that is also an input would be
     * emptied before it is read. The shards this runs on are gigabytes. */
    for (int i = 0; i < inputCount; ++i)
        if (!strcmp(inputs[i], out))
            dief("%s is both an input and the output", out);

    /*
     * All inputs must agree about whether a sidecar exists, and each one must
     * be exactly four bytes per record. A short .pol means the pairing is
     * already wrong going in, and copying it through would launder that into a
     * file nothing downstream can tell is broken.
     */
    bool policy = true;
    for (int i = 0; i < inputCount; ++i) {
        char pol[PATH_CAP];
        replace_ext(pol, sizeof(pol), inputs[i], ".pol");
        FILE *f = fopen(pol, "rb");
        if (f)
            fclose(f);
        else
            policy = false;
    }

    uint64_t total = 0;
    for (int i = 0; i < inputCount; ++i) {
        FILE *f = xfopen(inputs[i], "rb");
        fseek64(f, 0, SEEK_END);
        const long long size = (long long)ftell64(f);
        fclose(f);
        if (size % REC_BYTES)
            dief("%s holds %lld bytes, not a multiple of the %d-byte record", inputs[i], size,
                 REC_BYTES);
        const long long records = size / REC_BYTES;

        if (policy) {
            char polPath[PATH_CAP];
            replace_ext(polPath, sizeof(polPath), inputs[i], ".pol");
            FILE *pf = xfopen(polPath, "rb");
            fseek64(pf, 0, SEEK_END);
            const long long polSize = (long long)ftell64(pf);
            fclose(pf);
            if (polSize != records * POL_BYTES)
                dief("%s holds %lld bytes but its %lld records need %lld", polPath, polSize,
                     records, records * POL_BYTES);
        }
        total += (uint64_t)records;
    }

    if (total == 0)
        die("nothing to filter");

    ShardWriter w;
    shard_open(&w, out, policy);

    uint64_t seen = 0;
    uint64_t bySource[SRC_NB];
    memset(bySource, 0, sizeof(bySource));
    double lastReport = now_seconds();

    for (int f = 0; f < inputCount; ++f) {
        FILE *in  = xfopen(inputs[f], "rb");
        FILE *pol = NULL;
        if (policy) {
            char polPath[PATH_CAP];
            replace_ext(polPath, sizeof(polPath), inputs[f], ".pol");
            pol = xfopen(polPath, "rb");
        }

        uint8_t buf[REC_BYTES];
        while (fread(buf, 1, REC_BYTES, in) == REC_BYTES) {
            Record rec;
            record_decode(buf, &rec);

            /* Read the sidecar for EVERY record, kept or not: it is positional,
             * so a skipped read is exactly what desynchronises it. */
            PolicyRecord p = {0, 0};
            if (pol) {
                uint8_t pbuf[POL_BYTES];
                if (fread(pbuf, 1, POL_BYTES, pol) != POL_BYTES)
                    dief("the policy sidecar of %s ran out before its shard did", inputs[f]);
                policy_decode(pbuf, &p);
            }

            ++seen;
            const SourceTag src = record_source(&rec);
            if (src >= SRC_NB)
                dief("%s holds a record with source tag %d", inputs[f], (int)src);
            ++bySource[src];
            if (keep[src])
                shard_write(&w, &rec, &p);

            const double now = now_seconds();
            if (!Quiet && now - lastReport > 5.0) {
                lastReport = now;
                fprintf(stdout, "filter: %llu/%llu read, %llu kept (%.1f%%)\n",
                        (unsigned long long)seen, (unsigned long long)total,
                        (unsigned long long)w.count, 100.0 * (double)w.count / (double)seen);
            }
        }

        fclose(in);
        if (pol)
            fclose(pol);
    }

    char joined[PATH_CAP];
    joined[0] = '\0';
    for (int i = 0; i < inputCount; ++i) {
        if (strlen(joined) + strlen(inputs[i]) + 2 >= sizeof(joined))
            break;
        if (joined[0])
            strcat(joined, " ");
        strcat(joined, inputs[i]);
    }

    Manifest man;
    memset(&man, 0, sizeof(man));
    man.command = "filter";
    man.inputs  = joined;
    shard_close(&w, &man);

    fprintf(stdout, "kept %llu of %llu records in %s\n", (unsigned long long)w.count,
            (unsigned long long)seen, out);
    for (int i = 0; i < SRC_NB; ++i)
        if (bySource[i])
            fprintf(stdout, "  %-10s %10llu  %s\n", SourceNames[i], (unsigned long long)bySource[i],
                    keep[i] ? "kept" : "dropped");
    return 0;
}

/* ========================================================================== *
 *  Subcommand: verify - the Task 1 acceptance gate
 *
 *  Two properties, and a shard that fails either is not usable:
 *
 *    1. Every record round-trips. unpack -> FEN -> board_set_fen -> repack
 *       must give back the identical 32 bytes. That covers the occupancy, the
 *       nibbles, the castling encoding, the en passant square and the clocks
 *       in one comparison.
 *    2. Labelling a position twice, in different runs, gives the same score.
 *       `-relabel N` re-searches N randomly chosen records and diffs.
 *    3. The policy sidecar is still aligned with its records. Every move it
 *       holds was produced FOR that record's position, so every one must be
 *       pseudo-legal in it. An off-by-one in the shuffler would leave a file
 *       that is the right length, parses cleanly, and quietly teaches a policy
 *       head the wrong move for every position in the dataset.
 *
 *  Exits non-zero on any failure, so it works as a Makefile gate.
 * ========================================================================== */

typedef struct {
    char fen[FEN_MAX_LEN];
    int16_t score;
    uint64_t index;
} RelabelSample;

static void usage_verify(void) {
    printf("datagen verify <shard.cnn>... [options]\n"
           "\n"
           "The Task 1 acceptance gate. Exits non-zero on any failure, so it works in a\n"
           "Makefile. Checks three things:\n"
           "\n"
           "  1. every record round-trips - unpack, FEN, board_set_fen, repack - to the\n"
           "     identical 32 bytes;\n"
           "  2. the .pol sidecar is still aligned: every move it holds was produced for\n"
           "     that record's position, so every one must be legal in it;\n"
           "  3. with -relabel, that a label re-searched from scratch reproduces the\n"
           "     score it was stored with. Both the node count and the hash size are\n"
           "     part of how a label was made, and both are taken from the shard's own\n"
           "     manifest; name one on the command line only to override it.\n"
           "\n"
           "  -relabel N         re-search N randomly chosen records.     (0, max 4096)\n"
           "  -nodes N           nodes for those re-searches.        (manifest, or 10000)\n"
           "  -hash MB           hash for those re-searches.             (manifest, or 8)\n"
           "  -seed N            which records get sampled.               (1)\n"
           "  -quiet             progress lines off.\n");
}

static int cmd_verify(int argc, char **argv) {
    if (wants_help(argc, argv)) {
        usage_verify();
        return 0;
    }

    char *inputs[256];
    int inputCount = 0;
    int relabel    = 0;
    int nodes      = 10000;
    int hashMb     = 8;
    uint64_t seed  = 1;
    /* Both are read from the shard's manifest below unless the caller names
     * them, so the defaults above only survive for a shard with no .json. */
    bool nodesGiven = false;
    bool hashGiven  = false;

    for (int i = 0; i < argc; ++i) {
        if (!strcmp(argv[i], "-relabel") && i + 1 < argc)
            relabel = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-nodes") && i + 1 < argc) {
            nodes      = atoi(argv[++i]);
            nodesGiven = true;
        } else if (!strcmp(argv[i], "-hash") && i + 1 < argc) {
            hashMb    = atoi(argv[++i]);
            hashGiven = true;
        } else if (!strcmp(argv[i], "-seed") && i + 1 < argc)
            seed = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "-quiet"))
            Quiet = true;
        else if (argv[i][0] == '-') {
            fprintf(stderr, "datagen: unknown verify option '%s'\n", argv[i]);
            return 1;
        } else if (inputCount < (int)(sizeof(inputs) / sizeof(inputs[0])))
            inputs[inputCount++] = argv[i];
        else
            die("too many input shards");
    }

    if (inputCount == 0)
        die("usage: datagen verify <shard.cnn>... [-relabel N] [-nodes N]");

    if (relabel > 4096)
        relabel = 4096;

    /*
     * A label reproduces only under the conditions it was made under, and the
     * hash size is one of them. `search_clear()` empties the table but keeps
     * its geometry, so the same position at the same node count collides
     * differently in an 8 MB table than in a 64 MB one and occasionally scores
     * differently - about one record in a thousand. That is invisible in a spot
     * check and fatal to a 256-record gate, and it bit exactly once per
     * selfplay shard: `selfplay` defaults to 64 MB and this gate to 8.
     *
     * The manifest already records both numbers. Reading them back is what
     * makes the gate check the shard rather than the caller's memory of it.
     */
    if (relabel > 0) {
        int manNodes = 0, manHash = 0;
        bool haveNodes = false, haveHash = false;

        for (int i = 0; i < inputCount; ++i) {
            int v;
            if (manifest_int(inputs[i], "nodes", &v)) {
                if (haveNodes && v != manNodes)
                    die("these shards were labelled at different node counts - one relabel run "
                        "can only use one, so verify them separately");
                manNodes  = v;
                haveNodes = true;
            }
            if (manifest_int(inputs[i], "hash_mb", &v)) {
                if (haveHash && v != manHash)
                    die("these shards were labelled with different hash sizes - one relabel run "
                        "can only use one, so verify them separately");
                manHash  = v;
                haveHash = true;
            }
        }

        if (haveNodes && nodesGiven && nodes != manNodes)
            fprintf(stderr,
                    "datagen: -nodes %d overrides the manifest's %d - every record will "
                    "look wrong\n",
                    nodes, manNodes);
        else if (haveNodes)
            nodes = manNodes;

        if (haveHash && hashGiven && hashMb != manHash)
            fprintf(stderr,
                    "datagen: -hash %d overrides the manifest's %d - expect spurious "
                    "mismatches\n",
                    hashMb, manHash);
        else if (haveHash)
            hashMb = manHash;

        if (!haveNodes || !haveHash)
            fprintf(stderr,
                    "datagen: %s has no manifest to read the labelling conditions from - "
                    "relabelling at %d nodes, %d MB\n",
                    inputs[0], nodes, hashMb);
    }

    RelabelSample *samples = relabel ? xmalloc((size_t)relabel * sizeof(RelabelSample)) : NULL;
    int sampleCount        = 0;

    Rng rng          = seed;
    uint64_t checked = 0;
    uint64_t bad     = 0;

    uint64_t policyChecked = 0;
    uint64_t policyBad     = 0;

    for (int f = 0; f < inputCount; ++f) {
        FILE *in = xfopen(inputs[f], "rb");

        char polPath[PATH_CAP];
        replace_ext(polPath, sizeof(polPath), inputs[f], ".pol");
        FILE *pol = fopen(polPath, "rb"); /* optional: absent is not an error */

        uint8_t buf[REC_BYTES];
        uint64_t index = 0;

        while (fread(buf, 1, REC_BYTES, in) == REC_BYTES) {
            Record rec;
            record_decode(buf, &rec);

            PolicyRecord policy = {0, 0};
            bool havePolicy     = false;
            if (pol) {
                uint8_t pbuf[POL_BYTES];
                if (fread(pbuf, 1, POL_BYTES, pol) == POL_BYTES) {
                    policy_decode(pbuf, &policy);
                    havePolicy = true;
                } else if (!policyBad) {
                    fprintf(stderr, "datagen: %s is shorter than its shard\n", polPath);
                    ++policyBad;
                }
            }

            const char *problem = NULL;
            char fen[FEN_MAX_LEN];
            Position p;
            uint8_t again[REC_BYTES];
            memset(&p, 0, sizeof(p));

            if (rec.flags & 0xF0)
                problem = "reserved flag bits are set";
            else if (rec.wdl > WDL_UNKNOWN)
                problem = "WDL is out of range";
            else if (record_source(&rec) >= SRC_NB)
                problem = "source tag is out of range";
            else if (!record_to_fen(&rec, fen))
                problem = "record does not decode to a position";
            else if (!board_set_fen(&p, fen))
                problem = "decoded FEN does not parse";
            else {
                Record repacked;
                record_from_position(&repacked, &p, rec.score, rec.wdl, record_source(&rec));
                record_encode(&repacked, again);
                if (memcmp(buf, again, REC_BYTES))
                    problem = "record does not round-trip to identical bytes";
            }

            if (!problem && havePolicy) {
                /* Both moves were produced for THIS position - `best` by its
                 * label search, `cutoff` at the tree node it was sampled from -
                 * so both must be pseudo-legal in it. */
                const Move moves[2] = {(Move)policy.best, (Move)policy.cutoff};
                for (int m = 0; m < 2; ++m) {
                    if (!is_ok_move(moves[m]))
                        continue;
                    ++policyChecked;
                    if (movegen_is_pseudo_legal(&p, moves[m]))
                        continue;
                    if (policyBad < 10) {
                        char buf2[8];
                        fprintf(stderr, "datagen: %s:%llu policy move %s is not legal here: %s\n",
                                inputs[f], (unsigned long long)index, move_to_str(moves[m], buf2),
                                fen);
                    }
                    ++policyBad;
                }
            }

            if (problem) {
                if (bad < 10)
                    fprintf(stderr, "datagen: %s:%llu %s\n", inputs[f], (unsigned long long)index,
                            problem);
                ++bad;
            } else if (relabel) {
                /* Reservoir over the whole dataset, so a relabel check on a
                 * 100M-record shard still costs N searches. */
                int slot = -1;
                if (sampleCount < relabel)
                    slot = sampleCount++;
                else {
                    const uint64_t j = rng_below(&rng, checked + 1);
                    if (j < (uint64_t)relabel)
                        slot = (int)j;
                }
                if (slot >= 0) {
                    snprintf(samples[slot].fen, sizeof(samples[slot].fen), "%s", fen);
                    samples[slot].score = rec.score;
                    samples[slot].index = index;
                }
            }

            ++checked;
            ++index;
        }

        fclose(in);
        if (pol)
            fclose(pol);
    }

    fprintf(stdout, "round-trip: %llu records checked, %llu failures\n",
            (unsigned long long)checked, (unsigned long long)bad);

    if (policyChecked || policyBad)
        fprintf(stdout, "policy:     %llu moves checked, %llu not legal in their record\n",
                (unsigned long long)policyChecked, (unsigned long long)policyBad);

    uint64_t mismatched = 0;
    if (sampleCount) {
        tt_resize((size_t)hashMb);
        fprintf(stdout, "relabel:    re-searching %d records at %d nodes, %d MB hash\n",
                sampleCount, nodes, hashMb);

        for (int i = 0; i < sampleCount; ++i) {
            Position p;
            SearchResult res;
            if (!label_fen(samples[i].fen, nodes, &p, &res)) {
                fprintf(stderr, "datagen: record %llu no longer labels: %s\n",
                        (unsigned long long)samples[i].index, samples[i].fen);
                ++mismatched;
                continue;
            }
            if ((int)res.score != (int)samples[i].score) {
                if (mismatched < 10)
                    fprintf(stderr, "datagen: record %llu score %d, relabelled %d: %s\n",
                            (unsigned long long)samples[i].index, (int)samples[i].score,
                            (int)res.score, samples[i].fen);
                ++mismatched;
            }
        }

        fprintf(stdout, "relabel:    %d checked, %llu mismatches\n", sampleCount,
                (unsigned long long)mismatched);
        if (mismatched)
            fprintf(stderr, "datagen: a label that does not reproduce means the search saw "
                            "state the position does not carry - a warm table, a different "
                            "node count, or a different build\n");
    }

    free(samples);

    if (policyBad)
        fprintf(stderr, "datagen: a policy move that is not legal in its own record means the "
                        "sidecar has drifted out of step with the shard - suspect the shuffler\n");

    if (bad || mismatched || policyBad) {
        fprintf(stdout, "FAIL\n");
        return 1;
    }
    fprintf(stdout, "PASS\n");
    return 0;
}

/* ========================================================================== *
 *  Subcommand: stats
 *
 *  NNUE.md: "The distribution of scores, phases and piece counts is dumped and
 *  eyeballed. A dataset that is 60% endgames or has a score histogram spiked at
 *  zero has a filter bug, and this is the last point where that is cheap to
 *  find."
 * ========================================================================== */

static void bar(FILE *f, uint64_t value, uint64_t max, int width) {
    const int n = max ? (int)((value * (uint64_t)width) / max) : 0;
    for (int i = 0; i < n; ++i)
        fputc('#', f);
    for (int i = n; i < width; ++i)
        fputc(' ', f);
}

static void usage_stats(void) {
    printf("datagen stats <shard.cnn>...\n"
           "\n"
           "Histograms of score, source, result, side to move and piece count, to be\n"
           "eyeballed. A dataset that is mostly endgames, or whose score histogram is\n"
           "spiked at zero, has a filter bug - and this is the last point where that is\n"
           "cheap to find.\n"
           "\n"
           "  -quiet             accepted and ignored.\n");
}

static int cmd_stats(int argc, char **argv) {
    if (wants_help(argc, argv)) {
        usage_stats();
        return 0;
    }

    char *inputs[256];
    int inputCount = 0;

    for (int i = 0; i < argc; ++i) {
        if (!strcmp(argv[i], "-quiet")) {
            Quiet = true;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "datagen: unknown stats option '%s'\n", argv[i]);
            return 1;
        } else if (inputCount < (int)(sizeof(inputs) / sizeof(inputs[0])))
            inputs[inputCount++] = argv[i];
    }
    if (inputCount == 0)
        die("usage: datagen stats <shard.cnn>...");

    enum { SCORE_BINS = 21, SCORE_STEP = 200 };

    uint64_t total = 0, inCheck = 0, blackToMove = 0;
    uint64_t bySource[SRC_NB], byWdl[WDL_NB], byPieces[33], byScore[SCORE_BINS];
    uint64_t halfmoveSum = 0, fullmoveSum = 0;
    long long scoreSum = 0;
    memset(bySource, 0, sizeof(bySource));
    memset(byWdl, 0, sizeof(byWdl));
    memset(byPieces, 0, sizeof(byPieces));
    memset(byScore, 0, sizeof(byScore));

    for (int f = 0; f < inputCount; ++f) {
        FILE *in = xfopen(inputs[f], "rb");
        uint8_t buf[REC_BYTES];
        while (fread(buf, 1, REC_BYTES, in) == REC_BYTES) {
            Record rec;
            record_decode(buf, &rec);

            ++total;
            ++bySource[record_source(&rec) < SRC_NB ? record_source(&rec) : SRC_OTHER];
            ++byWdl[rec.wdl < WDL_NB ? rec.wdl : WDL_UNKNOWN];
            if (rec.flags & REC_IN_CHECK)
                ++inCheck;
            if (rec.stmEp & 0x80)
                ++blackToMove;

            const int pieces = popcount(rec.occupied);
            ++byPieces[pieces >= 0 && pieces <= 32 ? pieces : 32];

            /* Biased by a large multiple of the step so the division floors
             * rather than truncating toward zero, which would otherwise make
             * the bin below zero twice as wide as every other one. */
            int bin = (rec.score + SCORE_STEP / 2 + 1000 * SCORE_STEP) / SCORE_STEP - 1000 +
                      SCORE_BINS / 2;
            if (bin < 0)
                bin = 0;
            if (bin >= SCORE_BINS)
                bin = SCORE_BINS - 1;
            ++byScore[bin];

            scoreSum += rec.score;
            halfmoveSum += rec.halfmove;
            fullmoveSum += rec.fullmove;
        }
        fclose(in);
    }

    if (!total)
        die("no records");

    printf("records          %llu\n", (unsigned long long)total);
    printf("side to move     %.1f%% white, %.1f%% black\n",
           100.0 * (double)(total - blackToMove) / (double)total,
           100.0 * (double)blackToMove / (double)total);
    printf("in check         %llu (%.2f%%)\n", (unsigned long long)inCheck,
           100.0 * (double)inCheck / (double)total);
    printf("mean score       %.1f cp\n", (double)scoreSum / (double)total);
    printf("mean halfmove    %.1f\n", (double)halfmoveSum / (double)total);
    printf("mean fullmove    %.1f\n", (double)fullmoveSum / (double)total);

    printf("\nsource\n");
    for (int i = 0; i < SRC_NB; ++i)
        if (bySource[i])
            printf("  %-10s %10llu  %5.1f%%\n", SourceNames[i], (unsigned long long)bySource[i],
                   100.0 * (double)bySource[i] / (double)total);

    static const char *const WdlNames[WDL_NB] = {"loss", "draw", "win", "unknown"};
    printf("\nresult (side to move)\n");
    for (int i = 0; i < WDL_NB; ++i)
        if (byWdl[i])
            printf("  %-10s %10llu  %5.1f%%\n", WdlNames[i], (unsigned long long)byWdl[i],
                   100.0 * (double)byWdl[i] / (double)total);

    uint64_t peak = 0;
    for (int i = 0; i < SCORE_BINS; ++i)
        if (byScore[i] > peak)
            peak = byScore[i];

    printf("\nscore, centipawns (side to move)\n");
    for (int i = 0; i < SCORE_BINS; ++i) {
        const int centre = (i - SCORE_BINS / 2) * SCORE_STEP;
        printf("  %+6d  %10llu  ", centre, (unsigned long long)byScore[i]);
        bar(stdout, byScore[i], peak, 40);
        printf("\n");
    }

    peak = 0;
    for (int i = 2; i <= 32; ++i)
        if (byPieces[i] > peak)
            peak = byPieces[i];

    printf("\npieces on the board\n");
    for (int i = 2; i <= 32; ++i) {
        if (!byPieces[i])
            continue;
        printf("  %6d  %10llu  ", i, (unsigned long long)byPieces[i]);
        bar(stdout, byPieces[i], peak, 40);
        printf("\n");
    }

    return 0;
}

/* ========================================================================== *
 *  Subcommand: dump
 * ========================================================================== */

static void usage_dump(void) {
    printf("datagen dump <shard.cnn> [options]\n"
           "\n"
           "Records as semicolon-separated text: fen;score;wdl;source;in_check, plus\n"
           "best;cutoff with -pol. This is the C side of the format contract that\n"
           "trainer/tests/test_format.py checks nnue/format.py against.\n"
           "\n"
           "  -n N               records to print.                        (10)\n"
           "  -skip N            start at record N.                       (0)\n"
           "  -pol               also read <shard>.pol and print its moves.\n");
}

static int cmd_dump(int argc, char **argv) {
    if (wants_help(argc, argv)) {
        usage_dump();
        return 0;
    }

    const char *input = NULL;
    long count        = 10;
    long skip         = 0;
    bool withPolicy   = false;

    for (int i = 0; i < argc; ++i) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc)
            count = atol(argv[++i]);
        else if (!strcmp(argv[i], "-skip") && i + 1 < argc)
            skip = atol(argv[++i]);
        else if (!strcmp(argv[i], "-pol"))
            withPolicy = true;
        else if (argv[i][0] == '-') {
            fprintf(stderr, "datagen: unknown dump option '%s'\n", argv[i]);
            return 1;
        } else
            input = argv[i];
    }

    if (!input)
        die("usage: datagen dump <shard.cnn> [-n N] [-skip N] [-pol]");

    FILE *in  = xfopen(input, "rb");
    FILE *pol = NULL;
    if (withPolicy) {
        char path[PATH_CAP];
        replace_ext(path, sizeof(path), input, ".pol");
        pol = xfopen(path, "rb");
    }

    if (skip) {
        fseek64(in, (long long)skip * REC_BYTES, SEEK_SET);
        if (pol)
            fseek64(pol, (long long)skip * POL_BYTES, SEEK_SET);
    }

    /* Semicolon-separated so it round-trips through a spreadsheet or a python
     * split(';'), and the FEN keeps its own spaces. */
    printf("fen;score;wdl;source;in_check%s\n", withPolicy ? ";best;cutoff" : "");

    uint8_t buf[REC_BYTES];
    for (long i = 0; i < count && fread(buf, 1, REC_BYTES, in) == REC_BYTES; ++i) {
        Record rec;
        record_decode(buf, &rec);

        char fen[FEN_MAX_LEN];
        if (!record_to_fen(&rec, fen)) {
            printf("<undecodable>;%d;%u;%u;%u\n", rec.score, rec.wdl, (unsigned)record_source(&rec),
                   (rec.flags & REC_IN_CHECK) ? 1u : 0u);
            continue;
        }

        printf("%s;%d;%u;%s;%u", fen, rec.score, rec.wdl, SourceNames[record_source(&rec)],
               (rec.flags & REC_IN_CHECK) ? 1u : 0u);

        if (pol) {
            uint8_t pbuf[POL_BYTES];
            PolicyRecord p = {0, 0};
            if (fread(pbuf, 1, POL_BYTES, pol) == POL_BYTES)
                policy_decode(pbuf, &p);
            char a[8], b[8];
            printf(";%s;%s", p.best ? move_to_str((Move)p.best, a) : "-",
                   p.cutoff ? move_to_str((Move)p.cutoff, b) : "-");
        }
        printf("\n");
    }

    fclose(in);
    if (pol)
        fclose(pol);
    return 0;
}

/* ========================================================================== *
 *  Entry point
 * ========================================================================== */

static void usage(void) {
    fprintf(stderr, "usage:\n"
                    "  datagen selfplay -o <shard%%02d.cnn> [-games N] [-nodes N] [-threads N]\n"
                    "  datagen label <in.epd>... -o <shard.cnn> [-source human] [-nodes N]\n"
                    "                            [-max N per worker] [-stride N] [-skip N]\n"
                    "  datagen shuffle <in.cnn>... -o <out.cnn> [-buckets K] [-seed S]\n"
                    "  datagen filter <in.cnn>... -o <out.cnn> [-keep NAME] [-drop NAME]\n"
                    "  datagen verify <shard.cnn>... [-relabel N] [-nodes N]\n"
                    "  datagen stats <shard.cnn>...\n"
                    "  datagen dump <shard.cnn> [-n N] [-skip N] [-pol]\n"
                    "\n"
                    "`datagen <subcommand> -help` lists every option and its default.\n"
                    "Every subcommand takes -quiet. The design is docs/NNUE.md.\n");
}

/* Pulls `--worker N` out of argv so the subcommand parsers never see it. */
static void extract_worker_index(int *argc, char **argv) {
    for (int i = 0; i < *argc; ++i) {
        if (strcmp(argv[i], "--worker") || i + 1 >= *argc)
            continue;

        WorkerOverride = atoi(argv[i + 1]);
        for (int j = i; j + 2 < *argc; ++j)
            argv[j] = argv[j + 2];
        *argc -= 2;
        return;
    }
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);

    /* The same order main.c uses: attack tables underpin everything, and the
     * Zobrist keys must exist before any position is hashed. */
    bb_init();
    zobrist_init();
    eval_init();
#ifdef EVAL_NNUE
    /* `make datagen EVAL=nnue` is how the pipeline bootstraps: net n+1 is
     * trained on labels produced by searches that used net n. The label
     * definition is "the search in this build", so the build has to be able
     * to carry a net. */
    nnue_init();
#endif
    search_init();
    tt_resize(8);

    extract_worker_index(&argc, argv);

    if (argc < 2) {
        usage();
        return 1;
    }

    int rc;
    if (!strcmp(argv[1], "selfplay"))
        rc = cmd_selfplay(argc - 2, argv + 2);
    else if (!strcmp(argv[1], "label"))
        rc = cmd_label(argc - 2, argv + 2);
    else if (!strcmp(argv[1], "shuffle"))
        rc = cmd_shuffle(argc - 2, argv + 2);
    else if (!strcmp(argv[1], "filter"))
        rc = cmd_filter(argc - 2, argv + 2);
    else if (!strcmp(argv[1], "verify"))
        rc = cmd_verify(argc - 2, argv + 2);
    else if (!strcmp(argv[1], "stats"))
        rc = cmd_stats(argc - 2, argv + 2);
    else if (!strcmp(argv[1], "dump"))
        rc = cmd_dump(argc - 2, argv + 2);
    else {
        fprintf(stderr, "datagen: unknown subcommand '%s'\n", argv[1]);
        usage();
        rc = 1;
    }

    tt_free();
    return rc;
}
