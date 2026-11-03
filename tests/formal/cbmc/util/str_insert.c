/*
 * str_insert(s, at, p, n): every str_inv Str (cap symbolic up to INT_MAX),
 * every int at, every int n; allocations may fail (exit path).
 *
 * Preconditions (callers: prompt.c insert() passes typed/pasted text from a
 * separate buffer; do_complete() passes a completion string; util.c's own
 * contract also allows p to point INTO s->s, which the function copies
 * first):  n <= 0, or p points to >= n readable bytes.  Two cases are
 * proved, chosen nondeterministically:
 *   (a) p is a separate heap object of nondet size >= n;
 *   (b) p = s->s + off with off + n <= len (aliasing the Str itself).
 *
 * Checks: no overflow, memmove/memcpy within bounds (also across the
 * realloc in case (b)), no relational comparison between pointers into
 * different objects (pointer-check "same object"), str_inv
 * re-established, len grows by max(n, 0), inserted bytes correct, bytes
 * before `at` preserved and bytes after it shifted.
 */
#include "harness.h"

void harness(void) {
    Str s;
    int at = nondet_int(), n = nondet_int(), k = nondet_int(), oldlen, cat;
    char *p, *own = NULL, before = 0, src = 0, after = 0;
    HARNESS_PLATFORM();
    nondet_str(&s, CBMC_CAP);
    oldlen = s.len;
    cat = at < 0 ? 0 : at > s.len ? s.len : at; /* clamped position */

    if (s.s && nondet_int()) { /* (b) aliasing */
        int off = nondet_int();
        __CPROVER_assume(off >= 0 && n > 0 && n <= s.len - off);
        p = s.s + off;
    } else { /* (a) separate object */
        size_t psz = nondet_size_t();
        __CPROVER_assume(psz >= (n > 0 ? (size_t)n : 0) && psz <= (size_t)INT_MAX);
        p = own = h_malloc(psz);
    }
    if (n > 0 && k >= 0 && k < n) src = p[k];
    if (s.s && k >= 0 && k < cat) before = s.s[k];
    if (s.s && k >= cat && k < s.len) after = s.s[k];

    CONTENT_BOUND(n);
    str_insert(&s, at, p, n);

    __CPROVER_assert(str_inv(&s), "invariant re-established");
    if (n <= 0) {
        __CPROVER_assert(s.len == oldlen, "n <= 0: no change");
    } else {
        __CPROVER_assert(s.s != NULL && s.len == oldlen + n, "len grows by n");
        if (k >= 0 && k < n) CONTENT_ASSERT(s.s[cat + k] == src, "inserted bytes");
        if (k >= 0 && k < cat) CONTENT_ASSERT(s.s[k] == before, "prefix preserved");
        if (k >= cat && k < oldlen) CONTENT_ASSERT(s.s[k + n] == after, "suffix shifted");
    }
    str_free(&s);
    free(own);
}
