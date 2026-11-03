/*
 * xmalloc, xcalloc, xrealloc, xstrdup, xstrndup, xsprintf with every
 * allocation allowed to fail (--malloc-may-fail --malloc-fail-null): the
 * wrappers must never return NULL (they exit via oom()).
 *
 * Inputs:
 *   xmalloc/xcalloc/xrealloc: every size_t n, m (including 0, which the
 *     wrappers turn into 1); xrealloc of NULL or of a live heap object.
 *   xstrdup: every string of length <= CBMC_SLEN (8).
 *   xstrndup(s, n): every n <= INT_MAX with s readable for n bytes
 *     (precondition from all callers: they pass (size_t) of a
 *     non-negative int length of bytes that exist, e.g. actions.c
 *     xstrndup(s, len), buffer.c xstrndup(ln->s + at.x, tail_len));
 *     s need NOT be NUL-terminated.  n is fully symbolic.
 *   xsprintf: every formatted length L in [-1, INT_MAX] (vsnprintf stub).
 * Checks: never NULL, writable for the requested size, results
 * NUL-terminated at the right place, no size_t wraparound in n + 1, no
 * leaks (everything returned is freed here).
 */
#include "harness.h"

#ifndef CBMC_SLEN
#define CBMC_SLEN 8
#endif

void harness(void) {
    size_t n = nondet_size_t(), m = nondet_size_t();
    HARNESS_PLATFORM();
    switch (nondet_int()) {
    case 0: {
        char *p = xmalloc(n);
        __CPROVER_assert(p != NULL && __CPROVER_w_ok(p, n ? n : 1), "xmalloc: usable block");
        free(p);
        break;
    }
    case 1: {
        char *p = xcalloc(n, m);
        __CPROVER_assert(p != NULL, "xcalloc: never NULL");
        if (n && m) __CPROVER_assert(__CPROVER_w_ok(p, n * m), "xcalloc: usable block");
        free(p);
        break;
    }
    case 2: {
        char *old = nondet_int() ? NULL : h_malloc(m);
        char *p = xrealloc(old, n);
        __CPROVER_assert(p != NULL && __CPROVER_w_ok(p, n ? n : 1), "xrealloc: usable block");
        free(p);
        break;
    }
    case 3: {
        char *s = nondet_cstr(CBMC_SLEN);
        char *r = xstrdup(s);
        size_t l = strlen(s);
        __CPROVER_assert(r != NULL && r != s && r[l] == 0, "xstrdup: NUL-terminated copy");
        __CPROVER_assert(strlen(r) == l, "xstrdup: same length");
        free(r);
        free(s);
        break;
    }
    case 4: {
        size_t sz = nondet_size_t();
        char *s, *r;
        __CPROVER_assume(n <= (size_t)INT_MAX && sz >= n && sz <= (size_t)INT_MAX);
        s = h_malloc(sz);
        r = xstrndup(s, n);
        __CPROVER_assert(n + 1 > n, "xstrndup: n + 1 does not wrap");
        __CPROVER_assert(r != NULL && __CPROVER_OBJECT_SIZE(r) == n + 1 && r[n] == 0,
                         "xstrndup: n + 1 bytes, NUL-terminated");
        free(r);
        free(s);
        break;
    }
    default: {
        char *r = xsprintf("%d", 7);
        __CPROVER_assert(r != NULL && __CPROVER_POINTER_OFFSET(r) == 0, "xsprintf: heap string");
        __CPROVER_assert(r[0] == 0 || r[__CPROVER_OBJECT_SIZE(r) - 1] == 0, "xsprintf: NUL-terminated (empty on a late vsnprintf error)");
        free(r);
        break;
    }
    }
}
