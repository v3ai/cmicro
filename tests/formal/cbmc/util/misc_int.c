/*
 * Integer helpers for EVERY int input (complete; digits' loop runs <= 10
 * times, unwinding assertions on): is_word_char, is_space_char, clampi,
 * mini, maxi, digits.
 */
#include "harness.h"

void harness(void) {
    int a = nondet_int(), b = nondet_int(), c = nondet_int(), r;

    r = is_word_char(a);
    __CPROVER_assert(r == 0 || r == 1, "is_word_char is boolean");
    if (a < 0) __CPROVER_assert(r == 0, "negative is not a word char");
    r = is_space_char(a);
    __CPROVER_assert(r == (a == ' ' || a == '\t'), "is_space_char");

    r = mini(a, b);
    __CPROVER_assert(r <= a && r <= b && (r == a || r == b), "mini");
    r = maxi(a, b);
    __CPROVER_assert(r >= a && r >= b && (r == a || r == b), "maxi");
    if (b <= c) { /* every caller passes lo <= hi */
        r = clampi(a, b, c);
        __CPROVER_assert(r >= b && r <= c, "clampi within [lo,hi]");
        if (a >= b && a <= c) __CPROVER_assert(r == a, "clampi identity inside");
    }

    r = digits(a);
    __CPROVER_assert(r >= 1 && r <= 10, "digits in 1..10");
    if (a >= 0) {
        long long p = 1;
        int k;
        for (k = 1; k < r; k++) p *= 10;
        __CPROVER_assert((r == 1 || a >= p) && (long long)a < p * 10, "digits is the decimal length");
    }
}
