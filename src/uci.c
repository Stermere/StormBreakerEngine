/*
 * uci.c - the UCI protocol layer. Never block on the search (invariant 4), and never let
 * malformed input be fatal.
 */
#include "uci.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bench.h"
#include "bitboard.h"
#include "eval.h"
#include "movegen.h"
#include "nnue.h"
#include "perft.h"
#include "search.h"
#include "syzygy.h"
#include "test/chess960test.h"
#include "test/movepicktest.h"
#include "test/smptest.h"
#include "test/syzygytest.h"
#ifdef UNC_PROBE
#include "test/uncprobe.h"
#endif
#include "timeman.h"
#include "tt.h"

#define MAX_INPUT 65536

static Position Pos;

/* Shared by cmd_uci() and cmd_setoption() so the advertised and enforced bounds agree. */
#define OPT_HASH_MIN         1
#define OPT_HASH_MAX         65536
#define OPT_HASH_DEFAULT     16
#define OPT_OVERHEAD_MIN     0
#define OPT_OVERHEAD_MAX     5000
#define OPT_OVERHEAD_DEFAULT 10

static int OptHash         = OPT_HASH_DEFAULT;
static int OptMoveOverhead = OPT_OVERHEAD_DEFAULT;

static bool announcedChess960 = false;

/* False after a rejected `position`, until one succeeds. `go` then answers the null move
 * rather than a move from the stale board. */
static bool posValid = true;

int uci_move_overhead(void) { return OptMoveOverhead; }

static char *skip_spaces(char *s) {
    while (*s == ' ' || *s == '\t')
        ++s;
    return s;
}

/* The next token, or NULL at the end. Writes NULs into the buffer. */
static char *next_token(char **cursor) {
    char *s = skip_spaces(*cursor);
    if (*s == '\0') {
        *cursor = s;
        return NULL;
    }

    char *start = s;
    while (*s && *s != ' ' && *s != '\t')
        ++s;
    if (*s) {
        *s = '\0';
        ++s;
    }

    *cursor = s;
    return start;
}

static bool token_is(const char *tok, const char *word) {
    return tok != NULL && strcmp(tok, word) == 0;
}

/* For arguments a truncated command may be missing. */
static const char *next_or(char **cursor, const char *fallback) {
    const char *tok = next_token(cursor);
    return tok ? tok : fallback;
}

char *move_to_str(Move m, bool chess960, char *buf) {
    if (m == MOVE_NONE || m == MOVE_NULL) {
        strcpy(buf, "0000");
        return buf;
    }

    const Square from = from_sq(m);
    Square to         = to_sq(m);

    /* Stored king-takes-rook; standard notation wants the king's destination. */
    if (type_of_move(m) == MT_CASTLING && !chess960)
        to = make_square(to > from ? FILE_G : FILE_C, rank_of(from));

    int n    = 0;
    buf[n++] = (char)('a' + file_of(from));
    buf[n++] = (char)('1' + rank_of(from));
    buf[n++] = (char)('a' + file_of(to));
    buf[n++] = (char)('1' + rank_of(to));

    if (type_of_move(m) == MT_PROMOTION)
        buf[n++] = " pnbrqk"[promotion_type(m)];

    buf[n] = '\0';
    return buf;
}

/* Matched against the legal moves, which also rejects illegal input. */
static Move move_from_str(const Position *pos, const char *str) {
    ScoredMove moves[MAX_MOVES];
    char buf[8];

    const int count = movegen_generate(pos, GEN_ALL, moves);
    for (int i = 0; i < count; ++i) {
        if (!movegen_is_legal(pos, moves[i].m))
            continue;
        if (strcmp(str, move_to_str(moves[i].m, pos->chess960, buf)) == 0)
            return moves[i].m;
    }
    return MOVE_NONE;
}

