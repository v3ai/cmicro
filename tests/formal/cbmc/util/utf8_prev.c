/*
 * utf8_prev(s, i): for EVERY int i and EVERY content.  The only loop runs
 * at most 3 times (j > i - 4), so with --unwind 5 and unwinding assertions
 * the proof is complete for all inputs.
 *
 * Precondition (callers in actions.c, buffer.c, pane.c, prompt.c pass a
 * line/Str buffer and a cursor x with 0 <= x <= len, and most guard
 * x > 0): s points to at least max(i, 0) readable bytes.  i <= 0 returns 0
 * without reading.  The object is NOT assumed to extend past s[i-1].
 */
#include "harness.h"

void harness(void) {
    int i = nondet_int(), r;
    size_t sz = nondet_size_t();
    char *buf;
    __CPROVER_assume(sz >= (i > 0 ? (size_t)i : 0) && sz <= (size_t)INT_MAX);
    buf = h_malloc(sz);

    r = utf8_prev(buf, i);

    if (i <= 0) __CPROVER_assert(r == 0, "i <= 0 -> 0");
    else {
        int cp;
        __CPROVER_assert(r < i && r >= 0 && r >= i - 4, "steps back 1..4 bytes, stays >= 0");
        /* either one (invalid) byte, or exactly one character that
           utf8_decode decodes from r and that ends at i */
        __CPROVER_assert(r == i - 1 || r + utf8_decode(buf + r, i - r, &cp) == i,
                         "steps back over exactly one decodable char");
    }
    free(buf);
}
