/*
 * str_puts(s, p) and str_putc(s, c).
 *
 * str_puts: every str_inv Str (cap symbolic up to INT_MAX) and every
 * NUL-terminated p of length <= CBMC_PLEN (8; strlen's loop is the only
 * bounded part).  Precondition (callers): p is a NUL-terminated string not
 * inside s->s.
 * str_putc: every str_inv Str (cap up to INT_MAX) and every char c,
 * covering both the fast path (room left) and the str_putn slow path.
 * Allocations may fail (exit path).
 */
#include "harness.h"

#ifndef CBMC_PLEN
#define CBMC_PLEN 8
#endif

void harness(void) {
    Str s;
    char *p, c = nondet_char();
    int oldlen, k = nondet_int();
    size_t pl;
    HARNESS_PLATFORM();
    nondet_str(&s, CBMC_CAP);
    p = nondet_cstr(CBMC_PLEN);
    pl = strlen(p);
    oldlen = s.len;

    if (nondet_int()) {
        str_puts(&s, p);
        __CPROVER_assert(str_inv(&s) && s.s != NULL, "str_puts: invariant");
        __CPROVER_assert(s.len == oldlen + (int)pl, "str_puts: len grows by strlen");
        if (k >= 0 && (size_t)k < pl) CONTENT_ASSERT(s.s[oldlen + k] == p[k], "str_puts: bytes copied");
    } else {
        str_putc(&s, c);
        __CPROVER_assert(str_inv(&s) && s.s != NULL, "str_putc: invariant");
        __CPROVER_assert(s.len == oldlen + 1, "str_putc: len + 1");
        CONTENT_ASSERT(s.s[oldlen] == c, "str_putc: appended c");
    }
    str_free(&s);
    free(p);
}