void uci_print_bestmove(Move best, Move ponder) {
    char b[8], p[8];

    if (is_ok_move(ponder))
        printf("bestmove %s ponder %s\n", move_to_str(best, Pos.chess960, b),
               move_to_str(ponder, Pos.chess960, p));
    else
        printf("bestmove %s\n", move_to_str(best, Pos.chess960, b));

    fflush(stdout);
}

static void cmd_uci(void) {
    printf("id name %s %s\n", ENGINE_NAME, ENGINE_VERSION);
    printf("id author %s\n", ENGINE_AUTHOR);

    printf("option name Hash type spin default %d min %d max %d\n", OPT_HASH_DEFAULT, OPT_HASH_MIN,
           OPT_HASH_MAX);
    /* Hash and Threads are required by OpenBench. */
    printf("option name Threads type spin default 1 min 1 max %d\n", SEARCH_MAX_THREADS);
    printf("option name Ponder type check default false\n");
    printf("option name Move Overhead type spin default %d min %d max %d\n", OPT_OVERHEAD_DEFAULT,
           OPT_OVERHEAD_MIN, OPT_OVERHEAD_MAX);
    printf("option name UCI_Chess960 type check default false\n");

    /* Off by default, so bench never depends on which tables a machine has. */
    printf("option name SyzygyPath type string default <empty>\n");
#ifdef EVAL_NNUE
    printf("option name EvalFile type string default <internal>\n");
    printf("option name UncertaintyHead type check default true\n");
#endif
#ifdef TUNE_SEARCH
    for (int i = 0; i < search_tunable_count(); ++i) {
        const char *name;
        int value, min, max;

        search_tunable_info(i, &name, &value, &min, &max);
        printf("option name %s type spin default %d min %d max %d\n", name, value, min, max);
    }
#endif
    printf("uciok\n");
    fflush(stdout);
}

/* Stops a running search before an option frees something it reads (TT, tables, net). */
static void end_search_for_option(const char *name) {
    if (!search_running())
        return;

    printf("info string option '%s' changed during a search; ending the search first\n", name);
    search_stop();
    search_wait();
}

/* Parses and clamps to the advertised range, reporting either problem. */
static int spin_value(const char *name, const char *value, int min, int max, int fallback) {
    char *end         = NULL;
    errno             = 0;
    const long long v = strtoll(value, &end, 10);

    if (end == value) {
        printf("info string option '%s': '%s' is not a number; keeping %d\n", name, value,
               fallback);
        return fallback;
    }
    if (errno == ERANGE || v < min || v > max) {
        const int clamped = v < min ? min : max;

        /* Echo the text, not strtoll's saturated value. */
        printf("info string option '%s': %s is outside %d..%d; using %d\n", name, value, min, max,
               clamped);
        return clamped;
    }
    return (int)v;
}

