/*
 * uci.c - Universal Chess Interface protocol layer.
 *
 * Two rules govern everything here. NEVER BLOCK: every command is answered promptly
 * even mid-search, which is why the search runs on a worker thread. NEVER TRUST INPUT:
 * an unrecognised or malformed token is ignored, never fatal, because crashing on a
 * stray command forfeits the game.
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
#include "syzygy.h"
#include "test/chess960test.h"
#include "test/historytest.h"
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

/* The advertised bounds live here rather than inside the `option` strings, so cmd_uci()
 * and cmd_setoption() cannot drift apart. A Move Overhead outside this range is not a
 * harmless oddity: timeman.c subtracts it from the clock. */
#define OPT_HASH_MIN         1
#define OPT_HASH_MAX         65536
#define OPT_HASH_DEFAULT     16
#define OPT_OVERHEAD_MIN     0
#define OPT_OVERHEAD_MAX     5000
#define OPT_OVERHEAD_DEFAULT 10

static int OptHash         = OPT_HASH_DEFAULT;
static int OptMoveOverhead = OPT_OVERHEAD_DEFAULT;

/* Purely so the notice is printed once rather than on every `position`; the notation
 * itself is decided by Pos.chess960. */
static bool announcedChess960 = false;

/*
 * False once a `position` command has been rejected, until one succeeds. A rejected FEN
 * leaves `Pos` holding the last position that parsed, and answering from it confidently
 * returns a move belonging to a different game - on black's turn that move is White's,
 * and the GUI correctly reports the engine as having crashed.
 *
 * UCI has no "I cannot" reply, so the honest answer is the null move: it says "no move
 * from me" without inventing a legal-looking one for a board nobody else has.
 */
static bool posValid = true;

int uci_move_overhead(void) { return OptMoveOverhead; }

static char *skip_spaces(char *s) {
    while (*s == ' ' || *s == '\t')
        ++s;
    return s;
}

/* The next whitespace-delimited token, or NULL at end of input. Mutates the buffer, so
 * callers pass a scratch copy. */
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

/* GUIs do send truncated commands, and a missing argument must not become a NULL
 * dereference. */
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

    /* Castling is stored king-captures-own-rook so Chess960 stays unambiguous, but
     * standard GUIs expect the king's actual destination - so translate on the way out,
     * and only where that spelling cannot collide with an ordinary king move. */
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

/* Resolves "e2e4" / "e7e8q" against the moves legal in `pos`. Matching against generated
 * moves rather than parsing the string is the only way to know whether the move is a
 * capture, en passant, a castle or a double push, and it rejects illegal input free. */
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
    /* Hash and Threads are mandatory for OpenBench compliance. The default stays 1
     * because that is the configuration every SPRT and every bench node count in
     * docs/EXPERIMENTS.md was measured in; a GUI that wants the machine has to ask. */
    printf("option name Threads type spin default 1 min 1 max %d\n", SEARCH_MAX_THREADS);
    printf("option name Ponder type check default false\n");
    printf("option name Move Overhead type spin default %d min %d max %d\n", OPT_OVERHEAD_DEFAULT,
           OPT_OVERHEAD_MIN, OPT_OVERHEAD_MAX);
    printf("option name UCI_Chess960 type check default false\n");

    /* Off until a GUI supplies a path, and `make bench` never does: with tablebases
     * loaded the node count would depend on which files the machine has. */
    printf("option name SyzygyPath type string default <empty>\n");
    /* The net is embedded, so the default is not a path. Setting this swaps the
     * evaluation without a rebuild, which is what makes a candidate net cheap to try. */
#ifdef EVAL_NNUE
    printf("option name EvalFile type string default <internal>\n");
#endif
    /* A tuning build advertises every search margin, so a sweep can drive the whole set
     * through one binary rather than one build per candidate. */
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

/* Ends a running search before an option replaces something the worker still holds a
 * raw pointer into - the transposition table, the mapped tablebases, the network blob.
 * `setoption name Hash` mid-search frees the table out from under tt_probe() and the
 * process dies; a GUI has no business doing that, but "no business" is not a memory
 * barrier. */
