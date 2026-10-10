/*
 * syzygytest.c - does the prober answer known endgames correctly? Every expected result
 * comes from the lichess.org tablebase API, an independent oracle. The cases defeat a
 * material count (contrast pairs one square apart, drawn material advantages), and each
 * is also tested colour-mirrored.
 */
#include "syzygytest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "movegen.h"
#include "syzygy.h"
#include "types.h"

typedef enum { EXPECT_WIN, EXPECT_DRAW, EXPECT_LOSS } Expect;

typedef struct {
    const char *fen;
    Expect expect;
    const char *what;
} TbCase;

/* Every FEN has halfmove clock 0: that is the condition the WDL probe fires under, and a
 * case that cannot be probed is a case that tests nothing. */
static const TbCase Cases[] = {
    /* A piece up and drawn, worth up to +8 to a material count. */

    {"k7/8/K7/P7/8/8/6p1/6N1 w - - 0 1", EXPECT_DRAW, "KNPvKP, rook pawn, king in the corner"},
    {"k7/8/K7/P7/8/6p1/8/6N1 w - - 0 1", EXPECT_DRAW, "KNPvKP, same, black pawn a rank back"},

    {"k7/8/1K6/P7/8/8/5p2/5N2 w - - 0 1", EXPECT_WIN, "KNPvKP, king on b6 instead: won"},

    {"7k/8/5K2/7P/8/8/8/7B w - - 0 1", EXPECT_DRAW, "KBPvK, wrong-colour bishop on h1"},
    {"7k/8/5K2/7P/8/8/8/6B1 w - - 0 1", EXPECT_WIN, "KBPvK, right-colour bishop on g1"},

    {"7K/7Q/8/8/8/8/1kp5/8 w - - 0 1", EXPECT_DRAW, "KQvKP, c-pawn on the 7th"},
    {"7K/7Q/8/8/8/8/2p5/1k6 w - - 0 1", EXPECT_DRAW, "KQvKP, c-pawn, king behind"},

    {"8/8/8/8/8/1k6/1p6/1K1N1N2 w - - 0 1", EXPECT_DRAW, "KNNvKP, no forced mate"},

    {"8/8/8/4k3/4p3/4P3/4K3/8 w - - 0 1", EXPECT_DRAW, "KPvKP, blocked and facing"},
    {"8/8/8/8/8/6k1/6P1/6K1 b - - 0 1", EXPECT_DRAW, "KPvK, defender has the opposition"},

    /* The same material WON, one square over - no evaluation of material tells these
     * apart - and wins that need technique rather than counting. */
    {"8/8/8/4k3/4p3/4P3/4KN2/8 w - - 0 1", EXPECT_WIN, "KNPvKP, centre pawns: won"},
    {"8/8/8/4k3/8/8/4K3/4Q3 w - - 0 1", EXPECT_WIN, "KQvK"},
    {"8/8/8/3k4/8/8/3K4/3R4 w - - 0 1", EXPECT_WIN, "KRvK"},
    {"8/8/8/4k3/8/8/3r4/3QK3 w - - 0 1", EXPECT_WIN, "KQvKR"},

    {"4q3/4k3/8/8/8/8/4K3/8 w - - 0 1", EXPECT_LOSS, "KvKQ"},
    {"8/8/8/3k4/3p4/3P4/3KN3/8 b - - 0 1", EXPECT_LOSS, "KPvKNP, centre pawns"},
};

/* Mirrors a FEN vertically and swaps the colours: a position and its reflection must give
 * the same result from the mover's point of view. Done on the string and handed to
 * board_set_fen like any other FEN, which keeps every derived field computed the one way
 * the engine computes it. */