static void cmd_setoption(char *args) {
    /* Format: setoption name <name, may contain spaces> [value <value>] */
    char *name = strstr(args, "name ");
    if (!name)
        return;
    name += 5;

    char *value = strstr(name, " value ");
    if (value) {
        *value = '\0';
        value += 7;
        value = skip_spaces(value);
    }

    size_t len = strlen(name);
    while (len > 0 && (name[len - 1] == ' ' || name[len - 1] == '\t'))
        name[--len] = '\0';

    if (strcmp(name, "Hash") == 0 && value) {
        const int mb = spin_value(name, value, OPT_HASH_MIN, OPT_HASH_MAX, OptHash);
        end_search_for_option(name);
        OptHash = mb;
        if (!tt_resize((size_t)OptHash))
            printf("info string failed to allocate %d MB hash\n", OptHash);
    } else if (strcmp(name, "Threads") == 0 && value) {
        const int threads = spin_value(name, value, 1, SEARCH_MAX_THREADS, search_threads());

        end_search_for_option(name);
        search_set_threads(threads);

        printf("info string threads: %d (%zu MB)\n", search_threads(),
               (search_thread_bytes() * (size_t)search_threads()) / (1024 * 1024));
        if (search_threads() != threads)
            printf("info string could not allocate %d threads; using %d\n", threads,
                   search_threads());
    } else if (strcmp(name, "Ponder") == 0 && value) {
    } else if (strcmp(name, "Move Overhead") == 0 && value) {
        OptMoveOverhead =
            spin_value(name, value, OPT_OVERHEAD_MIN, OPT_OVERHEAD_MAX, OptMoveOverhead);
    } else if (strcmp(name, "SyzygyPath") == 0 && value) {
        end_search_for_option(name);

        if (*value == '\0' || strcmp(value, "<empty>") == 0) {
            syzygy_free();
            printf("info string syzygy: tablebases off\n");
        } else if (syzygy_init(value)) {
            printf("info string syzygy: %d-man tablebases at %s\n", syzygy_max_pieces(), value);
        } else {
            printf("info string syzygy: no usable tablebases at %s\n", value);
        }
#ifdef EVAL_NNUE
        /* A failed load keeps the previous net. */
    } else if (strcmp(name, "EvalFile") == 0 && value) {
        if (strcmp(value, "<internal>") == 0) {
            printf("info string EvalFile: keeping the embedded net\n");
        } else {
            end_search_for_option(name);
            if (nnue_load_file(value)) {
                /* The accumulators and every TT static eval came from the old net. */
                eval_state_clear(eval_state());
                tt_clear();
                nnue_print_info();
            }
        }
    } else if (strcmp(name, "UncertaintyHead") == 0 && value) {
        end_search_for_option(name);
        if (!nnue_set_uncertainty(strcmp(value, "true") == 0))
            printf("info string UncertaintyHead: net %.12s has no uncertainty head\n", nnue_hash());
        nnue_print_info();
#endif
        /* Seeds the position's flag, which board_set_fen carries forward. */
    } else if (strcmp(name, "UCI_Chess960") == 0 && value) {
        Pos.chess960 = strcmp(value, "true") == 0;
    } else {
#ifdef TUNE_SEARCH
        /* Matched first so the search can be stopped: setting one rewrites Reductions. */
        if (value) {
            for (int i = 0; i < search_tunable_count(); ++i) {
                const char *tname;
                int tvalue, tmin, tmax;

                search_tunable_info(i, &tname, &tvalue, &tmin, &tmax);
                if (strcmp(name, tname) != 0)
                    continue;

                end_search_for_option(name);
                search_tunable_set(name, atoi(value));
                fflush(stdout);
                return;
            }
        }
#endif
        /* A known option sent without a value lands here too; say so rather than "unknown". */
        static const char *const Known[] = {
            "Hash",     "Threads",         "Ponder", "Move Overhead", "SyzygyPath", "UCI_Chess960",
#ifdef EVAL_NNUE
            "EvalFile", "UncertaintyHead",
#endif
        };

        for (size_t i = 0; i < sizeof(Known) / sizeof(Known[0]); ++i)
            if (strcmp(name, Known[i]) == 0) {
                printf("info string option '%s': no value given\n", name);
                fflush(stdout);
                return;
            }

        printf("info string unknown option '%s'\n", name);
    }
    fflush(stdout);
}

