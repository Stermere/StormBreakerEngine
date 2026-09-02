/*
 * chess960test.c - the Chess960 checks that perft cannot make.
 *
 * Perft is the right tool for the RULES, and tests/perft/chess960.epd plus
 * tests/perft/chess960-startpos.epd cover those against an independent engine.
 * But a node count is blind to four things that would each be a real bug:
 *
 *   - The SP NUMBERING. Perft proves the position it was given is played
 *     correctly; it cannot know the position was the one asked for. An
 *     off-by-one in the numbering makes every SP index name its neighbour, and
 *     every perft count in the suite would still be right, because the suite
 *     was sealed from the same wrong FENs. Only an external anchor catches it,
 *     and SP 518 being the standard array is that anchor.
 *
 *   - FEN ROUND-TRIPS. board_to_fen has to be able to describe what
 *     board_set_fen built, or the engine cannot report its own position - and
 *     KQkq provably cannot describe some Chess960 boards. A suite that only
 *     ever reads FENs never exercises the writer.
 *
 *   - NOTATION AMBIGUITY. On a board where the king starts on b1, castling
 *     long spells "b1c1" in standard notation, and so does an ordinary king
 *     step. Perft counts both and is perfectly happy. A GUI is not: it plays
 *     the wrong move. The invariant is that no two legal moves in a position
 *     ever share a spelling, and it has to be asserted directly.
 *
 *   - UNDO. Perft's counts come out right if do_move and undo_move are wrong
 *     in exactly opposite ways, which castling makes easy: it moves two pieces
 *     and, in Chess960, they can swap squares. Comparing the whole position
 *     before and after is the only way to see it.
 *
 * Everything here is a pure function of a seed, so a failure is reproducible
 * from the line it printed.
 */
#include "chess960test.h"

#include <stdio.h>
#include <string.h>

#include "bitboard.h"
#include "board.h"
#include "movegen.h"
#include "uci.h"

/* Deterministic, self-contained, and NOT rand(): invariant 1 forbids entropy
 * anywhere near the engine, and a test that walked a different tree each run
 * would report failures nobody could reproduce. */
static uint64_t Rng = 0x9E3779B97F4A7C15ULL;

static uint64_t next_random(void) {
    Rng ^= Rng << 13;
    Rng ^= Rng >> 7;
    Rng ^= Rng << 17;
    return Rng;
}

static int Failures;

static void fail(const char *what, const char *fen, const char *detail) {
    printf("FAIL  %s\n      fen    %s\n      detail %s\n", what, fen, detail);
    ++Failures;
}

/* ------------------------------------------------------------- numbering -- */

/*
 * Every SP index must produce a legal Chess960 array, all 960 must differ, and
 * SP 518 must be the standard one.
 *
 * The three constraints checked per position are the definition of the
 * variant, not a paraphrase of the generator: bishops on opposite colours, the
 * king strictly between its rooks, and the same eight pieces as normal chess.
 * A generator that satisfies all three for 960 distinct arrays has enumerated
 * exactly the Chess960 set, because there are exactly 960 such arrays.
 */
