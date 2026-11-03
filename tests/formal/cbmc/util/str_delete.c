/*
 * str_delete(s, at, n): every str_inv Str (cap symbolic up to INT_MAX),
 * every int at and n (callers in prompt.c pass selection/word ranges, but
 * the function itself rejects/clamps anything out of range, so no
 * precondition is assumed).  No allocation.
 *
 * Checks: memmove within bounds, no overflow in at + n, str_inv
 * re-established, len shrinks by the clamped count, prefix kept, suffix
 * moved down.
 */
#include "harness.h"

void harness(void) {
    Str s;
    int at = nondet_int(), n = nondet_int(), k = nondet_int(), oldlen, del;
    char b = 0;
    HARNESS_PLATFORM();
    nondet_str(&s, CBMC_CAP);
    oldlen = s.len;
    if (at < 0 || at >= s.len || n <= 0) del = 0;
    else del = n > s.len - at ? s.len - at : n;
    if (s.s && k >= 0 && k < s.len) b = s.s[k];

    str_delete(&s, at, n);

    __CPROVER_assert(str_inv(&s), "invariant re-established");
    __CPROVER_assert(s.len == oldlen - del, "len shrinks by the clamped count");
    if (del > 0 && k >= 0 && k < at) CONTENT_ASSERT(s.s[k] == b, "prefix kept");
    if (del > 0 && k >= at + del && k < oldlen) CONTENT_ASSERT(s.s[k - del] == b, "suffix moved down");
    str_free(&s);
}
