/*
 * uci.h - Universal Chess Interface protocol layer.
 *
 * UCI is the lingua franca of computer chess: Cute Chess, En Croissant,
 * fastchess, OpenBench and every arbiter tool speak it. Getting this layer
 * exactly right is what makes the engine plug into all of them unchanged.
 */
#ifndef UCI_H
#define UCI_H

#include "board.h"
#include "move.h"
#include "types.h"

/* Reads commands from stdin until `quit` or EOF. */
void uci_loop(void);

/* Handles a single command line. Exposed so main() can dispatch argv commands
 * ("engine bench", "engine perft 5") through exactly the same code path the
 * GUI drives - one implementation, so the two can never diverge. Returns false
 * if the command was `quit`. */
bool uci_execute(const char *line);

/* Process exit status: non-zero once any executed command has failed, so
 * `engine perft suite` works as a CI gate. */
int uci_exit_code(void);

/* Emits `bestmove <move> [ponder <move>]`. Called by the search worker when it
 * finishes. Passing MOVE_NONE prints the UCI null move `0000`, which is what
 * GUIs expect when there is nothing to play. */
void uci_print_bestmove(Move best, Move ponder);

/* ------------------------------------------------------------- options --- */

/* Live values of the UCI options, read by the search and evaluation. */
bool uci_chess960(void);
int uci_move_overhead(void);
int uci_multipv(void);

#endif /* UCI_H */
