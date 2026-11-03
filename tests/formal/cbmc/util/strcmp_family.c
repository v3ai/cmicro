/*
 * streq, strieq, starts_with, ends_with for every pair of strings of
 * length <= CBMC_SLEN (run.sh: 16) each (all byte values, incl. high bytes for
 * tolower()).  streq/strieq also accept NULL (they check); starts_with and
 * ends_with are only called with non-NULL strings (options.c, runtime.c,
 * commands.c, buffer.c pass option values / file names), so NULL is not
 * passed to them.
 * Functional: compared against straightforward reference loops.
 */
#include <ctype.h>
#include "harness.h"

#ifndef CBMC_SLEN
#define CBMC_SLEN 6
#endif

void harness(void) {
    char *a = nondet_int() ? NULL : nondet_cstr(CBMC_SLEN);
    char *b = nondet_int() ? NULL : nondet_cstr(CBMC_SLEN);
    size_t la, lb, k;
    int r, ref;
    HARNESS_PLATFORM();

    r = streq(a, b);
    __CPROVER_assert(r == (a && b && strcmp(a, b) == 0), "streq");

    r = strieq(a, b);
    if (!a || !b) __CPROVER_assert(r == 0, "strieq NULL");
    else {
        la = strlen(a);
        lb = strlen(b);
        ref = la == lb;
        for (k = 0; ref && k < la; k++)
            if (tolower((unsigned char)a[k]) != tolower((unsigned char)b[k])) ref = 0;
        __CPROVER_assert(r == ref, "strieq == reference");

        r = starts_with(a, b);
        ref = lb <= la && memcmp(a, b, lb) == 0;
        __CPROVER_assert(r == ref, "starts_with == reference");

        r = ends_with(a, b);
        ref = lb <= la && memcmp(a + la - lb, b, lb) == 0;
        __CPROVER_assert(r == ref, "ends_with == reference");
    }
    free(a);
    free(b);
}
