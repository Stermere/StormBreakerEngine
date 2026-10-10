/*
 * chess960test.c - the Chess960 checks perft cannot make: the SP numbering, FEN
 * round-trips, unambiguous move notation, and do/undo restoring everything. Seeded, so
 * every failure is reproducible.
 */
#include "chess960test.h"

#include <stdio.h>
#include <string.h>

#include "bitboard.h"
#include "board.h"
#include "movegen.h"
#include "uci.h"

/* A fixed seed, not rand(). */
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

/* Every SP index gives a valid array (by the variant's own rules), all 960 differ, and
 * SP 518 is the standard one. */
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

        if (castling_rook_square(&pos, WHITE, false) != make_square((File)rooks[0], RANK_1) ||
            castling_rook_square(&pos, WHITE, true) != make_square((File)rooks[1], RANK_1))
            fail("numbering: castling rights do not name both rooks", fens[idx], detail);
    }

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

/* No two legal moves may share a spelling: perft cannot see that. */
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

/* board_to_fen -> board_set_fen must give the same position, rooks included: the key
 * hashes rights, not which rook they name. Only live rights are compared, since a revoked
 * right's geometry is left in place. */
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

/* do/undo restores everything, derived state included, and the incremental key matches a
 * full recomputation (release builds never check it otherwise). Castling is the risk. */
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

        if (board_compute_key(pos) != pos->key) {
            fail("undo: the incremental key drifted from the computed one", fen, buf);
            return;
        }
    }
}

/* KQkq and rook-file spellings must parse alike, where X-FEN can express the position
 * (each castling rook outermost on its side). */
static void check_dual_spelling(const Position *pos, const char *fen) {
    char shredder[FEN_MAX_LEN], xfen[FEN_MAX_LEN];
    Position from_shredder, from_xfen;

    board_to_fen(pos, shredder);

    char rights[8];
    size_t n = 0;
    for (int i = 0; i < CASTLING_NB; ++i)
        if (pos->castling & (CastlingRights)(1 << i))
            rights[n++] = "KQkq"[i];
    rights[n] = '\0';

    if (n == 0)
        return;

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
                    return;
        }
    }

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

/* Random play from all 960 start positions, checking the above everywhere: a castling bug
 * belongs to a whole geometry, so every one is visited. */
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
                break;
            board_do_move(&pos, legal[next_random() % (uint64_t)count]);
        }
    }

    printf("ok    walk: %d positions from all 960 arrays - geometry, notation, FEN "
           "round-trip,\n      dual spelling and do/undo all consistent\n",
           positions);
}

/* Rights the diagram cannot back must be dropped, or gen_castling moves a missing rook. */
static void test_unbacked_rights(void) {
    static const struct {
        const char *fen;
        CastlingRights expected;
        const char *what;
    } cases[] = {

        {"4k3/8/8/8/8/8/8/2K4R w BH - 0 1", WHITE_OO, "file letter naming an empty square"},
        /* Duplicate claims on one side: the first wins, and it must be backed. */

        {"4k3/8/8/8/8/8/4K3/R6R w KQ - 0 1", NO_CASTLING, "king off the back rank"},

        {"4k3/8/8/8/8/8/8/2K4R w KQ - 0 1", WHITE_OO, "X-FEN naming a side with no rook"},

        {"4k3/8/8/8/8/8/8/RR2K3 w AB - 0 1", WHITE_OOO, "duplicate claims on one side"},

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