static bool mirror_fen(const char *fen, char *out, size_t cap) {
    char board[128], rest[128];
    if (sscanf(fen, "%127s %127[^\n]", board, rest) != 2)
        return false;

    char *ranks[8];
    int n       = 0;
    char *saved = board;
    for (char *p = board;; ++p) {
        if (*p == '/' || *p == '\0') {
            const char end = *p;
            *p             = '\0';
            if (n == 8)
                return false;
            ranks[n++] = saved;
            saved      = p + 1;
            if (end == '\0')
                break;
        }
    }
    if (n != 8)
        return false;

    size_t used = 0;
    for (int r = 7; r >= 0; --r) {
        for (const char *p = ranks[r]; *p; ++p) {
            if (used + 2 >= cap)
                return false;

            char ch = *p;
            if (ch >= 'a' && ch <= 'z')
                ch = (char)(ch - 'a' + 'A');
            else if (ch >= 'A' && ch <= 'Z')
                ch = (char)(ch - 'A' + 'a');
            out[used++] = ch;
        }
        if (r > 0) {
            if (used + 2 >= cap)
                return false;
            out[used++] = '/';
        }
    }
    out[used] = '\0';

    /* A piece belongs to whichever side its case names, so swapping case is exactly swapping
     * colours. The side to move flips with them and the rest of the FEN is unchanged, which
     * holds only because these cases have no castling rights and no en passant square. */
    const char stm = rest[0] == 'w' ? 'b' : 'w';
    if (used + strlen(rest) + 2 >= cap)
        return false;
    out[used++] = ' ';
    out[used++] = stm;
    strcpy(out + used, rest + 1);
    return true;
}

static const char *expect_name(Expect e) {
    return e == EXPECT_WIN ? "win" : e == EXPECT_DRAW ? "draw" : "loss";
}

/* The probe answers at ply 0, so a win is exactly VALUE_TB_WIN. */
static bool value_matches(Value v, Expect e) {
    switch (e) {
    case EXPECT_WIN: return v == VALUE_TB_WIN;
    case EXPECT_LOSS: return v == -VALUE_TB_WIN;
    default: return v == VALUE_DRAW;
    }
}

static const char *value_name(Value v) {
    if (v == VALUE_NONE)
        return "NOT PROBED";
    if (v == VALUE_TB_WIN)
        return "win";
    if (v == -VALUE_TB_WIN)
        return "loss";
    if (v == VALUE_DRAW)
        return "draw";
    return "?";
}

static int check(const char *fen, Expect expect, const char *what, const char *how) {
    Position pos;
    if (!board_set_fen(&pos, fen)) {
        printf("  FAIL  %-44s %s: unparseable FEN %s\n", what, how, fen);
        return 1;
    }

    const Value v = syzygy_probe_wdl(&pos, 0);
    if (value_matches(v, expect))
        return 0;

    printf("  FAIL  %-44s %s: expected %s, probe said %s\n", what, how, expect_name(expect),
           value_name(v));
    printf("        %s\n", fen);
    return 1;
}

/* Defined with the position generator below; the conversion gate needs it first. */
static bool has_any_legal(Position *pos);

/* Does a won root convert? A move that keeps the win without progress scores just as well
 * as one that converts, until the fifty-move rule takes it. These play both sides from the
 * root probe and require mate with no draw on the way. Each FEN is a win for the side to
 * move, confirmed on lichess.org. */
typedef struct {
    const char *fen;
    const char *what;
} ConvertCase;

static const ConvertCase ConvertCases[] = {
    /* The original bug: the queen stood next to the zeroing capture instead of making it. */
    {"8/8/7p/4K2P/8/8/8/k2q4 b - - 17 80", "KQPvKP, the capture is one move away"},
    /* The same with the clock nearly out. */
    {"8/8/7p/4K2P/8/8/8/k2q4 b - - 90 80", "KQPvKP, same, clock 90 of 100"},
    /* The king step looks nearer than the promotion. */
    {"8/8/8/8/8/1K6/7p/k7 b - - 0 87", "KPvK, promote or shuffle the king"},

    /* No zeroing move at all, with a high clock. */
    {"8/8/8/4k3/8/8/4K3/4Q3 w - - 30 1", "KQvK, clock 30, mate is the only zeroing"},
    {"8/8/8/3k4/8/8/3K4/3R4 w - - 60 1", "KRvK, clock 60, 23 plies to mate"},

    /* Wins that need technique. */
    {"8/8/8/4k3/4p3/4P3/4KN2/8 w - - 5 1", "KNPvKP, centre pawns, 37 plies to mate"},
    {"7k/8/5K2/7P/8/8/8/6B1 w - - 40 1", "KBPvK, right-colour bishop, clock 40"},
    {"8/8/8/4k3/8/8/3r4/3QK3 w - - 20 1", "KQvKR, clock 20"},
};

/* Generous; it exists so a shuffling prober fails instead of hanging the gate. */
enum { CONVERT_MAX_PLIES = 600 };