static void end_search_for_option(const char *name) {
    if (!search_running())
        return;

    printf("info string option '%s' changed during a search; ending the search first\n", name);
    search_stop();
    search_wait();
}

/* Clamps to the range cmd_uci() advertised, and says so. Bare atoi() reports nothing for
 * either failure mode - "abc" reads as 0 and an overlarge value saturates - so both
 * would arrive as a plausible-looking setting the GUI never asked for. */
static int spin_value(const char *name, const char *value, int min, int max, int fallback) {
    char *end         = NULL;
    const long long v = strtoll(value, &end, 10);

    if (end == value) {
        printf("info string option '%s': '%s' is not a number; keeping %d\n", name, value,
               fallback);
        return fallback;
    }
    if (v < min || v > max) {
        const int clamped = v < min ? min : max;
        printf("info string option '%s': %lld is outside %d..%d; using %d\n", name, v, min, max,
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

        /* Said out loud in both directions. A pool costs a little over 8 MB a thread, so
         * the difference between 8 and 512 is the difference between nothing and four
         * gigabytes; and if the machine could not give us what the GUI asked for, a
         * match that is quietly running at a third of the requested strength is worth
         * more to know about than it is to hide. */
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

        /* `<empty>` is what a GUI sends to clear the option it advertised. */
        if (*value == '\0' || strcmp(value, "<empty>") == 0) {
            syzygy_free();
            printf("info string syzygy: tablebases off\n");
        } else if (syzygy_init(value)) {
            printf("info string syzygy: %d-man tablebases at %s\n", syzygy_max_pieces(), value);
        } else {
            /* A path that yields no tables is worth saying out loud: probing that never
             * fires looks identical to working tablebases from the outside. */
            printf("info string syzygy: no usable tablebases at %s\n", value);
        }
#ifdef EVAL_NNUE
        /* A failed load leaves the previous net in place and says why: a typo in a GUI
         * config must not leave the engine with no evaluation. */
    } else if (strcmp(name, "EvalFile") == 0 && value) {
        if (strcmp(value, "<internal>") == 0) {
            printf("info string EvalFile: keeping the embedded net\n");
        } else {
            end_search_for_option(name);
            if (nnue_load_file(value)) {
                /* Everything cached from the previous net is now wrong: the accumulator
                 * stack was built from its weights, and every TT entry carries a static
                 * eval it produced. Without this, a net-vs-net comparison driven through
                 * this option is partly scored by the net that was replaced. */
                eval_state_clear(eval_state());
                tt_clear();
                nnue_print_info();
            }
        }
#endif
        /* Seeds the POSITION's flag, which is what every spelling decision reads.
         * board_set_fen carries it across position changes and latches it on by itself
         * for a FEN only Chess960 can describe. */
    } else if (strcmp(name, "UCI_Chess960") == 0 && value) {
        Pos.chess960 = strcmp(value, "true") == 0;
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
        posValid = true;
        tok      = next_token(&cursor);
    } else if (token_is(tok, "fen")) {
        /* Reassemble the FEN: six space-separated fields, of which the clock fields may
         * be missing, so scan up to `moves`. */
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
            /* Both lines matter: the reason is what the user can act on, and the refusal
             * is what stops the next `go` answering out of whatever position was loaded
             * before this one. */
            printf("info string invalid fen (%s): %s\n", why, fen);
            printf("info string no position is loaded; `go` will not answer until a "
                   "valid `position` command arrives\n");
            fflush(stdout);
            posValid = false;
            return;
        }
        posValid = true;

        /* board_set_fen latches this on for a FEN only a Chess960 board can produce, even if
         * the GUI never sent UCI_Chess960 - say so, because it changes how castling is
         * spelled from here on. One-way on purpose: the standard spelling is genuinely
         * ambiguous on such a board. */
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
        /* The search pushes up to MAX_PLY more Undo records onto the same array, so the
         * game has to stop short of the end by that much. MAX_GAME_PLY is sized so no
         * real game gets here, but saying so beats writing past the end of Position. */
        if (Pos.gamePly + MAX_PLY >= MAX_GAME_PLY) {
            printf("info string game too long for the move history; ignoring the rest\n");
            fflush(stdout);
            return;
        }

        const Move m = move_from_str(&Pos, tok);

        /* An unresolvable move means the GUI and the engine disagree about the position.
         * Stopping leaves the board at the last state both agreed on. */
        if (m == MOVE_NONE) {
            printf("info string illegal move in position command: %s\n", tok);
            fflush(stdout);
            return;
        }

        board_do_move(&Pos, m);
    }
}

