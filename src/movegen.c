/*
 * movegen.c - move generation.
 *
 * TODO(engine): this entire module is unimplemented. It is the correct place
 * to start building the engine.
 *
 * Suggested order, verifying with `make perft` after each step:
 *   1. knight and king moves            (leaper tables in bitboard.h)
 *   2. sliding pieces                   (bishop_attacks / rook_attacks)
 *   3. pawn pushes, double pushes, captures, promotions
 *   4. en passant   - the classic perft failure; watch the discovered check
 *                     where capturing pawn and captured pawn sit on the same
 *                     rank as your own king
 *   5. castling     - verify the king does not start, pass through, or land on
 *                     an attacked square, and that the path is empty
 *
 * Perft numbers that disagree with the suites in tests/perft mean a bug here
 * or in board_do_move. Fix it before writing any search: every later
 * measurement rests on move generation being exactly right.
 */
#include "movegen.h"

int movegen_generate(const Position *pos, GenType type, ScoredMove *list) {
    (void)pos;
    (void)type;
    (void)list;
    return 0;
}

bool movegen_is_legal(const Position *pos, Move m) {
    (void)pos;
    (void)m;
    return false;
}

bool movegen_is_pseudo_legal(const Position *pos, Move m) {
    (void)pos;
    (void)m;
    return false;
}