static int check_converts(const ConvertCase *c) {
    Position pos;
    if (!board_set_fen(&pos, c->fen)) {
        printf("  FAIL  %-44s unparseable FEN %s\n", c->what, c->fen);
        return 1;
    }

    const Color winner = pos.sideToMove;

    for (int ply = 0; ply < CONVERT_MAX_PLIES; ++ply) {
        if (!has_any_legal(&pos)) {
            /* Mate for the winner is the pass. Stalemate, or the winner mated, is not. */
            if (board_checkers(&pos) && pos.sideToMove != winner)
                return 0;
            printf("  FAIL  %-44s ply %d: %s instead of mate\n", c->what, ply,
                   board_checkers(&pos) ? "the winner is mated" : "stalemate");
            printf("        %s\n", c->fen);
            return 1;
        }

        if (board_is_draw(&pos, 0)) {
            printf("  FAIL  %-44s ply %d: drawn with the win still on the board (clock %d)\n",
                   c->what, ply, pos.halfmoveClock);
            printf("        %s\n", c->fen);
            return 1;
        }

        const SyzygyRoot r = syzygy_probe_root(&pos);
        if (r.value == VALUE_NONE || r.move == MOVE_NONE) {
            printf("  FAIL  %-44s ply %d: root probe declined (%s)\n", c->what, ply,
                   r.value == VALUE_NONE ? "no value" : "no move");
            printf("        %s\n", c->fen);
            return 1;
        }

        if (pos.sideToMove == winner && r.value != VALUE_TB_WIN) {
            printf("  FAIL  %-44s ply %d: the win became %s\n", c->what, ply, value_name(r.value));
            printf("        %s\n", c->fen);
            return 1;
        }

        if (!movegen_is_pseudo_legal(&pos, r.move) || !movegen_is_legal(&pos, r.move)) {
            char buf[6];
            printf("  FAIL  %-44s ply %d: illegal move %s\n", c->what, ply,
                   move_to_str(r.move, pos.chess960, buf));
            printf("        %s\n", c->fen);
            return 1;
        }

        board_do_move(&pos, r.move);
    }

    printf("  FAIL  %-44s no mate in %d plies\n", c->what, CONVERT_MAX_PLIES);
    printf("        %s\n", c->fen);
    return 1;
}

int syzygy_verify_suite(const char *path) {
    if (!syzygy_init(path)) {
        printf("syzygy: no usable tablebases at %s\n", path);
        return 1;
    }

    const int men = syzygy_max_pieces();
    printf("syzygy: %d-man tablebases at %s\n", men, path);
    if (men < 5) {
        printf("FAIL: the suite needs 5-man tables; these cover %d\n", men);
        syzygy_free();
        return 1;
    }

    const int count = (int)(sizeof(Cases) / sizeof(Cases[0]));
    int failures    = 0;

    for (int i = 0; i < count; ++i) {
        failures += check(Cases[i].fen, Cases[i].expect, Cases[i].what, "as given");

        char mirrored[FEN_MAX_LEN];
        if (!mirror_fen(Cases[i].fen, mirrored, sizeof(mirrored))) {
            printf("  FAIL  %-44s could not be mirrored\n", Cases[i].what);
            ++failures;
            continue;
        }
        failures += check(mirrored, Cases[i].expect, Cases[i].what, "mirrored");
    }

    printf("syzygy: %d cases (%d probes, each position and its mirror), %d failures\n", count,
           count * 2, failures);

    const int converts = (int)(sizeof(ConvertCases) / sizeof(ConvertCases[0]));
    int convFailures   = 0;
    for (int i = 0; i < converts; ++i)
        convFailures += check_converts(&ConvertCases[i]);

    printf("syzygy: %d won roots played out to mate, %d failures\n", converts, convFailures);

    syzygy_free();
    return failures + convFailures;
}

/* The position generator. A configuration (a multiset of non-king pieces) is an integer
 * whose k-th decimal digit is the k-th piece's code, canonical when the digits are
 * non-increasing; codes that are not canonical are skipped. */
static const PieceType GenTypes[5] = {PAWN, KNIGHT, BISHOP, ROOK, QUEEN};

enum { GEN_MAX_EXTRA = 5 };

/* Fills counts[colour][type] and returns the number of non-king pieces, or -1 when `code`
 * is not a canonical (non-increasing) multiset. */
