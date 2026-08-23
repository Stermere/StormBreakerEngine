/*
 * uci.c - Universal Chess Interface protocol layer. Complete and functional.
 *
 * Two rules govern everything here:
 *
 *   1. NEVER BLOCK. Every command must be answered promptly, even mid-search.
 *      `stop` arriving during a think is the whole reason the search runs on a
 *      worker thread. If this loop ever waits on the search, GUIs will time
 *      the engine out and matches will be lost on protocol errors rather than
 *      chess.
 *
 *   2. NEVER TRUST INPUT. GUIs send malformed and unexpected commands. An
 *      unrecognised token is ignored, never fatal - crashing on a stray
 *      command forfeits the game.
 */
#include "uci.h"

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
#include "timeman.h"
#include "tt.h"

#define MAX_INPUT 65536 /* `position ... moves ...` grows long in a 300-move game */

static Position Pos;

/* ------------------------------------------------------------- options --- */

static int OptHash         = 16;
static int OptMoveOverhead = 10;
static bool OptChess960    = false;

int uci_move_overhead(void) { return OptMoveOverhead; }

/* ------------------------------------------------------------ tokenising -- */

static char *skip_spaces(char *s) {
    while (*s == ' ' || *s == '\t')
        ++s;
    return s;
}

/* Returns the next whitespace-delimited token and advances *cursor past it,
 * or NULL at end of input. Mutates the buffer, so callers pass a scratch copy. */
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

/* Next token, or `fallback` when the line ended early. GUIs do send truncated
 * commands, and a missing argument must not become a NULL dereference. */
static const char *next_or(char **cursor, const char *fallback) {
    const char *tok = next_token(cursor);
    return tok ? tok : fallback;
}

/* ---------------------------------------------------------- move syntax -- */

