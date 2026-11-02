/*
 * term_fuzz_input.c - libFuzzer harness for the terminal input decoder.
 *
 *   clang -g -O1 -fsanitize=fuzzer,address,undefined -Isrc \
 *       tests/term_fuzz_input.c src/util.c src/platform.c src/platform_win32.c \
 *       -o term_fuzz_input && ./term_fuzz_input -max_len=65536
 *
 * Without -fsanitize=fuzzer, build with -DSTANDALONE to replay files given
 * on the command line (or a built-in set of nasty inputs when none are).
 *
 * The fuzz input is the byte stream the terminal sends.  The first byte
 * picks the size of the chunks plat_read() hands out, so sequences get
 * split at every possible point.  The fake clock advances on every call so
 * the bracketed-paste deadline always expires.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define plat_read fz_read
#define plat_now_ms fz_now_ms
#define plat_resized fz_resized
#include "../src/input.c"

static const unsigned char *fz_data;
static size_t fz_len, fz_pos, fz_chunk;
static long long fz_clock;

int fz_read(char *buf, int n, int timeout_ms) {
    size_t k;
    (void)timeout_ms;
    fz_clock += 50;
    if (fz_pos >= fz_len) return 0;
    k = fz_len - fz_pos;
    if (k > fz_chunk) k = fz_chunk;
    if (k > (size_t)n) k = (size_t)n;
    memcpy(buf, fz_data + fz_pos, k);
    fz_pos += k;
    return (int)k;
}

long long fz_now_ms(void) { return fz_clock += 7; }

int fz_resized(void) { return 0; }

static void check(const InEvent *ev) {
    if (ev->type == IN_MOUSE) {
#ifndef FZ_LENIENT /* (to replay against older decoders) */
        if (ev->mx < 0 || ev->my < 0 || ev->mx > 65535 || ev->my > 65535) abort();
#endif
        if (ev->key < KEY_MOUSE_FIRST || ev->key >= KEY_MOUSE_LAST) abort();
    } else if (ev->type == IN_KEY) {
        if (ev->key <= 0) abort();
        if (ev->key < KEY_SPECIAL && (ev->key > 0x10FFFF || (ev->key >= 0xD800 && ev->key <= 0xDFFF)))
            abort();
        if (ev->key > KEY_SPECIAL && ev->key >= KEY_MOUSE_FIRST) abort();
        if (ev->rawlen < 0 || ev->rawlen >= (int)sizeof ev->raw || ev->raw[ev->rawlen]) abort();
        {
            /* key names must round-trip through the parser */
            char *name = key_name(ev->key, ev->mods);
            int k, m;
            if (strcmp(name, "Unknown") != 0 && !key_parse(name, &k, &m)) abort();
            free(name);
        }
    } else if (ev->type == IN_PASTE) {
        if (!ev->paste || ev->pastelen < 0 || ev->paste[ev->pastelen]) abort();
    } else if (ev->type != IN_NONE) {
        abort();
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    InEvent ev;
    int guard = 0;
    if (size < 1) return 0;
    fz_chunk = (size_t)(data[0] % 16) + 1;
    if (data[0] >= 0xF0) fz_chunk = sizeof inbuf; /* big reads */
    fz_data = data + 1;
    fz_len = size - 1;
    fz_pos = 0;
    inlen = 0;
    for (;;) {
        int r = input_poll(&ev, 0);
        if (r < 0) break;
        if (r == 0) {
            if (fz_pos >= fz_len && inlen == 0) break;
            if (++guard > 100000) abort(); /* no progress */
            continue;
        }
        check(&ev);
        input_free(&ev);
    }
    {
        /* also feed the bytes to the key-name parser */
        char *s = malloc(size + 1);
        int k, m;
        memcpy(s, data, size);
        s[size] = 0;
        key_parse(s, &k, &m);
        free(s);
    }
    return 0;
}

#ifdef STANDALONE
static const char *const nasty[] = {
    "\x1b[<0;99999999999999999999;99999999999999999999M",
    "\x1b[<64;0;0M\x1b[<65;-5;-5M",
    "\x1b[M",
    "\x1b[M\xff\x00\x00",
    "\x1b[200~unterminated paste",
    "\x1b[200~abc\x1b[201",
    "\x1b[1;99999999999999999999A",
    "\x1b[99999999999999999999u",
    "\x1b[55296u",
    "\xf0\x9f",
    "\x1b\xe2\x82",
    "\x1b[?1;2;3;4;5;6;7;8;9;10;11;12c",
};

int main(int argc, char **argv) {
    int i;
    if (argc < 2) {
        for (i = 0; i < (int)(sizeof nasty / sizeof nasty[0]); i++) {
            size_t n = strlen(nasty[i]);
            unsigned char *b = malloc(n + 1);
            int c;
            memcpy(b + 1, nasty[i], n);
            for (c = 0; c < 17; c++) {
                b[0] = (unsigned char)(c == 16 ? 0xF0 : c);
                LLVMFuzzerTestOneInput(b, n + 1);
            }
            free(b);
        }
        {
            /* a flood of ignored sequences, and a multi-megabyte paste */
            size_t n = 4u << 20, k;
            unsigned char *b = malloc(n + 16);
            b[0] = 0xF0;
            for (k = 1; k + 5 <= n; k += 5) memcpy(b + k, "\x1b[?1u", 5);
            LLVMFuzzerTestOneInput(b, k);
            memcpy(b + 1, "\x1b[200~", 6);
            memset(b + 7, 'x', n - 7);
            LLVMFuzzerTestOneInput(b, n);
            free(b);
        }
        puts("ok");
        return 0;
    }
    for (i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        unsigned char *b;
        long n;
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        b = malloc((size_t)n + 1);
        if (fread(b, 1, (size_t)n, f) == (size_t)n) LLVMFuzzerTestOneInput(b, (size_t)n);
        fclose(f);
        free(b);
    }
    return 0;
}
#endif
