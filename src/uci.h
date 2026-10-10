/* uci.h - Universal Chess Interface protocol layer. */
#ifndef UCI_H
#define UCI_H

#include "board.h"
#include "move.h"
#include "types.h"

void uci_loop(void);

/* One command line. False if it was `quit`. */
bool uci_execute(const char *line);

/* Non-zero once any command has failed. */
int uci_exit_code(void);

/* MOVE_NONE prints `0000`. */
void uci_print_bestmove(Move best, Move ponder);

int uci_move_overhead(void);

#endif