static void test_numbering(void) {
    char fens[960][FEN_MAX_LEN];
    Position pos;

    for (int idx = 0; idx < 960; ++idx) {
        memset(&pos, 0, sizeof(pos));

        if (!board_set_chess960_start(&pos, idx)) {
            char detail[64];
            snprintf(detail, sizeof(detail), "SP %d was rejected", idx);
            fail("numbering: valid index refused", "-", detail);
            return;
        }
        board_to_fen(&pos, fens[idx]);

        int rooks[2] = {-1, -1}, bishops[2] = {-1, -1}, king = -1, nr = 0, nb = 0;
        int counts[PIECE_TYPE_NB] = {0};

        for (File f = FILE_A; f <= FILE_H; ++f) {
            const Piece pc = piece_on(&pos, make_square(f, RANK_1));
            ++counts[type_of(pc)];
            if (type_of(pc) == ROOK && nr < 2)
                rooks[nr++] = f;
            else if (type_of(pc) == BISHOP && nb < 2)
                bishops[nb++] = f;
            else if (type_of(pc) == KING)
                king = f;
        }

        char detail[96];
        snprintf(detail, sizeof(detail), "SP %d", idx);

        if (counts[ROOK] != 2 || counts[BISHOP] != 2 || counts[KNIGHT] != 2 || counts[QUEEN] != 1 ||
            counts[KING] != 1)
            fail("numbering: wrong piece multiset", fens[idx], detail);
        else if (bishops[0] % 2 == bishops[1] % 2)
            fail("numbering: bishops on the same colour", fens[idx], detail);
        else if (!(rooks[0] < king && king < rooks[1]))
            fail("numbering: king is not between its rooks", fens[idx], detail);

        /* The two rooks must be the two castling rooks, or a right names a
         * piece that cannot serve it. */
        if (castling_rook_square(&pos, WHITE, false) != make_square((File)rooks[0], RANK_1) ||
            castling_rook_square(&pos, WHITE, true) != make_square((File)rooks[1], RANK_1))
            fail("numbering: castling rights do not name both rooks", fens[idx], detail);
    }

    /* The anchor. Without it every check above passes for a numbering that is
     * internally consistent and shifted by one. */
    Position standard;
    memset(&standard, 0, sizeof(standard));
    board_set_startpos(&standard);
    memset(&pos, 0, sizeof(pos));
    board_set_chess960_start(&pos, 518);

    if (pos.key != standard.key)
        fail("numbering: SP 518 is not the standard array", fens[518],
             "SP 518 must be rnbqkbnr/... - the numbering is off");

    for (int a = 0; a < 960; ++a)
        for (int b = a + 1; b < 960; ++b)
            if (strcmp(fens[a], fens[b]) == 0) {
                char detail[64];
                snprintf(detail, sizeof(detail), "SP %d and SP %d are identical", a, b);
                fail("numbering: duplicate array", fens[a], detail);
                return;
            }

    printf("ok    numbering: 960 distinct legal arrays, SP 518 == the standard array\n");
}

/* --------------------------------------------------------- per-position -- */

/* The geometry the position claims must match the pieces actually on it. */
static void check_geometry(const Position *pos, const char *fen) {
    for (Color c = WHITE; c <= BLACK; ++c) {
        for (int side = 0; side < 2; ++side) {
            const int idx       = castling_index(c, side == 0);
            const Square rook   = pos->castlingRook[idx];
            const bool hasRight = (pos->castling & castling_right(c, side == 0)) != 0;

            if (!hasRight)
                continue;

            char detail[96];
            snprintf(detail, sizeof(detail), "%s %s right", c == WHITE ? "white" : "black",
                     side == 0 ? "kingside" : "queenside");

            if (rook == SQ_NONE || piece_on(pos, rook) != make_piece(c, ROOK))
                fail("geometry: right without a rook on its square", fen, detail);
            else if ((rook > king_square(pos, c)) != (side == 0))
                fail("geometry: rook is on the wrong side of the king", fen, detail);
            else if (!(pos->castlingKingPath[idx] & square_bb(king_square(pos, c))))
                fail("geometry: king path omits the king's own square", fen, detail);
            else if (pos->castlingEmptyPath[idx] &
                     (square_bb(king_square(pos, c)) | square_bb(rook)))
                fail("geometry: empty path includes a square the castle vacates", fen, detail);
        }
    }
}

/*
 * No two legal moves may share a spelling.
 *
 * This is the reason Chess960 notation exists, and the only test that would
 * catch move_to_str reverting to king-destination spelling on a 960 board:
 * every perft count stays correct, and the engine starts handing GUIs a string
 * that names two different moves.
 */
static void check_notation(const Position *pos, const char *fen) {
    ScoredMove moves[MAX_MOVES];
    char spellings[MAX_MOVES][8];
    const int n = movegen_generate(pos, GEN_ALL, moves);
    int legal   = 0;

    for (int i = 0; i < n; ++i)
        if (movegen_is_legal(pos, moves[i].m))
            move_to_str(moves[i].m, pos->chess960, spellings[legal++]);

    for (int a = 0; a < legal; ++a)
        for (int b = a + 1; b < legal; ++b)
            if (strcmp(spellings[a], spellings[b]) == 0) {
                char detail[64];
                snprintf(detail, sizeof(detail), "'%.7s' names two different legal moves",
                         spellings[a]);
                fail("notation: ambiguous move spelling", fen, detail);
                return;
            }
}