static int config_decode(uint64_t code, int extra, int counts[2][PIECE_TYPE_NB]) {
    for (int c = 0; c < 2; ++c)
        for (int t = 0; t < PIECE_TYPE_NB; ++t)
            counts[c][t] = 0;

    int prev = 10;
    int n    = 0;
    for (int k = 0; k < extra; ++k) {
        const int digit = (int)(code % 10);
        code /= 10;
        if (digit > prev)
            return -1;
        prev = digit;
        counts[digit / 5][GenTypes[digit % 5]]++;
        ++n;
    }
    return n;
}

static int config_count_for(int extra) {
    uint64_t limit = 1;
    for (int k = 0; k < extra; ++k)
        limit *= 10;

    int counts[2][PIECE_TYPE_NB];
    int n = 0;
    for (uint64_t code = 0; code < limit; ++code)
        if (config_decode(code, extra, counts) >= 0)
            ++n;
    return n;
}

/* The `index`-th canonical code among those with exactly `extra` pieces. */
static uint64_t config_code(int extra, int index, int *found) {
    uint64_t limit = 1;
    for (int k = 0; k < extra; ++k)
        limit *= 10;

    int counts[2][PIECE_TYPE_NB];
    int seen = 0;
    for (uint64_t code = 0; code < limit; ++code) {
        if (config_decode(code, extra, counts) < 0)
            continue;
        if (seen == index) {
            *found = 1;
            return code;
        }
        ++seen;
    }
    *found = 0;
    return 0;
}

/* Configurations are laid out by piece count - every 2-man one, then every 3-man one - so
 * a prefix of the space is the smaller endgames and a run cut short still covered whole
 * piece counts. */
static int config_split(int maxMen, int config, int *extraOut) {
    const int maxExtra = maxMen - 2;
    for (int extra = 0; extra <= maxExtra && extra <= GEN_MAX_EXTRA; ++extra) {
        const int n = config_count_for(extra);
        if (config < n) {
            *extraOut = extra;
            return config;
        }
        config -= n;
    }
    *extraOut = -1;
    return 0;
}

/* Every material configuration up to a piece count, each with seeded random legal
 * placements. Material is covered exhaustively and only placement is random: a prober that
 * is wrong is wrong for a whole table, which uniform sampling would find only in proportion
 * to that table's size. Every position is a pure function of (config, seed), which is what
 * lets the oracle's verdicts stay checkable after the oracle is gone. */
static int tbgen_config_count(int maxMen) {
    const int maxExtra = maxMen - 2;
    int total          = 0;
    for (int extra = 0; extra <= maxExtra && extra <= GEN_MAX_EXTRA; ++extra)
        total += config_count_for(extra);
    return total;
}

static bool config_counts(int maxMen, int config, int counts[2][PIECE_TYPE_NB], int *extraOut) {
    const int index = config_split(maxMen, config, extraOut);
    if (*extraOut < 0)
        return false;

    int found;
    const uint64_t code = config_code(*extraOut, index, &found);
    return found && config_decode(code, *extraOut, counts) >= 0;
}

static void tbgen_config_name(int maxMen, int config, char *buf, size_t cap) {
    int counts[2][PIECE_TYPE_NB];
    int extra;

    if (cap < 16) {
        if (cap > 0)
            buf[0] = 0;
        return;
    }
    if (!config_counts(maxMen, config, counts, &extra)) {
        buf[0] = 0;
        return;
    }

    size_t n = 0;
    for (int c = 0; c < 2; ++c) {
        if (c == 1)
            buf[n++] = 'v';
        buf[n++] = 'K';
        for (int t = QUEEN; t >= PAWN; --t)
            for (int i = 0; i < counts[c][t]; ++i)
                buf[n++] = " PNBRQK"[t];
    }
    buf[n] = 0;
}

