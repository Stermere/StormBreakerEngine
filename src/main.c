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
#include "syzygy.h"
#include "tt.h"
#include "uci.h"
#include "zobrist.h"

int main(int argc, char **argv) {
    /*
     * When a GUI or match runner launches the engine, stdout is a pipe, and C
     * makes pipes FULLY buffered - output sits in the buffer instead of
     * reaching the GUI, so the engine appears to hang during the handshake or
     * never answers `isready`. Every printf in the protocol layer also
     * flushes; this is the belt-and-braces fix.
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

    /* A failed allocation here is survivable - tt_probe/tt_store both check for
     * a NULL table - but it is not silent: an engine playing a whole match with
     * no transposition table is drastically weaker, and that has to be visible
     * rather than showing up as an unexplained result. */
    if (!tt_resize(16))
        printf("info string failed to allocate the default 16 MB hash\n");

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

    syzygy_free();
    tt_free();
    return uci_exit_code();
}