/*
 * board_to_fen -> board_set_fen must return the same position.
 *
 * Comparing keys is not enough: the Zobrist key hashes the castling RIGHTS,
 * not the squares they refer to, so a writer that lost track of which rook a
 * right belongs to round-trips with an identical key and a different board.
 * The rook squares are compared directly for exactly that reason.
 *
 * Only for rights that still EXIST, though. Position keeps the geometry of a
 * revoked right rather than paying to clear it on the hot path - board.h says
 * so, and nothing reads it, because every consultation is gated on the right
 * first. A round-trip therefore legitimately produces SQ_NONE where a walked
 * position still holds the square a rook left ten moves ago. Comparing those
 * would be asserting an invariant the engine deliberately does not maintain.
 */
static void check_fen_roundtrip(const Position *pos, const char *fen) {
    char written[FEN_MAX_LEN], rewritten[FEN_MAX_LEN];
    Position reparsed;

    board_to_fen(pos, written);
    memset(&reparsed, 0, sizeof(reparsed));
    reparsed.chess960 = pos->chess960;

    if (!board_set_fen(&reparsed, written)) {
        fail("fen: the engine cannot read back its own FEN", fen, written);
        return;
    }

    board_to_fen(&reparsed, rewritten);
    if (strcmp(written, rewritten) != 0)
        fail("fen: round-trip is not stable", written, rewritten);
    else if (reparsed.key != pos->key)
        fail("fen: round-trip changed the Zobrist key", written, "keys differ");
    else if (reparsed.castling != pos->castling)
        fail("fen: round-trip changed the castling rights", written, "rights differ");
    else
        for (int i = 0; i < CASTLING_NB; ++i)
            if ((pos->castling & (CastlingRights)(1 << i)) &&
                reparsed.castlingRook[i] != pos->castlingRook[i]) {
                fail("fen: round-trip moved a castling rook", written,
                     "the rights survived but now name different rooks");
                return;
            }
}

/*
 * do_move followed by undo_move must restore the position exactly.
 *
 * Castling is where this can fail while perft still passes, because it moves
 * two pieces at once and in Chess960 they may swap squares or stay put. The
 * comparison covers the derived state too - pinned, checkers, the pawn key -
 * since those are restored from the Undo record rather than recomputed, and a
 * stale one silently changes what the search sees.
 */
static void check_do_undo(Position *pos, const char *fen) {
    ScoredMove moves[MAX_MOVES];
    const int n = movegen_generate(pos, GEN_ALL, moves);

    for (int i = 0; i < n; ++i) {
        if (!movegen_is_legal(pos, moves[i].m))
            continue;

        const Position before = *pos;
        board_do_move(pos, moves[i].m);
        board_undo_move(pos, moves[i].m);

        char buf[8];
        move_to_str(moves[i].m, pos->chess960, buf);

        if (memcmp(before.board, pos->board, sizeof(before.board)) != 0 ||
            memcmp(before.byType, pos->byType, sizeof(before.byType)) != 0 ||
            memcmp(before.byColor, pos->byColor, sizeof(before.byColor)) != 0 ||
            memcmp(before.pieceCount, pos->pieceCount, sizeof(before.pieceCount)) != 0) {
            fail("undo: the board was not restored", fen, buf);
            return;
        }
        if (before.key != pos->key || before.pawnKey != pos->pawnKey ||
            before.castling != pos->castling || before.epSquare != pos->epSquare ||
            before.halfmoveClock != pos->halfmoveClock || before.checkers != pos->checkers ||
            before.pinned != pos->pinned || before.sideToMove != pos->sideToMove) {
            fail("undo: state around the board was not restored", fen, buf);
            return;
        }

        /* The incremental key must also equal the one computed from scratch,
         * which is what release builds otherwise never check. */
        if (board_compute_key(pos) != pos->key) {
            fail("undo: the incremental key drifted from the computed one", fen, buf);
            return;
        }
    }
}

