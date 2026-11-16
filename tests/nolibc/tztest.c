#include <stdio.h>
#include <time.h>
int main(void) {
    long base[] = {0, 951782400, 1700000000, 1794869581, 1710054000, 1730620800, 1741503600, 1762066800, 2000000000, 2147483647, 2200000000L, 4102444800L, 4118083200L, 4133894400L, -86400 * 365L * 30, 1301792400};
    int i, j;
    for (i = 0; i < 16; i++) for (j = -2; j <= 2; j++) {
        char out[128]; time_t t = base[i] + j * 1800; struct tm *tm = localtime(&t);
        strftime(out, sizeof out, "%Y-%m-%d %H:%M:%S %a %j", tm); printf("%ld %s dst=%d\n", (long)t, out, tm->tm_isdst); }
    /* every 6 hours over 2 years around now, and in 2045 */
    { long t0; for (t0 = 1767225600; t0 < 1767225600 + 2 * 31622400L; t0 += 6 * 3600 + 7) { time_t t = t0; struct tm *tm = localtime(&t); printf("%d%02d%02d%02d%d ", tm->tm_year, tm->tm_mon, tm->tm_mday, tm->tm_hour, tm->tm_isdst); } putchar('\n'); }
    { long t0; for (t0 = 2366841600L; t0 < 2366841600L + 31622400L; t0 += 3 * 3600 + 11) { time_t t = t0; struct tm *tm = localtime(&t); printf("%d%02d%02d%02d%02d%d ", tm->tm_year, tm->tm_mon, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_isdst); } putchar('\n'); }
    return 0;
}
