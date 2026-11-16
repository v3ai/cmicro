#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define N 4000
static unsigned long long st = 12345;
static unsigned long long rnd(void) { st ^= st << 13; st ^= st >> 7; st ^= st << 17; return st; }
static unsigned char *p[N]; static size_t sz[N]; static unsigned char tag[N];
static int check(int i) { size_t k; for (k = 0; k < sz[i]; k++) if (p[i][k] != (unsigned char)(tag[i] + k)) return 0; return 1; }
static void fill(int i, size_t from) { size_t k; for (k = from; k < sz[i]; k++) p[i][k] = (unsigned char)(tag[i] + k); }
int main(void) {
    long ops; int bad = 0;
    for (ops = 0; ops < 100000; ops++) {
        int i = (int)(rnd() % N), r = (int)(rnd() % 100);
        size_t n = (rnd() % 8 == 0) ? (size_t)(rnd() % 3000000) : (size_t)(rnd() % 600);
        if (p[i] && !check(i)) { printf("CORRUPT op %ld slot %d\n", ops, i); bad++; break; }
        if (r < 40) { free(p[i]); p[i] = malloc(n); if (!p[i] && n) { puts("OOM"); return 1; } sz[i] = n; tag[i] = (unsigned char)rnd(); fill(i, 0); }
        else if (r < 75) { unsigned char *q = realloc(p[i], n ? n : 1); if (!q) { puts("OOM"); return 1; } size_t old = p[i] ? sz[i] : 0; if (!p[i]) tag[i] = (unsigned char)rnd(); p[i] = q; sz[i] = n ? n : 1; if (old > sz[i]) old = sz[i]; fill(i, old); }
        else if (r < 85) { free(p[i]); p[i] = calloc(1, n); sz[i] = n; { size_t k; for (k = 0; k < n; k++) if (p[i][k]) { printf("calloc not zero\n"); bad++; break; } } tag[i] = 7; fill(i, 0); }
        else { free(p[i]); p[i] = NULL; sz[i] = 0; }
        if (p[i] && ((unsigned long)p[i] & 15)) { printf("misaligned %p\n", (void *)p[i]); bad++; }
    }
    { int i; for (i = 0; i < N; i++) if (p[i] && !check(i)) { printf("CORRUPT at end %d\n", i); bad++; } }
    printf("alloc test done bad=%d\n", bad);
    return bad;
}