/*
 * The same board spelled KQkq and spelled with rook files must parse alike.
 *
 * Only meaningful when X-FEN can express the position at all - that is, when
 * each side's castling rook is the outermost one on its side. Where it cannot,
 * there is nothing to compare, and the Shredder spelling is the only correct
 * output; tests/perft/chess960.epd covers that case by counting it.
 */
static void check_dual_spelling(const Position *pos, const char *fen) {
    char shredder[FEN_MAX_LEN], xfen[FEN_MAX_LEN];
    Position from_shredder, from_xfen;

    board_to_fen(pos, shredder);

    /* Rebuild the same FEN with the rights spelled KQkq. */
    char rights[8];
    size_t n = 0;
    for (int i = 0; i < CASTLING_NB; ++i)
        if (pos->castling & (CastlingRights)(1 << i))
            rights[n++] = "KQkq"[i];
    rights[n] = '\0';

    if (n == 0)
        return; /* nothing to spell two ways */

    /* X-FEN names the outermost rook. If the castling rook is not the
     * outermost one on its side, the two spellings genuinely mean different
     * positions and comparing them would be asserting a falsehood. */
    for (Color c = WHITE; c <= BLACK; ++c) {
        const Rank home = c == WHITE ? RANK_1 : RANK_8;
        for (int side = 0; side < 2; ++side) {
            const Square rook = castling_rook_square(pos, c, side == 0);
            if (rook == SQ_NONE)
                continue;
            const int edge = side == 0 ? FILE_H : FILE_A;
            const int step = side == 0 ? -1 : 1;
            for (int f = edge; f != (int)file_of(rook); f += step)
                if (piece_on(pos, make_square((File)f, home)) == make_piece(c, ROOK))
                    return; /* an outer rook exists: X-FEN would name that one */
        }
    }

    /* Splice the new castling field into the written FEN. */
    const char *placement = shredder;
    const char *afterStm  = strchr(strchr(placement, ' ') + 1, ' ');
    const char *tail      = strchr(afterStm + 1, ' ');
    snprintf(xfen, sizeof(xfen), "%.*s %s%s", (int)(afterStm - placement), placement, rights, tail);

    memset(&from_shredder, 0, sizeof(from_shredder));
    memset(&from_xfen, 0, sizeof(from_xfen));
    if (!board_set_fen(&from_shredder, shredder) || !board_set_fen(&from_xfen, xfen)) {
        fail("spelling: one of the two spellings did not parse", shredder, xfen);
        return;
    }

    if (from_shredder.key != from_xfen.key || from_shredder.castling != from_xfen.castling)
        fail("spelling: KQkq and the rook files describe different positions", shredder, xfen);
    else
        for (int i = 0; i < CASTLING_NB; ++i)
            if ((from_shredder.castling & (CastlingRights)(1 << i)) &&
                from_shredder.castlingRook[i] != from_xfen.castlingRook[i]) {
                fail("spelling: the two spellings resolved to different rooks", shredder, xfen);
                return;
            }
    (void)fen;
}

/* ------------------------------------------------------------- the walk -- */

/*
 * Random legal play from every start position, checking the invariants above
 * at every position along the way.
 *
 * Every SP is visited rather than a sample, for the same reason the tablebase
 * generator enumerates material configurations rather than sampling them: a
 * castling bug is a bug for a whole GEOMETRY, and there are only 960 of them.
 * Sampling would find one in proportion to how often random play reaches it.
 */
static void test_walk(int pliesPerGame) {
    Position pos;
    char fen[FEN_MAX_LEN];
    int positions = 0;

    for (int idx = 0; idx < 960; ++idx) {
        memset(&pos, 0, sizeof(pos));
        if (!board_set_chess960_start(&pos, idx))
            continue;

        for (int ply = 0; ply < pliesPerGame; ++ply) {
            board_to_fen(&pos, fen);
            ++positions;

            check_geometry(&pos, fen);
            check_notation(&pos, fen);
            check_fen_roundtrip(&pos, fen);
            check_dual_spelling(&pos, fen);
            check_do_undo(&pos, fen);

            if (Failures)
                return;

            ScoredMove moves[MAX_MOVES];
            const int n = movegen_generate(&pos, GEN_ALL, moves);
            Move legal[MAX_MOVES];
            int count = 0;
            for (int i = 0; i < n; ++i)
                if (movegen_is_legal(&pos, moves[i].m))
                    legal[count++] = moves[i].m;

            if (count == 0)
                break; /* mate or stalemate */
            board_do_move(&pos, legal[next_random() % (uint64_t)count]);
        }
    }

    printf("ok    walk: %d positions from all 960 arrays - geometry, notation, FEN "
           "round-trip,\n      dual spelling and do/undo all consistent\n",
           positions);
}

