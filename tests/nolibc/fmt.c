#include <stdio.h>
#include <stdlib.h>
int main(void) {
    double vals[] = {0, -0.0, 1, -1, 0.5, 1.5, 2.5, 2.25, 0.125, 0.1, 0.05, 0.005, 0.0049999, 9.5, 99.5, 999.9996, 1e-5, 1.23456789e-7,
                     123456.789, 1e15, 1e16, 1.7e17, 9.999999e22, 1e100, 3.14159265358979, 1.0/3, 2.0/3, 1e300, 5e-324, 1.5e-310, 12345678901234567890.0,
                     0.000123456, 100, 1e6, 1e-4, 9.9999e-5, 0.95, 0.995, 0.9995, 1048576.0/3};
    const char *fm[] = {"%f", "%.0f", "%.1f", "%.2f", "%.3f", "%.10f", "%e", "%.0e", "%.2e", "%E", "%g", "%.0g", "%.1g", "%.3g", "%.10g", "%.17g", "%G", "%#g", "%#.0f", "%10.2f", "%-10.2f|", "%+.2f", "% .2f", "%010.3f", "%.1fKB"};
    unsigned i, j;
    for (i = 0; i < sizeof vals / sizeof *vals; i++) {
        for (j = 0; j < sizeof fm / sizeof *fm; j++) { printf(fm[j], vals[i]); putchar(' '); }
        putchar('\n');
    }
    { const char *ns[] = {"0", "42", "-17", "3.5", "1e3", "2.5e-3", "  +7.25x", ".5", "1e309", "-1e-400", "abc", "0x1A", "0x1.8p3", "0x", "007", "123456789012345678901234", "-0.000001", "1.7976931348623157e308", "4.9e-324", "0.1", "3.141592653589793238", "1e-5", "inf", "-Infinity", "nanx", "1.", "e5", "1e", "1e+", "99999999999999999999999999.5"};
      for (i = 0; i < sizeof ns / sizeof ns[0]; i++) { char *e1, *e2, *e3; double d = strtod(ns[i], &e1); long l = strtol(ns[i], &e2, 0); unsigned long u = strtoul(ns[i], &e3, 10);
        printf("strtod(%s)=%.17g end+%d strtol=%ld end+%d strtoul=%lu end+%d\n", ns[i], d, (int)(e1 - ns[i]), l, (int)(e2 - ns[i]), u, (int)(e3 - ns[i])); } }
    { const char *ls[] = {"9223372036854775807", "9223372036854775808", "-9223372036854775808", "-9223372036854775809", "18446744073709551615", "18446744073709551616", "-1", "0x7fffffffffffffff", "0777", "z"};
      for (i = 0; i < sizeof ls / sizeof ls[0]; i++) printf("strtol(%s)=%ld strtoul=%lu\n", ls[i], strtol(ls[i], NULL, 0), strtoul(ls[i], NULL, 0)); }
    return 0;
}
