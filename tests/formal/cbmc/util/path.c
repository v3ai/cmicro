/*
 * path_last_sep, path_base, path_dir, path_join, expand_home for every
 * string of length <= CBMC_SLEN (run.sh: 16; path_join: both operands), on
 * both platforms (plat_is_sep / plat_is_abs stubs mirror platform.c and
 * platform_win32.c, chosen nondeterministically).  Allocations may fail
 * (exit path).  Preconditions (callers pass real C strings): arguments are
 * non-NULL and NUL-terminated.
 * Functional: last_sep is the last separator (or NULL); base points inside
 * p after it; dir is a NUL-terminated heap string no longer than p.
 */
#include "harness.h"
#include "platform.h"

#ifndef CBMC_SLEN
#define CBMC_SLEN 6
#endif

void harness(void) {
    char *p = nondet_cstr(CBMC_SLEN), *q = nondet_cstr(CBMC_SLEN), *r;
    const char *sep, *base;
    size_t lp;
    HARNESS_PLATFORM();
    lp = strlen(p);

    sep = path_last_sep(p);
    if (sep) {
        size_t k;
        __CPROVER_assert(sep >= p && sep < p + lp && plat_is_sep((unsigned char)*sep), "last_sep: a separator in p");
        for (k = (size_t)(sep - p) + 1; k < lp; k++) __CPROVER_assert(!plat_is_sep((unsigned char)p[k]), "last_sep: the last one");
    } else {
        size_t k;
        for (k = 0; k < lp; k++) __CPROVER_assert(!plat_is_sep((unsigned char)p[k]), "last_sep: NULL only without separators");
    }

    base = path_base(p);
    __CPROVER_assert(base >= p && base <= p + lp, "base points into p");

    r = path_dir(p);
    __CPROVER_assert(r != NULL && strlen(r) <= (lp ? lp : 1), "dir: NUL-terminated, not longer");
    free(r);

    switch (nondet_int()) {
    case 0:
        r = path_join(p, q);
        __CPROVER_assert(r != NULL && __CPROVER_POINTER_OFFSET(r) == 0, "join: heap string");
        free(r);
        break;
    default:
        r = expand_home(p);
        __CPROVER_assert(r != NULL && __CPROVER_POINTER_OFFSET(r) == 0, "expand_home: heap string");
        free(r);
        break;
    }
    free(p);
    free(q);
}
