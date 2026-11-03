/*
 * str_init, str_clear, str_free, str_detach: every str_inv Str (cap
 * symbolic up to INT_MAX); allocations may fail (str_detach of an empty
 * Str allocates via xstrdup -> oom() exit path).
 *
 * Checks: str_clear keeps the invariant with len 0 and an empty string;
 * str_detach returns an owned NUL-terminated heap string (freed here, so
 * --memory-leak-check proves nothing is lost or double-freed) and resets
 * the Str to the init state; str_free releases everything.
 */
#include "harness.h"

void harness(void) {
    Str s;
    char *r;
    int oldlen;
    HARNESS_PLATFORM();
    nondet_str(&s, CBMC_CAP);
    oldlen = s.len;

    if (nondet_int()) {
        str_clear(&s);
        __CPROVER_assert(str_inv(&s) && s.len == 0, "clear: empty, invariant kept");
        if (s.s) __CPROVER_assert(s.s[0] == 0, "clear: empty string");
    }
    if (nondet_int()) {
        int len = s.len, had = s.s != NULL;
        r = str_detach(&s);
        __CPROVER_assert(r != NULL, "detach: non-NULL result");
        /* the buffer is handed over as is: its NUL is the Str's own */
        if (had) __CPROVER_assert(r[len] == 0, "detach: NUL-terminated result");
        /* empty Str: xstrdup("") copies the NUL with memcpy, whose value
           the safety runs' havocking memcpy model does not keep */
        else CONTENT_ASSERT(r[0] == 0, "detach: empty string for an empty Str");
        __CPROVER_assert(__CPROVER_POINTER_OFFSET(r) == 0, "detach: start of a heap object");
        __CPROVER_assert(s.s == NULL && s.len == 0 && s.cap == 0, "detach: Str reset");
        free(r);
    }
    str_free(&s);
    __CPROVER_assert(s.s == NULL && s.len == 0 && s.cap == 0, "free: Str reset");
    (void)oldlen;
}
