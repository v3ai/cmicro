#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned long long st = 88172645463325252ULL;
static unsigned long long rnd(void) { st ^= st << 13; st ^= st >> 7; st ^= st << 17; return st; }
int main(void) {
    const char *fm[] = {"%f", "%.0f", "%.3f", "%.17f", "%e", "%.0e", "%.5e", "%.20e", "%g", "%.1g", "%.4g", "%.17g", "%#.3g", "%.40f"};
    int i, j;
    char s[128], o[2048];
    for (i = 0; i < 40000; i++) {
        union { unsigned long long u; double d; } x;
        x.u = rnd();
        if (i % 3 == 0) { x.d = (double)(rnd() % 100000) / (double)(1 + rnd() % 1000); }
        if (i % 7 == 0) { x.d = (double)(long long)(rnd() % 2000000) / 8.0 + 0.5; }
        if (x.d != x.d) continue;
        for (j = 0; j < 14; j++) {
            if ((x.d > 1e60 || x.d < -1e60) && (fm[j][1] == '.' && fm[j][2] == '4')) continue;
            snprintf(o, sizeof o, fm[j], x.d); fputs(o, stdout); putchar(' ');
        }
        putchar('\n');
        /* round trip and random decimal strings */
        snprintf(s, sizeof s, "%.17g", x.d);
        { double y = strtod(s, NULL); if (memcmp(&y, &x.d, 8)) printf("RT FAIL %s\n", s); }
        {   int nd = 1 + (int)(rnd() % 25), k; char *p = s;
            if (rnd() & 1) *p++ = '-';
            for (k = 0; k < nd; k++) { *p++ = (char)('0' + rnd() % 10); if (k == (int)(rnd() % 30)) *p++ = '.'; }
            p += sprintf(p, "e%d", (int)(rnd() % 700) - 350);
            *p = 0;
            { double y = strtod(s, NULL); union { double d; unsigned long long u; } yy; yy.d = y; printf("%s %016llx\n", s, yy.u); }
        }
    }
    /* halfway cases */
    for (i = 0; i < 2000; i++) {
        union { unsigned long long u; double d; } x; char buf[1200];
        x.u = rnd() & 0x7fefffffffffffffULL;
        /* exact decimal of the midpoint between x and its successor */
        snprintf(buf, sizeof buf, "%.800e", x.d);
        { double y = strtod(buf, NULL); union { double d; unsigned long long u; } yy; yy.d = y; printf("H %016llx %016llx\n", x.u, yy.u); }
    }
    return 0;
}
