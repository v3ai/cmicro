/*
 * utf8_count(s, len) and utf8_offset(s, len, nchars), bounded:
 * len <= CBMC_LEN (run.sh: 8 bytes; every negative len too), all contents,
 * every nchars >= 0.
 * Preconditions (callers): s has max(len, 0) readable bytes; for
 * utf8_offset nchars >= 0 (commands.c:753, editor.c:151, main.c:267 all
 * pass col - 1 guarded by col > 0).  Without that assumption CBMC finds
 * nchars == INT_MIN: `nchars-- > 0` then overflows (latent, unreachable).
 * The loops run at most len times; --unwind CBMC_LEN+2 with unwinding
 * assertions.
 */
#include "harness.h"

#ifndef CBMC_LEN
#define CBMC_LEN 16
#endif

void harness(void) {
    int len = nondet_int(), nchars = nondet_int(), c, o;
    char *buf;
    __CPROVER_assume(len <= CBMC_LEN);
    buf = h_malloc(len > 0 ? (size_t)len : 0);

    c = utf8_count(buf, len);
    __CPROVER_assert(c >= 0 && c <= (len > 0 ? len : 0), "count in [0, len]");

    __CPROVER_assume(nchars >= 0); /* guaranteed by every caller */
    o = utf8_offset(buf, len, nchars);
    __CPROVER_assert(o >= 0 && o <= (len > 0 ? len : 0), "offset in [0, len]");
    if (nchars == 0) __CPROVER_assert(o == 0, "nchars == 0 -> 0");
    if (nchars >= c) __CPROVER_assert(o == (len > 0 ? len : 0), "nchars >= count -> len");
    free(buf);
}
