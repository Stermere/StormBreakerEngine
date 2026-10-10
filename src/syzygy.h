/*
 * syzygy.h - Syzygy tablebase probing. Inactive until syzygy_init() finds tables, and
 * then every probe declines, so bench does not depend on what is on disk.
 */
#ifndef SYZYGY_H
#define SYZYGY_H

#include "board.h"
#include "move.h"
#include "types.h"

/* A ';' (Windows) or ':' separated path list. False, and inactive, if nothing usable. */
bool syzygy_init(const char *path);
void syzygy_free(void);

/* 0 while inactive. */
int syzygy_max_pieces(void);

/* VALUE_NONE if not probable: inactive, too many pieces, castling rights, or a nonzero
 * halfmove clock. Plays captures on `pos` and restores it exactly. */
Value syzygy_probe_wdl(Position *pos, int ply);

typedef struct {
    Move move;
    Value value;
    int dtz; /* reported, for the differential tests */
} SyzygyRoot;

/* A move that makes progress (DTZ), and the fifty-move-aware value. Either can fail on
 * its own: MOVE_NONE with a value means every child probe declined. */
SyzygyRoot syzygy_probe_root(Position *pos);

#endif
