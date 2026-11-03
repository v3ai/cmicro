/*
 * str_putn(s, p, n): for EVERY str_inv Str (cap up to INT_MAX, symbolic)
 * and EVERY int n, with every allocation allowed to fail (exit path).
 *
 * Preconditions (all callers: actions.c, commands.c, conf.c, input.c,
 * render.c, screen.c, util.c):
 *   - n <= 0, or p points to >= n readable bytes;
 *   - p does not point into s->s (no caller appends a Str to itself;
 *     str_insert is the function that supports aliasing).
 * p is a separate object of nondet size >= n here; its bytes are nondet.
 *
 * Checks: no overflow in len + n, memcpy within both objects, str_inv
 * re-established, len grows by max(n, 0), old contents preserved, the new
 * bytes equal p's bytes (one nondet index each).
 */
#include "harness.h"

void harness(void) {
    Str s;
    int n = nondet_int(), k = nondet_int(), oldlen;
    size_t psz = nondet_size_t();
    char *p, before = 0;
    HARNESS_PLATFORM();
    nondet_str(&s, CBMC_CAP);
    __CPROVER_assume(psz >= (n > 0 ? (size_t)n : 0) && psz <= (size_t)INT_MAX);
    p = h_malloc(psz);
    oldlen = s.len;
    if (s.s && k >= 0 && k < s.len) before = s.s[k];

    CONTENT_BOUND(n);
    str_putn(&s, p, n);

    __CPROVER_assert(str_inv(&s) && s.s != NULL, "Str invariant re-established, buffer allocated");
    __CPROVER_assert(s.len == oldlen + (n > 0 ? n : 0), "len grows by n");
    if (k >= 0 && k < oldlen) CONTENT_ASSERT(s.s[k] == before, "old contents preserved");
    if (n > 0 && k >= 0 && k < n) CONTENT_ASSERT(s.s[oldlen + k] == p[k], "appended bytes copied");
    str_free(&s);
    free(p);
}
