/* uci.h - Universal Chess Interface protocol layer. */
#ifndef UCI_H
#define UCI_H

#include "board.h"
#include "move.h"
#include "types.h"

void uci_loop(void);

/* Handles one command line, so main() can dispatch argv commands through exactly
 * the path the GUI drives. False if the command was `quit`. */
bool uci_execute(const char *line);

/* Non-zero once any executed command has failed, so `engine perft suite` works as
 * a CI gate. */
int uci_exit_code(void);

/* MOVE_NONE prints the UCI null move `0000`, which is what GUIs expect when there
 * is nothing to play. */
void uci_print_bestmove(Move best, Move ponder);

/* Only the options something outside uci.c consults are exported. */
int uci_move_overhead(void);

#endif