static void cmd_position(char *args) {
    char *cursor = args;
    char *tok    = next_token(&cursor);

    if (token_is(tok, "startpos")) {
        board_set_startpos(&Pos);
        posValid = true;
        tok      = next_token(&cursor);
    } else if (token_is(tok, "fen")) {
        /* Everything up to `moves`; the clock fields may be missing. */
        char fen[FEN_MAX_LEN] = {0};
        size_t used           = 0;

        while ((tok = next_token(&cursor)) != NULL && !token_is(tok, "moves")) {
            const size_t n = strlen(tok);
            if (used + n + 2 >= sizeof(fen))
                break;
            if (used)
                fen[used++] = ' ';
            memcpy(fen + used, tok, n);
            used += n;
            fen[used] = '\0';
        }

        const char *why = "malformed";
        if (!board_set_fen_reason(&Pos, fen, &why)) {
            printf("info string invalid fen (%s): %s\n", why, fen);
            printf("info string no position is loaded; `go` will not answer until a "
                   "valid `position` command arrives\n");
            fflush(stdout);
            posValid = false;
            return;
        }
        posValid = true;

        /* A FEN can switch on Chess960 notation by itself; announce it once. */
        if (Pos.chess960 && !announcedChess960) {
            announcedChess960 = true;
            printf("info string Chess960 position detected; castling is now "
                   "reported as king-takes-rook\n");
        }
    } else {
        return;
    }

    if (!token_is(tok, "moves"))
        return;

    while ((tok = next_token(&cursor)) != NULL) {
        /* Leave MAX_PLY of history for the search. */
        if (Pos.gamePly + MAX_PLY >= MAX_GAME_PLY) {
            printf("info string game too long for the move history; ignoring the rest\n");
            fflush(stdout);
            return;
        }

        const Move m = move_from_str(&Pos, tok);

        /* Stop at the last position both sides agree on. */
        if (m == MOVE_NONE) {
            printf("info string illegal move in position command: %s\n", tok);
            fflush(stdout);
            return;
        }

        board_do_move(&Pos, m);
    }
}

static void cmd_go(char *args) {
    if (!posValid) {
        printf("info string ignoring `go`: the last `position` command was rejected\n");
        uci_print_bestmove(MOVE_NONE, MOVE_NONE);
        return;
    }

    SearchLimits limits;
    search_limits_clear(&limits);

    char *cursor = args;
    char *tok;

    while ((tok = next_token(&cursor)) != NULL) {
        /* `timeGiven` tells a zero clock from an absent one; see search.h. */
        if (token_is(tok, "wtime")) {
            limits.time[WHITE] = atoll(next_or(&cursor, "0"));
            limits.timeGiven   = true;
        } else if (token_is(tok, "btime")) {
            limits.time[BLACK] = atoll(next_or(&cursor, "0"));
            limits.timeGiven   = true;
        } else if (token_is(tok, "winc"))
            limits.inc[WHITE] = atoll(next_or(&cursor, "0"));
        else if (token_is(tok, "binc"))
            limits.inc[BLACK] = atoll(next_or(&cursor, "0"));
        else if (token_is(tok, "movestogo"))
            limits.movestogo = atoi(next_or(&cursor, "0"));
        /* Floored at 1: zero means "no limit" internally. */
        else if (token_is(tok, "depth")) {
            const int d  = atoi(next_or(&cursor, "1"));
            limits.depth = d > 0 ? (Depth)d : 1;
        } else if (token_is(tok, "nodes")) {
            const unsigned long long n = strtoull(next_or(&cursor, "1"), NULL, 10);
            limits.nodes               = n > 0 ? (uint64_t)n : 1;
        } else if (token_is(tok, "movetime")) {
            limits.movetime      = atoll(next_or(&cursor, "0"));
            limits.movetimeGiven = true;
        } else if (token_is(tok, "mate"))
            limits.mate = atoi(next_or(&cursor, "0"));
        else if (token_is(tok, "infinite"))
            limits.infinite = true;
        else if (token_is(tok, "ponder"))
            limits.ponder = true;
        else if (token_is(tok, "perft")) {
            perft_divide(&Pos, atoi(next_or(&cursor, "1")));
            fflush(stdout);
            return;
            /* The rest of the line is moves. */
        } else if (token_is(tok, "searchmoves")) {
            while ((tok = next_token(&cursor)) != NULL && limits.searchmovesCount < MAX_MOVES) {
                const Move m = move_from_str(&Pos, tok);
                if (is_ok_move(m))
                    limits.searchmoves[limits.searchmovesCount++] = m;
            }
            break;
        }
    }

    search_start(&Pos, &limits);
}