static void cmd_go(char *args) {
    /* No position means no answer. Replying from the stale board is what turns a
     * rejected FEN into an "engine crashed" report: the move is legal somewhere, just
     * not here, and on the wrong turn it is the wrong colour entirely. */
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
        /* `go perft N` - the divide output GUIs and Stockfish both use. */
        else if (token_is(tok, "perft")) {
            perft_divide(&Pos, atoi(next_or(&cursor, "1")));
            fflush(stdout);
            return;
            /* Consumes the rest of the line: every remaining token is a move. */
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

/* Non-zero once any command has reported failure. main() returns this, so
 * `engine perft suite` works directly as a CI gate. */
static int ExitCode;

int uci_exit_code(void) { return ExitCode; }

static void cmd_perft(char *args) {
    char *cursor = args;
    char *tok    = next_token(&cursor);

    /* perft suite [path] [maxdepth] - maxdepth caps how deep each position is taken, so
     * CI can run the same file the release check does, just faster. */
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

/* `probe unc` measures the distribution unc_scale()'s constants are centred on, so a
 * retrain can be re-centred instead of quietly shifting every margin. It ships in the
 * binary for the reason the gates do: the net and the search it measures are the ones
 * this build plays with. */
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

/* `chess960 selftest` is the structural gate; test/chess960test.c says what it checks
 * that perft cannot. */
static void cmd_chess960(char *args) {
    char *cursor = args;
    char *tok    = next_token(&cursor);

    if (token_is(tok, "selftest")) {
        if (chess960_selftest() != 0)
            ExitCode = 1;
        /* No argument prints all 960, which is what you diff against a published table
         * when the numbering itself is in question. */
    } else if (token_is(tok, "sp")) {
        char *idx = next_token(&cursor);
        if (chess960_print_startpos(idx ? atoi(idx) : -1) != 0)
            ExitCode = 1;
    } else {
        printf("usage: chess960 [selftest | sp [0-959]]\n");
    }
    fflush(stdout);
}

/* `smp selftest [threads]` is the parallel-search gate; test/smptest.c says what it
 * checks that a node count cannot. It resizes the pool and clears the table, so it is
 * a developer command and not something to run in the middle of a game. */
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

/* `syzygy verify <path>` is the tablebase acceptance gate. It loads and releases its
 * own tables, so it does not disturb whatever SyzygyPath a running session had set. */
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
        /* The differential campaign's verdict, re-checked without the oracle that
         * produced it. See syzygytest.h. */
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

    /* Work on a mutable copy: tokenising writes NULs into the string. */
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    /* Skip a UTF-8 BOM. No GUI sends one, but PowerShell prepends it when a string is
     * piped into the engine by hand, and "unknown command 'uci'" is baffling. */
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
        /* Must be answerable at any time, including mid-search. Do NOT wait on the search
         * here - that is exactly the deadlock rule 1 warns about. */
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
    } else if (strcmp(cmd, "history") == 0) {
        if (token_is(next_token(&cursor), "selftest")) {
            if (history_selftest() != 0)
                ExitCode = 1;
        } else {
            printf("usage: history selftest\n");
        }
#ifdef UNC_PROBE
    } else if (strcmp(cmd, "probe") == 0) {
        cmd_probe(cursor);
#endif
    } else if (strcmp(cmd, "d") == 0) {
        board_print(&Pos);
        fflush(stdout);
    } else if (strcmp(cmd, "eval") == 0) {
        /* Always the classical breakdown: it is the only evaluation with terms to name.
         * An NNUE build answers `nnue eval` as well. */
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
            /* Reached on EOF too: a GUI that dies without sending `quit` must not leave a
             * detached search thread spinning. */
            break;
    }

    search_stop();
    search_wait();
}
