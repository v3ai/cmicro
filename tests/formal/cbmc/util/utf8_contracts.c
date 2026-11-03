/*
 * Function contracts for the UTF-8 leaf functions, checked with
 * goto-instrument --enforce-contract (one run per function, see run.sh).
 * A contract proof is modular and unbounded: the function is checked
 * against its requires/ensures/assigns for ALL inputs satisfying the
 * precondition, with its write set checked against __CPROVER_assigns
 * (no write outside *cp / out[0..3]).  The functions are loop-free
 * (utf8_prev's loop has a constant bound of 3), so no loop contracts are
 * needed.
 *
 * The contracts are attached to re-declarations here; util.c is #included
 * so the definitions are in the same translation unit (code unchanged).
 */
#include <limits.h>
#include <stdlib.h>

int utf8_decode(const char *str, int len, int *cp)
    __CPROVER_requires(len <= 0 || __CPROVER_is_fresh(str, len))
    __CPROVER_requires(__CPROVER_is_fresh(cp, sizeof(int)))
    __CPROVER_assigns(*cp)
    __CPROVER_ensures(len <= 0 ==> (__CPROVER_return_value == 0 && *cp == 0))
    __CPROVER_ensures(len > 0 ==> (__CPROVER_return_value >= 1 && __CPROVER_return_value <= 4 &&
                                   __CPROVER_return_value <= len))
    __CPROVER_ensures(len > 0 ==> (*cp >= 0 && *cp <= 0x10FFFF && (*cp < 0xD800 || *cp > 0xDFFF)));

int utf8_encode(int cp, char *out)
    __CPROVER_requires(__CPROVER_is_fresh(out, 4))
    __CPROVER_assigns(__CPROVER_object_upto(out, 4))
    __CPROVER_ensures(__CPROVER_return_value >= 1 && __CPROVER_return_value <= 4);

int utf8_next(const char *s, int len, int i)
    __CPROVER_requires(len <= 0 || __CPROVER_is_fresh(s, len))
    __CPROVER_assigns()
    __CPROVER_ensures(i >= len ==> __CPROVER_return_value == len)
    __CPROVER_ensures((i < len && i < 0) ==> __CPROVER_return_value == 0)
    __CPROVER_ensures((i < len && i >= 0) ==> (__CPROVER_return_value > i && __CPROVER_return_value <= len));

int utf8_prev(const char *s, int i)
    __CPROVER_requires(i <= 0 || __CPROVER_is_fresh(s, i))
    __CPROVER_assigns()
    __CPROVER_ensures(i <= 0 ==> __CPROVER_return_value == 0)
    __CPROVER_ensures(i > 0 ==> (__CPROVER_return_value < i && __CPROVER_return_value >= 0 &&
                                 __CPROVER_return_value >= i - 4));

int char_width(int cp)
    __CPROVER_assigns()
    __CPROVER_ensures(__CPROVER_return_value >= 0 && __CPROVER_return_value <= 2);

#include "../../../../src/util.c"

/* --enforce-contract F with entry point harness_F: call F with nondet
   arguments; the instrumentation assumes the requires clauses (allocating
   fresh objects) and asserts ensures/assigns. */
int nondet_int(void);
char *nondet_ptr(void);
int *nondet_iptr(void);

void harness_utf8_decode(void) {
    int len = nondet_int();
    utf8_decode(nondet_ptr(), len, nondet_iptr());
}
void harness_utf8_encode(void) { utf8_encode(nondet_int(), nondet_ptr()); }
void harness_utf8_next(void) { utf8_next(nondet_ptr(), nondet_int(), nondet_int()); }
void harness_utf8_prev(void) { utf8_prev(nondet_ptr(), nondet_int()); }
void harness_char_width(void) { char_width(nondet_int()); }
