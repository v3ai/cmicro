#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static int cmpint(const void *a, const void *b) { int x = *(const int *)a, y = *(const int *)b; return x < y ? -1 : x > y; }
int main(void) {
    char buf[256];
    int i;
    /* printf */
    printf("[%d][%5d][%-5d][%05d][%+d][% d][%.3d][%ld][%lld][%u][%x][%X][%04x][%02X][%o]\n", 42, 42, 42, -42, 7, 7, 5, -123456789L, -9000000000LL, 3000000000U, 255, 255, 10, 7, 8);
    printf("[%s][%10s][%-10s][%.3s][%*s][%*d][%c][%%][%.1f][%.2f][%f][%8.3f][%-8.1f][%.0f]\n", "hi", "hi", "hi", "abcdef", 6, "x", -4, 3, 'Z', 3.14159, -2.005, 1.5, 3.14159, 2.25, 2.5);
    printf("[%.1fKB][%.1fMB][%zu][%p]\n", 1536 / 1024.0, 5242880 / (1024.0 * 1024.0), (size_t)12345, (void *)0);
    i = snprintf(buf, 8, "%s-%d", "truncated", 12345); printf("snprintf %d [%s]\n", i, buf);
    i = snprintf(NULL, 0, "%d%s", 100, "abc"); printf("measure %d\n", i);
    /* numbers */
    { const char *ns[] = {"0", "42", "-17", "3.5", "1e3", "2.5e-3", "  +7.25x", ".5", "1e309", "abc", "0x1A", "007", "123456789012345678901234", "-0.000001"};
      for (i = 0; i < (int)(sizeof ns / sizeof ns[0]); i++) { char *e1, *e2; double d = strtod(ns[i], &e1); long l = strtol(ns[i], &e2, 0);
        printf("strtod(%s)=%.6g end+%d strtol=%ld end+%d\n", ns[i], d, (int)(e1 - ns[i]), l, (int)(e2 - ns[i])); } }
    printf("atoi %d %d\n", atoi("  -12abc"), atoi("x"));
    /* strings */
    strcpy(buf, "hello world"); strcat(buf, "!!");
    printf("%s %d %s %s %s %d %d\n", buf, (int)strlen(buf), strchr(buf, 'o'), strrchr(buf, 'o'), strstr(buf, "wor"), (int)strcspn(buf, " !"), (int)strspn(buf, "hel"));
    printf("%d %d %d %d\n", strcmp("abc", "abd") < 0, strncmp("abcx", "abcy", 3), memcmp("ab", "ac", 2) < 0, strpbrk("abc", "xc") - (char*)0 > 0);
    memmove(buf + 2, buf, 5); buf[12] = 0; printf("memmove [%s]\n", buf);
    printf("ctype %d%d%d%d%d %c%c\n", !!isalnum('a'), !!isspace('\t'), !!isxdigit('F'), !!isupper('a'), !!isdigit('5'), tolower('Q'), toupper('q'));
    /* qsort */
    { int a[200]; for (i = 0; i < 200; i++) a[i] = (i * 7919) % 211 - 100; qsort(a, 200, sizeof a[0], cmpint);
      for (i = 1; i < 200; i++) if (a[i - 1] > a[i]) break; printf("qsort sorted=%d first=%d last=%d\n", i == 200, a[0], a[199]); }
    /* malloc stress */
    { void *ptrs[1000]; unsigned long sum = 0; int k;
      for (i = 0; i < 1000; i++) { ptrs[i] = malloc((size_t)(i * 37 % 5000) + 1); memset(ptrs[i], i & 255, (size_t)(i * 37 % 5000) + 1); }
      for (k = 0; k < 3; k++) for (i = 0; i < 1000; i += 2) { size_t n = (size_t)((i * 53 + k * 999) % 300000) + 1; ptrs[i] = realloc(ptrs[i], n); memset(ptrs[i], 1, n); }
      for (i = 1; i < 1000; i += 2) sum += ((unsigned char *)ptrs[i])[0];
      for (i = 0; i < 1000; i++) free(ptrs[i]);
      printf("malloc ok %lu\n", sum); }
    return 0;
}
