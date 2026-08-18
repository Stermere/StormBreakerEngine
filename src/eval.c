/*
 * eval.c - static position evaluation.
 *
 * TODO(engine): unimplemented. See the roadmap in eval.h.
 */
#include "eval.h"

#include <stdio.h>

void eval_init(void) { /* TODO(engine): build piece-square tables and any derived constants. */ }

Value eval_evaluate(const Position *pos) {
    (void)pos;
    /* TODO(engine): start with material counting. */
    return VALUE_ZERO;
}

void eval_trace(const Position *pos) {
    (void)pos;
    printf("evaluation is not implemented yet\n");
}
