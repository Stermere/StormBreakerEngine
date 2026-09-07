/* main.c - startup, initialisation order and command dispatch. */
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
    /* A GUI launches the engine on a pipe, and C makes pipes fully buffered, so the
     * handshake would sit in the buffer and the engine would appear to hang. */
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Order matters: the attack tables underpin everything, and the Zobrist keys must
     * exist before any position is hashed. */
    bb_init();
    zobrist_init();
    eval_init();
    /* Before search_init(), and fatal on failure: finding out at the first `go` rather
     * than at startup wastes a match. */
#ifdef EVAL_NNUE
    nnue_init();
#endif
    search_init();

    /* A failed allocation is survivable - probe and store both check for a NULL table -
     * but an engine playing a whole match without one is drastically weaker, so it must
     * not be silent. */
    if (!tt_resize(16))
        printf("info string failed to allocate the default 16 MB hash\n");

    if (argc > 1) {
        /* Re-joined into one line and run through the dispatcher the GUI drives, so
         * `engine bench` and the UCI `bench` command cannot behave differently. */
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
        search_wait();
    } else {
        uci_loop();
    }

    /* Before tt_free(): a pooled thread that is still parked has a raw pointer to the
     * table, and the search it is parked between could in principle be woken by nothing
     * at all - but the ordering costs nothing and the reverse is a use-after-free. */
    search_exit();

    syzygy_free();
    tt_free();
    return uci_exit_code();
}