/* Non-zero once any command failed; main() returns it, so gates work in CI. */
static int ExitCode;

int uci_exit_code(void) { return ExitCode; }

static void cmd_perft(char *args) {
    char *cursor = args;
    char *tok    = next_token(&cursor);

    /* perft suite [path] [maxdepth] */
    if (token_is(tok, "suite")) {
        const char *path   = next_or(&cursor, "tests/perft/standard.epd");
        const int maxDepth = atoi(next_or(&cursor, "0"));
        if (!perft_run_suite(path, maxDepth))
            ExitCode = 1;
        fflush(stdout);
        return;
    }

    perft_divide(&Pos, tok ? atoi(tok) : 1);
    fflush(stdout);
}

#ifdef EVAL_NNUE

static void cmd_nnue(char *args) {
    char *cursor = args;
    char *tok    = next_token(&cursor);

    if (token_is(tok, "verify")) {
        char *path = next_token(&cursor);
        if (!path) {
            printf("usage: nnue verify <vectors file>\n");
            ExitCode = 1;
        } else if (nnue_verify_vectors(path) != 0) {
            ExitCode = 1;
        }
    } else if (token_is(tok, "eval")) {
        printf("nnue eval: %d cp\n", (int)nnue_evaluate(&Pos));
    } else if (!tok) {
        nnue_print_info();
    } else {
        printf("usage: nnue [verify <file> | eval]\n");
    }
    fflush(stdout);
}
#endif

#ifdef UNC_PROBE

/* `probe unc` measures what unc_scale() reads, to re-centre it for a new net. */
static void cmd_probe(char *args) {
    char *cursor = args;
    char *tok    = next_token(&cursor);

    if (token_is(tok, "unc")) {
        if (unc_probe_command(cursor) != 0)
            ExitCode = 1;
    } else if (token_is(tok, "err")) {
        if (unc_probe_err_command(cursor) != 0)
            ExitCode = 1;
    } else {
        printf("usage: probe [unc | err] [options]   (each takes -help)\n");
    }
    fflush(stdout);
}
#endif

static void cmd_chess960(char *args) {
    char *cursor = args;
    char *tok    = next_token(&cursor);

    if (token_is(tok, "selftest")) {
        if (chess960_selftest() != 0)
            ExitCode = 1;
        /* No argument prints all 960, to diff against a published table. */
    } else if (token_is(tok, "sp")) {
        char *idx = next_token(&cursor);
        if (chess960_print_startpos(idx ? atoi(idx) : -1) != 0)
            ExitCode = 1;
    } else {
        printf("usage: chess960 [selftest | sp [0-959]]\n");
    }
    fflush(stdout);
}

/* Resizes the pool and clears the TT: not for use mid-game. */
static void cmd_smp(char *args) {
    char *cursor = args;
    char *tok    = next_token(&cursor);

    if (token_is(tok, "selftest")) {
        char *n = next_token(&cursor);
        if (smp_selftest(n ? atoi(n) : 0) != 0)
            ExitCode = 1;
    } else if (!tok) {
        printf("smp: %d threads, %zu MB of per-thread state\n", search_threads(),
               (search_thread_bytes() * (size_t)search_threads()) / (1024 * 1024));
    } else {
        printf("usage: smp [selftest [max threads]]\n");
    }
    fflush(stdout);
}

