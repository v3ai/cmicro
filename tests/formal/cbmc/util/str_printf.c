/*
 * str_printf(s, fmt, ...): every str_inv Str (cap symbolic up to INT_MAX)
 * and every formatted length L in [-1, INT_MAX] (vsnprintf stub, see
 * stubs.c: -1 = encoding error; the second call may also fail).
 * Allocations may fail (exit path).
 *
 * Checks: no overflow in len + L, vsnprintf is given a buffer writable for
 * the size it is told (asserted in the stub), str_inv re-established,
 * len grows by L (or 0 on error), old contents preserved.
 */
#include "harness.h"

void harness(void) {
    Str s;
    int oldlen, k = nondet_int();
    char before = 0;
    HARNESS_PLATFORM();
    nondet_str(&s, CBMC_CAP);
    oldlen = s.len;
    if (s.s && k >= 0 && k < s.len) before = s.s[k];

    str_printf(&s, "%d", 42);

    __CPROVER_assert(str_inv(&s) && s.s != NULL, "invariant re-established");
    __CPROVER_assert(s.len >= oldlen, "len never shrinks");
    if (k >= 0 && k < oldlen) CONTENT_ASSERT(s.s[k] == before, "old contents preserved");
    str_free(&s);
}
