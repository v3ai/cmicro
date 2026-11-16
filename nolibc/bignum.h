/*
 * bignum.h - small fixed-size unsigned big integers, used by printf and
 * strtod to convert between binary and decimal floating point exactly.
 * Internal to nolibc.
 */
#ifndef NOLIBC_BIGNUM_H
#define NOLIBC_BIGNUM_H

/* 200 limbs of 32 bits = 6400 bits; the largest number needed is about
   10^1150 (strtod) or 5^1074 * 2^53 (printf), both well under 4100 bits. */
#define BN_LIMBS 200

typedef struct {
    unsigned int d[BN_LIMBS]; /* little-endian limbs */
    int n;                    /* limbs in use; 0 means the value 0 */
} bignum;

void __nl_bn_set(bignum *b, unsigned long long v);
void __nl_bn_mul_add(bignum *b, unsigned int m, unsigned int add);
void __nl_bn_mul_pow10(bignum *b, int k);
void __nl_bn_mul_pow5(bignum *b, int k);
void __nl_bn_shl(bignum *b, int k);
int __nl_bn_cmp(const bignum *a, const bignum *b);
/* Divide by 10^9 in place; returns the remainder. */
unsigned int __nl_bn_div1e9(bignum *b);
/* Decimal digits of b (destroys b); returns the count, writes no NUL. */
int __nl_bn_to_dec(bignum *b, char *out, int cap);

/* Exact decimal digits of a finite double v > 0: writes n >= 0 significant
   digits to d, rounded half-to-even, and stores the decimal exponent of the
   first digit in *e.  Returns the number of digits written: n, or 1 when
   n == 0 and the value rounds up to one unit of the first place.  With
   n < 0 only *e is computed. */
int __nl_dtoa(double v, int n, char *d, int *e);

#endif
