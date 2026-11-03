/*
 * char_width(cp) / in_table for EVERY int cp (complete: the binary search
 * runs <= 6 times on <= 58 entries; unwinding assertions on).
 * Bounds checks prove every table lookup is in bounds.  Functional checks:
 * the result is 0, 1 or 2 and equals a straightforward LINEAR scan of the
 * same tables (so the binary search is exact).  Entry point
 * harness_tables proves the tables are sorted, non-empty and disjoint.
 *
 * The tables are static in util.c, so util.c is #included here (not
 * linked) to reach them; the code under test is the same.
 */
#include "harness.h"
#include "../../../../src/util.c"

static int lin(int cp, const struct range *t, int n) {
    int k;
    for (k = 0; k < n; k++)
        if (cp >= t[k].lo && cp <= t[k].hi) return 1;
    return 0;
}

void harness(void) {
    int cp = nondet_int(), w, ref;
    w = char_width(cp);
    __CPROVER_assert(w == 0 || w == 1 || w == 2, "width is 0, 1 or 2");
    if (cp == 0) ref = 0;
    else if (cp < 32 || (cp >= 0x7F && cp < 0xA0)) ref = 1;
    else if (cp < 0x300) ref = 1;
    else if (lin(cp, zero_width, (int)(sizeof zero_width / sizeof zero_width[0]))) ref = 0;
    else if (lin(cp, wide, (int)(sizeof wide / sizeof wide[0]))) ref = 2;
    else ref = 1;
    __CPROVER_assert(w == ref, "binary search == linear scan");
}

void harness_tables(void) {
    int k = nondet_int();
    int nz = (int)(sizeof zero_width / sizeof zero_width[0]);
    int nw = (int)(sizeof wide / sizeof wide[0]);
    if (k >= 0 && k < nz) {
        __CPROVER_assert(zero_width[k].lo <= zero_width[k].hi, "zero_width range non-empty");
        if (k + 1 < nz) __CPROVER_assert(zero_width[k].hi < zero_width[k + 1].lo, "zero_width sorted/disjoint");
    }
    if (k >= 0 && k < nw) {
        __CPROVER_assert(wide[k].lo <= wide[k].hi, "wide range non-empty");
        if (k + 1 < nw) __CPROVER_assert(wide[k].hi < wide[k + 1].lo, "wide sorted/disjoint");
    }
}
