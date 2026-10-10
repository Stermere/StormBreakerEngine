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
    /* Pipes are fully buffered by default, which would stall the UCI handshake. */
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Attack tables, then Zobrist keys, before any position exists. */
    bb_init();
    zobrist_init();
    eval_init();
    /* Fatal at startup rather than at the first `go`. */
#ifdef EVAL_NNUE
    nnue_init();
#endif
    search_init();

    /* Survivable, but not silently. */
    if (!tt_resize(16))
        printf("info string failed to allocate the default 16 MB hash\n");

    if (argc > 1) {
        /* Command-line arguments go through the same dispatcher as UCI input. */
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

    /* Before tt_free(): parked threads hold pointers into the table. */
    search_exit();

    syzygy_free();
    tt_free();
    return uci_exit_code();
}
