/*
 * main.c - startup, initialisation order and command dispatch.
 */
#include <stdio.h>
#include <string.h>

#include "bitboard.h"
#include "board.h"
#include "eval.h"
#include "nnue.h"
#include "search.h"
#include "tt.h"
#include "uci.h"
#include "zobrist.h"

int main(int argc, char **argv) {
    /*
     * Unbuffer stdout before anything is printed.
     *
     * When a GUI or match runner launches the engine, stdout is a pipe, and C
     * makes pipes FULLY buffered - output sits in the buffer instead of
     * reaching the GUI. The symptom is an engine that appears to hang during
     * the handshake or never answers `isready`, and it is the single most
     * common reason a new engine "does not work in Cute Chess". Every printf
     * in the protocol layer also flushes, but this is the belt-and-braces fix.
     */
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Initialisation order matters: attack tables underpin everything, and the
     * Zobrist keys must exist before any position is hashed. */
    bb_init();
    zobrist_init();
    eval_init();
#ifdef EVAL_NNUE
    /* Before search_init(), and fatal on failure: an engine whose evaluation
     * did not load has nothing useful to do, and finding that out at the first
     * `go` rather than at startup wastes a match. */
    nnue_init();
#endif
    search_init();
    tt_resize(16);

    if (argc > 1) {
        /* Re-join argv into a single command line and run it through exactly
         * the same dispatcher the GUI drives, so `engine bench` and the UCI
         * `bench` command can never behave differently. */
        char line[4096] = {0};
        size_t used     = 0;

        for (int i = 1; i < argc; ++i) {
            const size_t n = strlen(argv[i]);
            if (used + n + 2 >= sizeof(line))
                break;
            if (used)
                line[used++] = ' ';
            memcpy(line + used, argv[i], n);
            used += n;
            line[used] = '\0';
        }

        uci_execute(line);
        search_wait(); /* let an async command finish before we tear down */
    } else {
        uci_loop();
    }

    tt_free();
    return uci_exit_code();
}
