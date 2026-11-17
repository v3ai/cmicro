/* Checks cm_str_printf, the editor's C89 replacement for vsnprintf, against
 * fixed expected output.  Prints failures; exit 1 if any. */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/util.h"

static int fails, count;

static void expect(Str *s, const char *want) {
    count++;
    if (strcmp(s->s ? s->s : "", want) != 0) {
        printf("got \"%s\", want \"%s\"\n", s->s ? s->s : "", want);
        fails++;
    }
    cm_str_clear(s);
}

int main(void) {
    Str s;
    char *x;
    cm_str_init(&s);
    cm_str_printf(&s, "plain"); expect(&s, "plain");
    cm_str_printf(&s, "%d|%i|%d", 0, 42, -7); expect(&s, "0|42|-7");
    cm_str_printf(&s, "%d", INT_MIN + 1); expect(&s, "-2147483647");
    cm_str_printf(&s, "%d", INT_MAX); expect(&s, "2147483647");
    cm_str_printf(&s, "%ld|%ld", 2147483647L, -2147483647L - 1); expect(&s, "2147483647|-2147483648");
    cm_str_printf(&s, "%u|%lu", 4000000000U, 4294967295UL); expect(&s, "4000000000|4294967295");
    cm_str_printf(&s, "%x|%X|%04x|%02X", 255, 255, 10, 7); expect(&s, "ff|FF|000a|07");
    cm_str_printf(&s, "[%5d][%-5d][%05d][%-05d]", 42, 42, -42, 42); expect(&s, "[   42][42   ][-0042][42   ]");
    cm_str_printf(&s, "[%*d][%*d]", 4, 7, -4, 7); expect(&s, "[   7][7   ]");
    cm_str_printf(&s, "[%.3d][%6.3d]", 5, -5); expect(&s, "[005][  -005]");
    cm_str_printf(&s, "[%s][%5s][%-5s][%.2s][%*s]", "ab", "ab", "ab", "abc", 3, "x");
    expect(&s, "[ab][   ab][ab   ][ab][  x]");
    cm_str_printf(&s, "[%.*s]", 2, "hello"); expect(&s, "[he]");
    cm_str_printf(&s, "%c%c%c", 'a', 0xC3, 0xA9); expect(&s, "a\xc3\xa9");
    cm_str_printf(&s, "100%%"); expect(&s, "100%");
    cm_str_printf(&s, "%s", (const char *)NULL); expect(&s, "(null)");
    cm_str_printf(&s, "a"); cm_str_printf(&s, "%s", "b"); expect(&s, "ab");
    x = cm_xsprintf("%s/%s-%ld-%d", "/tmp", "micro", 1234567L, 89);
    cm_str_puts(&s, x); free(x); expect(&s, "/tmp/micro-1234567-89");
    cm_str_free(&s);
    printf("%d format checks, %d failures\n", count, fails);
    return fails != 0;
}