/* The gates load and release their own tables, leaving SyzygyPath's alone. */
static void cmd_syzygy(char *args) {
    char *cursor = args;
    char *tok    = next_token(&cursor);

    if (token_is(tok, "verify")) {
        char *path = next_token(&cursor);
        if (!path) {
            printf("usage: syzygy verify <tablebase directory>\n");
            ExitCode = 1;
        } else if (syzygy_verify_suite(path) != 0) {
            ExitCode = 1;
        }
    } else if (token_is(tok, "manifest")) {
        char *path     = next_token(&cursor);
        char *manifest = next_token(&cursor);
        if (!path || !manifest) {
            printf("usage: syzygy manifest <tablebase directory> <manifest file>\n");
            ExitCode = 1;
        } else if (syzygy_verify_manifest(path, manifest) != 0) {
            ExitCode = 1;
        }
    } else if (!tok) {
        const int men = syzygy_max_pieces();
        if (men > 0)
            printf("syzygy: %d-man tablebases loaded\n", men);
        else
            printf("syzygy: no tablebases loaded (set SyzygyPath)\n");
    } else {
        printf("usage: syzygy [verify <dir> | manifest <dir> <file>]\n");
    }
    fflush(stdout);
}

bool uci_execute(const char *line) {
    char buf[MAX_INPUT];
    char *cursor = buf;

    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    /* PowerShell prepends a UTF-8 BOM when piping into the engine. */
    if ((unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB &&
        (unsigned char)buf[2] == 0xBF)
        cursor = buf + 3;

    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        buf[--len] = '\0';

    char *cmd = next_token(&cursor);
    if (!cmd)
        return true;

    if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "stop") == 0) {
        search_stop();
        if (strcmp(cmd, "quit") == 0) {
            search_wait();
            return false;
        }
        return true;
    }

    if (strcmp(cmd, "uci") == 0) {
        cmd_uci();
    } else if (strcmp(cmd, "isready") == 0) {
        /* Never wait on the search here (invariant 4). */
        printf("readyok\n");
        fflush(stdout);
    } else if (strcmp(cmd, "ucinewgame") == 0) {
        search_stop();
        search_wait();
        search_clear();
        board_set_startpos(&Pos);
        posValid = true;
    } else if (strcmp(cmd, "setoption") == 0) {
        cmd_setoption(cursor);
    } else if (strcmp(cmd, "position") == 0) {
        cmd_position(cursor);
    } else if (strcmp(cmd, "go") == 0) {
        cmd_go(cursor);
    } else if (strcmp(cmd, "ponderhit") == 0) {
        search_ponderhit();
    } else if (strcmp(cmd, "bench") == 0) {
        char *depth = next_token(&cursor);
        bench_run(depth ? atoi(depth) : 0);
    } else if (strcmp(cmd, "perft") == 0) {
        cmd_perft(cursor);
    } else if (strcmp(cmd, "syzygy") == 0) {
        cmd_syzygy(cursor);
    } else if (strcmp(cmd, "chess960") == 0) {
        cmd_chess960(cursor);
    } else if (strcmp(cmd, "smp") == 0) {
        cmd_smp(cursor);
    } else if (strcmp(cmd, "movepick") == 0) {
        if (token_is(next_token(&cursor), "selftest")) {
            if (movepick_selftest() != 0)
                ExitCode = 1;
        } else {
            printf("usage: movepick selftest\n");
        }
#ifdef UNC_PROBE
    } else if (strcmp(cmd, "probe") == 0) {
        cmd_probe(cursor);
#endif
    } else if (strcmp(cmd, "d") == 0) {
        board_print(&Pos);
        fflush(stdout);
    } else if (strcmp(cmd, "eval") == 0) {
        /* The classical breakdown in every build; `nnue eval` gives the net's score. */
        eval_trace(&Pos);
        fflush(stdout);
#ifdef EVAL_NNUE
    } else if (strcmp(cmd, "nnue") == 0) {
        cmd_nnue(cursor);
#endif
    } else {
        printf("info string unknown command '%s'\n", cmd);
        fflush(stdout);
    }

    return true;
}

void uci_loop(void) {
    char line[MAX_INPUT];

    board_set_startpos(&Pos);

    while (fgets(line, sizeof(line), stdin)) {
        if (!uci_execute(line))
            break;
    }

    /* Also reached on EOF, if the GUI died without `quit`. */
    search_stop();
    search_wait();
}
