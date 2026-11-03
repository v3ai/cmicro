/*
 * str_reserve(s, n): for EVERY Str satisfying str_inv (cap up to INT_MAX,
 * fully symbolic) and EVERY int n, with every allocation allowed to fail
 * (xrealloc -> oom() -> exit, which ends the path).  The doubling loop runs
 * at most 32 times (unwinding assertions on), so this is complete.
 *
 * Checks: no UB/overflow in the growth computation (signed-overflow-check
 * covers c * 2 and n + 1), the size passed to realloc equals the new cap,
 * str_inv is re-established, cap > n, len and contents are unchanged.
 */
#include "harness.h"

void harness(void) {
    Str s;
    int n = nondet_int(), k = nondet_int(), oldlen, oldcap;
    char before = 0;
    HARNESS_PLATFORM();
    nondet_str(&s, CBMC_CAP);
    oldlen = s.len;
    oldcap = s.cap;
    if (s.s && k >= 0 && k < s.len) before = s.s[k];

    CONTENT_BOUND(n);
    str_reserve(&s, n);

    /* oom() exits for n < 0 or n == INT_MAX, so here 0 <= n < INT_MAX */
    __CPROVER_assert(n >= 0 && n < INT_MAX, "returns only for a valid n");
    __CPROVER_assert(str_inv(&s), "Str invariant re-established");
    __CPROVER_assert(s.s != NULL, "buffer allocated");
    __CPROVER_assert(s.cap > n, "room for n bytes + NUL");
    __CPROVER_assert(s.cap >= oldcap, "never shrinks");
    __CPROVER_assert(s.len == oldlen, "len unchanged");
    if (k >= 0 && k < oldlen) CONTENT_ASSERT(s.s[k] == before, "contents preserved");
    str_free(&s);
}
