/*
 * utf8_next(s, len, i): for EVERY int len, EVERY int i and EVERY content.
 * Loop-free: complete proof.
 *
 * Precondition (all callers): s points to at least max(len, 0) readable
 * bytes (callers pass a line/Str and its length; utf8_count/utf8_offset
 * pass their own (s, len)).  i is unconstrained: the function clamps
 * i < 0 to 0 and i >= len to len itself.
 */
#include "harness.h"

void harness(void) {
    int len = nondet_int(), i = nondet_int(), r;
    size_t sz = nondet_size_t();
    char *buf;
    __CPROVER_assume(sz >= (len > 0 ? (size_t)len : 0) && sz <= (size_t)INT_MAX);
    buf = h_malloc(sz);

    r = utf8_next(buf, len, i);

    if (i >= len) __CPROVER_assert(r == len, "i >= len -> len");
    else if (i < 0) __CPROVER_assert(r == 0, "i < 0 -> 0");
    else {
        int cp;
        __CPROVER_assert(r > i && r <= len, "strictly advances, stays within [0,len]");
        __CPROVER_assert(r - i == utf8_decode(buf + i, len - i, &cp), "advances by one decoded char");
    }
    free(buf);
}