/*
 * The rights a diagram cannot back must be dropped, not trusted.
 *
 * Chess960 widens this from a tidiness rule into a crash: a right whose rook
 * is not there makes gen_castling emit a move whose make_move lifts a piece
 * off an empty square. Position editors emit such FENs routinely.
 */
static void test_unbacked_rights(void) {
    static const struct {
        const char *fen;
        CastlingRights expected;
        const char *what;
    } cases[] = {
        /* Claims a rook on b1; there is none. */
        {"4k3/8/8/8/8/8/8/2K4R w BH - 0 1", WHITE_OO, "file letter naming an empty square"},
        /* King is not on its back rank, so no right can have survived. */
        {"4k3/8/8/8/8/8/4K3/R6R w KQ - 0 1", NO_CASTLING, "king off the back rank"},
        /* X-FEN 'Q' with no rook to the queenside of the king. */
        {"4k3/8/8/8/8/8/8/2K4R w KQ - 0 1", WHITE_OO, "X-FEN naming a side with no rook"},
        /* Two claims on the same side: not a reachable position. First wins,
         * and the important part is that the survivor is backed by a rook. */
        {"4k3/8/8/8/8/8/8/RR2K3 w AB - 0 1", WHITE_OOO, "duplicate claims on one side"},
        /* A black piece on the square a white right names. */
        {"4k3/8/8/8/8/8/8/1r2K2R w BH - 0 1", WHITE_OO, "enemy rook on the named square"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
        Position pos;
        memset(&pos, 0, sizeof(pos));

        if (!board_set_fen(&pos, cases[i].fen)) {
            fail("unbacked rights: FEN was rejected outright", cases[i].fen, cases[i].what);
            continue;
        }
        if (pos.castling != cases[i].expected) {
            char detail[128];
            snprintf(detail, sizeof(detail), "%s: kept 0x%x, expected 0x%x", cases[i].what,
                     (unsigned)pos.castling, (unsigned)cases[i].expected);
            fail("unbacked rights: an unsupportable right survived", cases[i].fen, detail);
            continue;
        }

        /* Whatever survived must be playable: generate and make every move. */
        ScoredMove moves[MAX_MOVES];
        const int n = movegen_generate(&pos, GEN_ALL, moves);
        for (int j = 0; j < n; ++j) {
            if (!movegen_is_legal(&pos, moves[j].m))
                continue;
            board_do_move(&pos, moves[j].m);
            board_undo_move(&pos, moves[j].m);
        }
    }

    if (!Failures)
        printf("ok    unbacked rights: %zu malformed castling fields dropped cleanly\n",
               sizeof(cases) / sizeof(*cases));
}

/* --------------------------------------------------------------- driver -- */

int chess960_selftest(void) {
    Failures = 0;
    Rng      = 0x9E3779B97F4A7C15ULL;

    printf("Chess960 structural self-test\n\n");

    test_numbering();
    if (!Failures)
        test_unbacked_rights();
    if (!Failures)
        test_walk(24);

    printf("\n%s\n", Failures ? "FAILED" : "all checks passed");
    return Failures;
}

int chess960_print_startpos(int idx) {
    Position pos;
    char fen[FEN_MAX_LEN];

    if (idx >= 960) {
        printf("error: SP index must be 0-959\n");
        return 1;
    }

    for (int i = idx < 0 ? 0 : idx; i < (idx < 0 ? 960 : idx + 1); ++i) {
        memset(&pos, 0, sizeof(pos));
        if (!board_set_chess960_start(&pos, i)) {
            printf("error: SP %d could not be built\n", i);
            return 1;
        }
        board_to_fen(&pos, fen);
        printf("%3d  %s\n", i, fen);
    }
    return 0;
}
