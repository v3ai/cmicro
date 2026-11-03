/*
 * utf8_decode(s, len, &cp): memory safety + functional properties for
 * EVERY int len and EVERY byte content.  The function has no loops, so the
 * proof is complete (no bound) once the buffer size is symbolic.
 *
 * Precondition (all callers): s points to at least max(len, 0) readable
 * bytes.  E.g. buffer.c/actions.c/render.c pass (line.s + x, line.len - x)
 * with 0 <= x <= line.len; prompt.c passes (text.s + p, text.len - p).
 * len <= 0 is legal (the function returns 0 without reading).
 */
#include "harness.h"

void harness(void) {
    int len = nondet_int();
    size_t sz = nondet_size_t();
    char *buf;
    int cp = -12345, n;
    const unsigned char *u;

    __CPROVER_assume(sz >= (len > 0 ? (size_t)len : 0));
    __CPROVER_assume(sz <= (size_t)INT_MAX);
    buf = h_malloc(sz);
    u = (const unsigned char *)buf;

    n = utf8_decode(buf, len, &cp);

    /* consumed count */
    if (len <= 0) {
        __CPROVER_assert(n == 0 && cp == 0, "len<=0: returns 0, cp=0");
    } else {
        __CPROVER_assert(n >= 1 && n <= 4, "returns 1..4 bytes");
        __CPROVER_assert(n <= len, "never consumes beyond len");
        /* result is a Unicode scalar value */
        __CPROVER_assert(cp >= 0 && cp <= 0x10FFFF, "cp in range");
        __CPROVER_assert(cp < 0xD800 || cp > 0xDFFF, "cp not a surrogate");
        /* invalid input consumes exactly one byte and yields U+FFFD */
        if (n == 1 && u[0] >= 0x80) __CPROVER_assert(cp == 0xFFFD, "invalid byte -> U+FFFD");
        if (n == 1 && u[0] < 0x80) __CPROVER_assert(cp == u[0], "ASCII decodes to itself");
        /* round trip: the decoded multi-byte sequence re-encodes to exactly
           the consumed bytes (so the decoder accepts only shortest forms) */
        if (n > 1) {
            char out[4];
            int m = utf8_encode(cp, out);
            __CPROVER_assert(m == n, "re-encoding has the same length");
            __CPROVER_assert(memcmp(out, buf, (size_t)n) == 0, "re-encoding gives the same bytes");
        }
    }
    free(buf);
}