char *move_to_str(Move m, char *buf) {
    if (m == MOVE_NONE || m == MOVE_NULL) {
        strcpy(buf, "0000"); /* UCI's "no move" token */
        return buf;
    }

    const Square from = from_sq(m);
    Square to         = to_sq(m);

    /* Castling is stored internally as king-captures-own-rook so that Chess960
     * stays unambiguous. Standard chess GUIs expect the king's actual
     * destination (e1g1 / e1c1), so translate on the way out. */
    if (type_of_move(m) == MT_CASTLING && !OptChess960)
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

/*
 * Resolves "e2e4" / "e7e8q" against the moves legal in `pos`.
 *
 * Matching against generated moves rather than parsing the string into a move
 * directly is deliberate: it is the only way to know whether the move is a
 * capture, en passant, a castle or a double push, and it rejects illegal input
 * for free.
 */
static Move move_from_str(const Position *pos, const char *str) {
    ScoredMove moves[MAX_MOVES];
    char buf[8];

    const int count = movegen_generate(pos, GEN_ALL, moves);
    for (int i = 0; i < count; ++i) {
        if (!movegen_is_legal(pos, moves[i].m))
            continue;
        if (strcmp(str, move_to_str(moves[i].m, buf)) == 0)
            return moves[i].m;
    }
    return MOVE_NONE;
}

void uci_print_bestmove(Move best, Move ponder) {
    char b[8], p[8];

    if (is_ok_move(ponder))
        printf("bestmove %s ponder %s\n", move_to_str(best, b), move_to_str(ponder, p));
    else
        printf("bestmove %s\n", move_to_str(best, b));

    fflush(stdout);
}

/* ------------------------------------------------------------- handlers -- */

static void cmd_uci(void) {
    printf("id name %s %s\n", ENGINE_NAME, ENGINE_VERSION);
    printf("id author %s\n", ENGINE_AUTHOR);

    /* Hash and Threads are mandatory for OpenBench compliance. Threads is
     * pinned to max 1 until the search is actually parallel - advertising a
     * range the engine cannot honour makes test clients run games that are
     * silently single-threaded. */
    printf("option name Hash type spin default 16 min 1 max 65536\n");
    printf("option name Threads type spin default 1 min 1 max 1\n");
    printf("option name Ponder type check default false\n");
    printf("option name Move Overhead type spin default 10 min 0 max 5000\n");
    printf("option name UCI_Chess960 type check default false\n");
#ifdef EVAL_NNUE
    /* The net is embedded, so the default is not a path. Setting this swaps
     * the evaluation without a rebuild, which is what makes a candidate net
     * cheap to try before it is worth embedding. */
    printf("option name EvalFile type string default <internal>\n");
#endif
#ifdef TUNE_SEARCH
    /* A tuning build advertises every search margin, so a sweep can drive the
     * whole set through one binary rather than one build per candidate. */
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

static void cmd_setoption(char *args) {
    /* Format: setoption name <name, may contain spaces> [value <value>] */
    char *name = strstr(args, "name ");
    if (!name)
        return;
    name += 5;

    char *value = strstr(name, " value ");
    if (value) {
        *value = '\0'; /* terminate the name */
        value += 7;
        value = skip_spaces(value);
    }

    /* Trim trailing whitespace from the name. */
    size_t len = strlen(name);
    while (len > 0 && (name[len - 1] == ' ' || name[len - 1] == '\t'))
        name[--len] = '\0';

    if (strcmp(name, "Hash") == 0 && value) {
        OptHash = atoi(value);
        if (OptHash < 1)
            OptHash = 1;
        if (!tt_resize((size_t)OptHash))
            printf("info string failed to allocate %d MB hash\n", OptHash);
    } else if (strcmp(name, "Threads") == 0 && value) {
        const int threads = atoi(value);
        if (threads != 1)
            printf("info string only 1 thread is supported; ignoring Threads=%d\n", threads);
    } else if (strcmp(name, "Ponder") == 0 && value) {
        /* Accepted and ignored on purpose. The option exists so that a GUI
         * will send `go ponder`, and it is that command the search acts on -
         * there is no separate state for this flag to hold. */
    } else if (strcmp(name, "Move Overhead") == 0 && value) {
        OptMoveOverhead = atoi(value);
#ifdef EVAL_NNUE
    } else if (strcmp(name, "EvalFile") == 0 && value) {
        if (strcmp(value, "<internal>") == 0)
            printf("info string EvalFile: keeping the embedded net\n");
        else if (nnue_load_file(value))
            nnue_print_info();
            /* A failed load leaves the previous net in place and says why: a typo
             * in a GUI config must not leave the engine with no evaluation. */
#endif
    } else if (strcmp(name, "UCI_Chess960") == 0 && value) {
        OptChess960  = strcmp(value, "true") == 0;
        Pos.chess960 = OptChess960;
    } else {
#ifdef TUNE_SEARCH
        if (value && search_tunable_set(name, atoi(value))) {
            fflush(stdout);
            return;
        }
#endif
        printf("info string unknown option '%s'\n", name);
    }
    fflush(stdout);
}

static void cmd_position(char *args) {
    char *cursor = args;
    char *tok    = next_token(&cursor);

    if (token_is(tok, "startpos")) {
        board_set_startpos(&Pos);
        tok = next_token(&cursor);
    } else if (token_is(tok, "fen")) {
        /* Reassemble the FEN: it is six space-separated fields, and the
         * optional clock fields may be missing, so scan up to `moves`. */
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

        if (!board_set_fen(&Pos, fen)) {
            printf("info string invalid fen: %s\n", fen);
            fflush(stdout);
            return;
        }
    } else {
        return; /* neither `startpos` nor `fen`: ignore */
    }

    if (!token_is(tok, "moves"))
        return;

    while ((tok = next_token(&cursor)) != NULL) {
        /* The search pushes up to MAX_PLY more Undo records onto the same
         * array, so the game itself has to stop short of the end by that much.
         * MAX_GAME_PLY is sized so no real game reaches this; saying so and
         * stopping still beats writing past the end of Position if one does. */
        if (Pos.gamePly + MAX_PLY >= MAX_GAME_PLY) {
            printf("info string game too long for the move history; ignoring the rest\n");
            fflush(stdout);
            return;
        }

        const Move m = move_from_str(&Pos, tok);

        /* An unresolvable move means the GUI and the engine disagree about the
         * position. Stopping here leaves the board at the last state both
         * agreed on, which is far safer than guessing. */
        if (m == MOVE_NONE) {
            printf("info string illegal move in position command: %s\n", tok);
            fflush(stdout);
            return;
        }

        board_do_move(&Pos, m);
    }
}

static void cmd_go(char *args) {
    SearchLimits limits;
    search_limits_clear(&limits);

    char *cursor = args;
    char *tok;

    while ((tok = next_token(&cursor)) != NULL) {
        if (token_is(tok, "wtime"))
            limits.time[WHITE] = atoll(next_or(&cursor, "0"));
        else if (token_is(tok, "btime"))
            limits.time[BLACK] = atoll(next_or(&cursor, "0"));
        else if (token_is(tok, "winc"))
            limits.inc[WHITE] = atoll(next_or(&cursor, "0"));
        else if (token_is(tok, "binc"))
            limits.inc[BLACK] = atoll(next_or(&cursor, "0"));
        else if (token_is(tok, "movestogo"))
            limits.movestogo = atoi(next_or(&cursor, "0"));
        else if (token_is(tok, "depth"))
            limits.depth = atoi(next_or(&cursor, "0"));
        else if (token_is(tok, "nodes"))
            limits.nodes = strtoull(next_or(&cursor, "0"), NULL, 10);
        else if (token_is(tok, "movetime"))
            limits.movetime = atoll(next_or(&cursor, "0"));
        else if (token_is(tok, "mate"))
            limits.mate = atoi(next_or(&cursor, "0"));
        else if (token_is(tok, "infinite"))
            limits.infinite = true;
        else if (token_is(tok, "ponder"))
            limits.ponder = true;
        else if (token_is(tok, "perft")) {
            /* `go perft N` - the divide output GUIs and Stockfish both use. */
            perft_divide(&Pos, atoi(next_or(&cursor, "1")));
            fflush(stdout);
            return;
        } else if (token_is(tok, "searchmoves")) {
            /* Consumes the rest of the line: every remaining token is a move. */
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

/* Non-zero once any command has reported failure. main() returns this, so
 * `engine perft suite` can be used directly as a CI gate. */
static int ExitCode;

int uci_exit_code(void) { return ExitCode; }

static void cmd_perft(char *args) {
    char *cursor = args;
    char *tok    = next_token(&cursor);

    /* perft suite [path] [maxdepth] - maxdepth caps how deep each position is
     * taken, so CI can run the same file the release check does, just faster. */
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
/*
 * `nnue verify <file>` is the Task 3 acceptance gate. It runs the shipped
 * inference vectors through the net and compares the output to the expected values.
 */
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

/* ---------------------------------------------------------- dispatcher --- */

bool uci_execute(const char *line) {
    char buf[MAX_INPUT];
    char *cursor = buf;

    /* Work on a mutable copy: tokenising writes NULs into the string. */
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    /* Skip a UTF-8 BOM. No GUI sends one, but PowerShell prepends it when you
     * pipe a string into the engine by hand, and the resulting "unknown
     * command 'uci'" is baffling enough to be worth three lines to avoid. */
    if ((unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB &&
        (unsigned char)buf[2] == 0xBF)
        cursor = buf + 3;

    /* Strip the trailing newline GUIs send. */
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        buf[--len] = '\0';

    char *cmd = next_token(&cursor);
    if (!cmd)
        return true; /* blank line */

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
        /* Must be answerable at any time, including mid-search. Do NOT wait on
         * the search here - that is exactly the deadlock rule 1 warns about. */
        printf("readyok\n");
        fflush(stdout);
    } else if (strcmp(cmd, "ucinewgame") == 0) {
        search_stop();
        search_wait();
        search_clear();
        board_set_startpos(&Pos);
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
    } else if (strcmp(cmd, "d") == 0) {
        board_print(&Pos);
        fflush(stdout);
    } else if (strcmp(cmd, "eval") == 0) {
        /* Always the classical breakdown: it is the only evaluation with terms
         * to name. An NNUE build answers `nnue eval` as well. */
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

    /* Reached on EOF too: a GUI that dies without sending `quit` must not
     * leave a detached search thread spinning. */
    search_stop();
    search_wait();
}