/* SplitMix64, so adjacent seeds give unrelated positions. */
static uint64_t splitmix(uint64_t *x) {
    uint64_t z = (*x += 0x9E3779B97F4A7C15ULL);
    z          = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z          = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* Built as a FEN so board_set_fen fills in every derived field. */
static bool tbgen_position(int maxMen, int config, uint64_t seed, Position *pos) {
    int counts[2][PIECE_TYPE_NB];
    int extra;
    if (!config_counts(maxMen, config, counts, &extra))
        return false;

    uint64_t rng = seed * 0x2545F4914F6CDD1DULL + (uint64_t)config * 0x9E3779B97F4A7C15ULL + 1;

    char board[SQUARE_NB];
    for (int s = 0; s < SQUARE_NB; ++s)
        board[s] = 0;

    /* Kings first: everything else needs somewhere to go around them. */
    const Square wk = (Square)(splitmix(&rng) % SQUARE_NB);
    Square bk;
    int guard = 0;
    do {
        bk = (Square)(splitmix(&rng) % SQUARE_NB);
        if (++guard > 64)
            return false;
    } while (bk == wk);
    board[wk] = 'K';
    board[bk] = 'k';

    int placedW = 0, placedB = 0;
    /* Sometimes build en passant geometry on purpose: by chance it arose 63 times in
     * 410,960 placements. */
    if (counts[0][PAWN] && counts[1][PAWN] && (splitmix(&rng) & 1)) {
        const int f  = (int)(splitmix(&rng) % 7);
        const int wq = 24 + f;
        const int bq = wq + 1;
        if (board[wq] == 0 && board[bq] == 0 && board[wq - 8] == 0 && board[wq - 16] == 0) {
            board[wq] = 'P';
            board[bq] = 'p';
            placedW   = 1;
            placedB   = 1;
        }
    }

    for (int c = 0; c < 2; ++c) {
        for (int t = PAWN; t <= QUEEN; ++t) {
            int already = (t == PAWN) ? (c == 0 ? placedW : placedB) : 0;
            for (int i = already; i < counts[c][t]; ++i) {
                Square s;
                int tries = 0;
                do {
                    s = (t == PAWN) ? (Square)(8 + splitmix(&rng) % 48)
                                    : (Square)(splitmix(&rng) % SQUARE_NB);
                    if (++tries > 128)
                        return false;
                } while (board[s] != 0);
                board[s] = (c == 0) ? " PNBRQK"[t] : (char)(" pnbrqk"[t]);
            }
        }
    }

    char fen[FEN_MAX_LEN];
    size_t n = 0;
    for (int r = 7; r >= 0; --r) {
        int empty = 0;
        for (int f = 0; f < 8; ++f) {
            const char pc = board[r * 8 + f];
            if (pc == 0) {
                ++empty;
                continue;
            }
            if (empty) {
                fen[n++] = (char)('0' + empty);
                empty    = 0;
            }
            fen[n++] = pc;
        }
        if (empty)
            fen[n++] = (char)('0' + empty);
        if (r > 0)
            fen[n++] = '/';
    }

    char epField[3] = {'-', 0, 0};
    char stm        = (splitmix(&rng) & 1) ? 'w' : 'b';

    {
        for (int sq = 0; sq < SQUARE_NB; ++sq) {
            const int f = sq & 7, r = sq >> 3;

            if (board[sq] == 'P' && r == 3 && board[sq - 8] == 0 && board[sq - 16] == 0 &&
                ((f > 0 && board[sq - 1] == 'p') || (f < 7 && board[sq + 1] == 'p'))) {
                epField[0] = (char)('a' + f);
                epField[1] = '3';
                stm        = 'b';
                break;
            }

            if (board[sq] == 'p' && r == 4 && board[sq + 8] == 0 && board[sq + 16] == 0 &&
                ((f > 0 && board[sq - 1] == 'P') || (f < 7 && board[sq + 1] == 'P'))) {
                epField[0] = (char)('a' + f);
                epField[1] = '6';
                stm        = 'w';
                break;
            }
        }
    }

    /* No castling and a zero clock, as WDL requires; an ep square only when consistent. */
    n += (size_t)snprintf(fen + n, FEN_MAX_LEN - n, " %c - %s 0 1", stm, epField);
    (void)n;

    if (!board_set_fen(pos, fen))
        return false;

    /* The side not to move may not be in check (this also rejects adjacent kings). */
    const Color them = pos->sideToMove == WHITE ? BLACK : WHITE;
    return !board_square_attacked(pos, king_square(pos, them), pos->sideToMove, occupied_bb(pos));
}

/* The root probe needs a legal move; the sealer skipped the same positions. */
static bool has_any_legal(Position *pos) {
    ScoredMove list[MAX_MOVES];
    const int n = movegen_generate(pos, board_checkers(pos) ? GEN_EVASIONS : GEN_ALL, list);
    for (int i = 0; i < n; ++i)
        if (movegen_is_legal(pos, list[i].m))
            return true;
    return false;
}

/* FNV-1a, so the manifest is one diffable line per configuration. */
static uint64_t tbgen_checksum(uint64_t acc, int wdl, int dtz) {
    const uint64_t Prime = 0x100000001B3ULL;
    acc                  = (acc ^ (uint64_t)(uint32_t)wdl) * Prime;
    acc                  = (acc ^ (uint64_t)(uint32_t)dtz) * Prime;
    return acc;
}

/* Folded in for a declined probe, so declining more than the oracle did fails. */
enum { TBGEN_DECLINED = 99 };

/* Must match the sealer exactly: the root probe is skipped without a legal move, and the
 * distance folded in as a magnitude (the oracle's is unsigned). */
static uint64_t tbgen_config_checksum(int maxMen, int config, uint64_t seed, long per) {
    uint64_t acc = 0xcbf29ce484222325ULL;

    for (long i = 0; i < per; ++i) {
        Position pos;
        if (!tbgen_position(maxMen, config, seed + (uint64_t)i, &pos))
            continue;

        const Value v = syzygy_probe_wdl(&pos, 0);
        const int wdl = v == VALUE_NONE      ? TBGEN_DECLINED
                        : v == VALUE_TB_WIN  ? 1
                        : v == -VALUE_TB_WIN ? -1
                                             : 0;

        int dtz = TBGEN_DECLINED;
        if (has_any_legal(&pos)) {
            const SyzygyRoot r = syzygy_probe_root(&pos);
            if (r.value != VALUE_NONE)
                dtz = r.dtz < 0 ? -r.dtz : r.dtz;
        }

        acc = tbgen_checksum(acc, wdl, dtz);
    }
    return acc;
}

int syzygy_verify_manifest(const char *tbPath, const char *manifestPath) {
    FILE *f = fopen(manifestPath, "r");
    if (!f) {
        printf("syzygy: cannot open the manifest at %s\n", manifestPath);
        return 1;
    }

    int maxMen    = 0;
    long per      = 0;
    uint64_t seed = 0;
    int version   = 0;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n')
            continue;
        if (sscanf(line, "version %d", &version) == 1)
            continue;
        if (sscanf(line, "men %d", &maxMen) == 1)
            continue;
        if (sscanf(line, "per %ld", &per) == 1)
            continue;
        if (sscanf(line, "seed %llu", (unsigned long long *)&seed) == 1)
            continue;
        break;
    }

    if (version != 1 || maxMen < 3 || per < 1) {
        printf("syzygy: %s is not a manifest this build understands "
               "(version %d, men %d, per %ld)\n",
               manifestPath, version, maxMen, per);
        fclose(f);
        return 1;
    }

    if (!syzygy_init(tbPath)) {
        printf("syzygy: no usable tablebases at %s\n", tbPath);
        fclose(f);
        return 1;
    }
    if (syzygy_max_pieces() < maxMen) {
        printf("syzygy: the manifest covers %d men and these tables reach %d\n", maxMen,
               syzygy_max_pieces());
        syzygy_free();
        fclose(f);
        return 1;
    }

    rewind(f);

    int checked = 0, failures = 0;
    while (fgets(line, sizeof(line), f)) {
        char name[32];
        unsigned long long want;
        if (sscanf(line, "%31s %llx", name, &want) != 2)
            continue;
        if (name[0] != 'K')
            continue;

        /* Lines follow the generator's order; the names are checked anyway. */
        char expect[16];
        tbgen_config_name(maxMen, checked, expect, sizeof(expect));
        if (strcmp(expect, name) != 0) {
            printf("  FAIL  line %d names %s, this build's configuration %d is %s\n", checked + 1,
                   name, checked, expect);
            ++failures;
            ++checked;
            continue;
        }

        const uint64_t got = tbgen_config_checksum(maxMen, checked, seed, per);
        if (got != want) {
            printf("  FAIL  %-10s expected %016llx, got %016llx\n", name, want,
                   (unsigned long long)got);
            ++failures;
        }
        ++checked;
    }

    fclose(f);

    const int configs = tbgen_config_count(maxMen);
    if (checked != configs) {
        printf("syzygy: the manifest holds %d configurations, this build enumerates %d\n", checked,
               configs);
        ++failures;
    }

    printf("syzygy: manifest %s, %d configurations x %ld positions, %d failures\n", manifestPath,
           checked, per, failures);

    syzygy_free();
    return failures;
}
