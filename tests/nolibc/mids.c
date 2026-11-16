#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void) {
    static char line[8192];
    FILE *f = fopen("mids.txt", "r");
    while (fgets(line, sizeof line, f)) {
        union { double d; unsigned long long u; } y;
        char *e;
        line[strcspn(line, "\n")] = 0;
        y.d = strtod(line, &e);
        printf("%016llx %d\n", y.u, (int)(e - line));
    }
    return 0;
}
