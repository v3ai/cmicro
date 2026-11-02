/*
 * io_test.c - tests for text loading/saving, undo/redo, backups and the
 * sudo save path (POSIX only).  Build and run with tests/io_run.sh, which
 * also builds it with -fsanitize=address,undefined.
 *
 * backup.c is #included so its static helpers can be tested directly;
 * link every editor object except main.o and backup.o.
 */
#define _POSIX_C_SOURCE 200809L
#include "../src/backup.c"

#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

int term_ready;
static int prompt_answer = 1; /* index into backup choices: 1 = ignore */
int term_prompt(const char *msg, const char *const *choices, int nchoices) {
    (void)msg; (void)choices; (void)nchoices;
    return prompt_answer;
}

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "%s:%d: FAIL %s\n", __FILE__, __LINE__, #c); } } while (0)

static char dir[512];

static char *tpath(const char *name) { return xsprintf("%s/%s", dir, name); }

static void write_raw(const char *path, const char *data, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(2); }
    fwrite(data, 1, n, f);
    fclose(f);
}

static char *read_raw(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    Str s;
    char buf[4096];
    size_t k;
    if (!f) return NULL;
    str_init(&s);
    str_reserve(&s, 0);
    while ((k = fread(buf, 1, sizeof buf, f)) > 0) str_putn(&s, buf, (int)k);
    fclose(f);
    *n = (size_t)s.len;
    return str_detach(&s);
}

static int put_str(void *ctx, const char *p, size_t n) { str_putn(ctx, p, (int)n); return 0; }

static char *serialize(Buffer *b, size_t *n) {
    Str s;
    str_init(&s);
    str_reserve(&s, 0);
    buf_serialize(b, put_str, &s);
    *n = (size_t)s.len;
    return str_detach(&s);
}

/* text -> buffer -> bytes gives back exactly the same bytes */
static void roundtrip(const char *t, size_t n) {
    Buffer *b = buf_new_empty();
    size_t m;
    char *out;
    buf_set_text(b, t, (int)n);
    out = serialize(b, &m);
    CHECK(m == n && memcmp(out, t, n) == 0);
    if (!(m == n && memcmp(out, t, n) == 0)) fprintf(stderr, "  input of %d bytes\n", (int)n);
    free(out);
    buf_free(b);
}

static void test_text(void) {
    static const char *cases[] = {
        "", "a", "\n", "\n\n", "abc\n", "abc", "a\r\nb\r\n", "a\r\nb", "a\r\nb\r",
        "abc\r", "\r", "\r\n", "a\nb\r\nc\n", "\xEF\xBB\xBF", "\xEF\xBB\xBFx\r\ny\r\n",
        "\xEF\xBB", "\xff\xfe\x80 bad utf8 \xc3\n\xe2\x82", "\r\r\n",
    };
    size_t i;
    Buffer *b;
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) roundtrip(cases[i], strlen(cases[i]));
    roundtrip("nul\0inside\0\nline\0", 18);
    /* line structure */
    b = buf_new_empty();
    buf_set_text(b, "a\r\nb\r\nc", 7);
    CHECK(b->nlines == 3 && b->lines[1].len == 1 && !strcmp(BOPTS(b, O_FILEFORMAT), "dos"));
    buf_set_text(b, "a\nb\r\nc", 6);
    CHECK(b->nlines == 3 && b->lines[1].len == 2 && !strcmp(BOPTS(b, O_FILEFORMAT), "unix"));
    buf_set_text(b, "x\0y", 3);
    CHECK(b->nlines == 1 && b->lines[0].len == 3);
    buf_free(b);
    /* random byte soup made of "interesting" bytes */
    {
        static const char alpha[] = {'a', '\n', '\r', '\0', (char)0xEF, (char)0xBB, (char)0xBF, (char)0xC3};
        char t[64];
        int k, j;
        srand(1);
        for (k = 0; k < 20000; k++) {
            size_t n = (size_t)(rand() % 40);
            for (j = 0; j < (int)n; j++) t[j] = alpha[rand() % 8];
            /* a CRLF first line makes lone LFs CRLF on save: skip those */
            {
                char *nl = memchr(t, '\n', n);
                int bom = n >= 3 && !memcmp(t, "\xEF\xBB\xBF", 3);
                int dos = nl && nl > t + (bom ? 3 : 0) && nl[-1] == '\r';
                if (dos) {
                    int ok = 1;
                    for (j = 0; j < (int)n; j++)
                        if (t[j] == '\n' && (j == 0 || t[j - 1] != '\r' || (bom && j == 3))) ok = 0;
                    if (!ok) continue;
                }
            }
            roundtrip(t, n);
        }
    }
}

/* random edits, then undo everything / redo everything */
static void test_undo(void) {
    int round;
    srand(7);
    for (round = 0; round < 200; round++) {
        Buffer *b = buf_new_empty();
        char *orig, *final, *now;
        int ol, fl, nl, k;
        buf_set_text(b, "hello\nworld\n\xc3\xa9t\xc3\xa9\n", 15);
        orig = buf_full_text(b, &ol);
        for (k = 0; k < 60; k++) {
            Loc a = loc_make(rand() % 12, rand() % (b->nlines + 1));
            if (rand() % 3) {
                /* stray UTF-8 pieces too: they merge into characters */
                static const char *ins[] = {"x", "\n", "ab\ncd", "\xe2\x82\xac", "\r\n", "", "\n\n\n", "\x81", "\xc3", "\xa9"};
                const char *s = ins[rand() % 10];
                buf_insert(b, a, s, (int)strlen(s));
            } else {
                Loc z = loc_make(rand() % 12, rand() % (b->nlines + 1));
                buf_remove(b, a, z);
            }
        }
        final = buf_full_text(b, &fl);
        while (buf_undo(b)) {}
        now = buf_full_text(b, &nl);
        CHECK(nl == ol && !memcmp(now, orig, (size_t)ol));
        CHECK(!buf_modified(b));
        free(now);
        while (buf_redo(b)) {}
        now = buf_full_text(b, &nl);
        CHECK(nl == fl && !memcmp(now, final, (size_t)fl));
        free(now);
        free(orig);
        free(final);
        buf_free(b);
    }
}

static mode_t file_mode(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 ? st.st_mode & 07777 : 0;
}

static void test_files(void) {
    char *p = tpath("plain.txt"), *q, *err = NULL, *data;
    size_t n;
    Buffer *b;
    static const char content[] = "one\r\ntwo\0zero\r\n\xEF\xBB\xBF";
    write_raw(p, content, sizeof content - 1);
    chmod(p, 0640);
    b = buf_open(p, &err);
    CHECK(b != NULL);
    if (!b) return;
    CHECK(b->nlines == 3 && b->lines[1].len == 8);
    BOPT(b, O_EOFNEWLINE) = 0;
    CHECK(buf_save(b, p, &err) == 0);
    data = read_raw(p, &n);
    CHECK(data && n == sizeof content - 1 && !memcmp(data, content, n));
    free(data);
    CHECK(file_mode(p) == 0640); /* mode survives the replace */

    /* symlinks are written through: the link stays, the target changes */
    q = tpath("link.txt");
    CHECK(symlink(p, q) == 0);
    buf_insert(b, loc_make(0, 0), "X", 1);
    CHECK(buf_save(b, q, &err) == 0);
    {
        struct stat st;
        CHECK(lstat(q, &st) == 0 && S_ISLNK(st.st_mode));
    }
    data = read_raw(p, &n);
    CHECK(data && n > 0 && data[0] == 'X');
    free(data);
    free(q);

    /* hard links stay shared (rewritten in place) */
    q = tpath("hard.txt");
    CHECK(link(p, q) == 0);
    buf_insert(b, loc_make(0, 0), "Y", 1);
    CHECK(buf_save(b, p, &err) == 0);
    data = read_raw(q, &n);
    CHECK(data && n > 0 && data[0] == 'Y');
    free(data);
    unlink(q);
    free(q);
    buf_free(b);

    /* a read-only directory: denied, nothing touched or left behind */
    if (geteuid() != 0) {
        char *ro = tpath("ro"), *f;
        Buffer *c = buf_new_empty();
        mkdir(ro, 0755);
        f = xsprintf("%s/new.txt", ro);
        chmod(ro, 0555);
        buf_set_text(c, "data\n", 5);
        CHECK(buf_save(c, f, &err) == -2);
        free(err);
        err = NULL;
        CHECK(!plat_exists(f));
        /* a writable file in a read-only dir is saved in place */
        chmod(ro, 0755);
        write_raw(f, "old contents that are longer\n", 29);
        chmod(ro, 0555);
        CHECK(buf_save(c, f, &err) == 0);
        data = read_raw(f, &n);
        CHECK(data && n == 5 && !memcmp(data, "data\n", 5));
        free(data);
        /* a read-only file: denied, and its contents survive */
        chmod(f, 0444);
        buf_insert(c, loc_make(0, 0), "zzz", 3);
        CHECK(buf_save(c, f, &err) == -2);
        free(err);
        err = NULL;
        data = read_raw(f, &n);
        CHECK(data && n == 5 && !memcmp(data, "data\n", 5));
        free(data);
        chmod(ro, 0755);
        unlink(f);
        rmdir(ro);
        free(f);
        free(ro);
        buf_free(c);
    }

    /* /dev/null is not a regular file: no truncate/fsync errors */
    {
        Buffer *c = buf_new_empty();
        buf_set_text(c, "x", 1);
        CHECK(buf_write_file(c, "/dev/null", 0, &err) == 0);
        buf_free(c);
    }

    /* an unreadable file is refused, not opened empty */
    if (geteuid() != 0) {
        chmod(p, 0);
        b = buf_open(p, &err);
        CHECK(b == NULL && err != NULL);
        free(err);
        err = NULL;
        chmod(p, 0644);
    }
    unlink(p);
    free(p);
}

static void test_private(void) {
    char *p = tpath("state"), *victim = tpath("victim"), *err = NULL, *data;
    Buffer *b = buf_new_empty();
    size_t n;
    struct stat st;
    write_raw(victim, "precious", 8);
    CHECK(symlink(victim, p) == 0);
    buf_set_text(b, "secret", 6);
    /* a planted symlink is replaced, not followed */
    CHECK(buf_write_file(b, p, PLAT_SAVE_PRIVATE, &err) == 0);
    data = read_raw(victim, &n);
    CHECK(data && n == 8 && !memcmp(data, "precious", 8));
    free(data);
    CHECK(lstat(p, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0777) == 0600);
    unlink(p);
    unlink(victim);
    free(p);
    free(victim);
    buf_free(b);
}

static void test_backup_names(void) {
    char *e;
    e = escape_path("/home/u/a b/../c.txt");
    CHECK(!strcmp(e, "%2Fhome%2Fu%2Fa+b%2F..%2Fc.txt"));
    CHECK(!strchr(e, '/'));
    free(e);
    e = escape_path("..");
    CHECK(strcmp(e, "..") != 0 && e[0] != '.');
    free(e);
    {
        char longp[2000];
        memset(longp, 'a', sizeof longp - 1);
        longp[0] = '/';
        longp[sizeof longp - 1] = 0;
        e = escape_path(longp);
        CHECK(strlen(e) <= MAX_NAME && !strchr(e, '/'));
        free(e);
        longp[500] = 'b'; /* same prefix, different path: different name */
        {
            char *e2 = escape_path(longp);
            e = escape_path(longp + 0);
            longp[500] = 'a';
            free(e);
            e = escape_path(longp);
            CHECK(strcmp(e, e2) != 0);
            free(e2);
        }
        free(e);
    }
}

static void test_backups(void) {
    char *p = tpath("doc.txt"), *bdir = tpath("cfg/backups"), *bf, *err = NULL, *data;
    Buffer *b;
    Pane *pane;
    size_t n;
    struct stat st;
    config_dir = tpath("cfg");
    write_raw(p, "a\r\nb\r\n", 6);
    b = buf_open(p, &err);
    CHECK(b != NULL);
    if (!b) return;
    pane = pane_new(b);
    editor_add_tab(pane);
    buf_insert(b, loc_make(0, 1), "new ", 4);
    backup_flush_all();
    bf = backup_file(b);
    CHECK(bf && lstat(bf, &st) == 0 && (st.st_mode & 0777) == 0600);
    CHECK(stat(bdir, &st) == 0 && (st.st_mode & 0777) == 0700);
    data = read_raw(bf, &n); /* the file's own line endings */
    CHECK(data && n == 10 && !memcmp(data, "a\r\nnew b\r\n", 10));
    free(data);
    /* reopening offers recovery; recover keeps dos format */
    prompt_answer = 0;
    {
        Buffer *r = buf_open(p, &err);
        CHECK(r && r->nlines == 3 && r->lines[1].len == 5 && !strcmp(BOPTS(r, O_FILEFORMAT), "dos"));
        CHECK(r && buf_modified(r));
        if (r) { r->path[0] = 0; buf_free(r); }
    }
    /* someone else's file / a symlink in the backup dir is not offered */
    unlink(bf);
    CHECK(symlink(p, bf) == 0);
    {
        Buffer *r = buf_open(p, &err);
        CHECK(r && !buf_modified(r));
        if (r) { r->path[0] = 0; buf_free(r); }
    }
    unlink(bf);
    prompt_answer = 1;
    /* sudo save: a hostile file name goes to dd as one argument */
    {
        static const char *names[] = {"x'; touch PWNED; '", "$(touch PWNED2)", "`touch PWNED3`", "a\"b;c|d&e"};
        size_t i;
        char *pw = tpath("PWNED");
        free(GOPTS(O_SUCMD));
        gopts[O_SUCMD].s = xstrdup("env"); /* `env dd bs=4k of=...` */
        CHECK(chdir(dir) == 0);
        for (i = 0; i < sizeof names / sizeof names[0]; i++) {
            char *t = tpath(names[i]);
            CHECK(buf_save_sudo(b, t, &err) == 0);
            data = read_raw(t, &n);
            CHECK(data && n == 10 && !memcmp(data, "a\r\nnew b\r\n", 10));
            free(data);
            unlink(t);
            free(t);
        }
        CHECK(!plat_exists(pw) && !plat_exists("PWNED2") && !plat_exists("PWNED3"));
        free(pw);
        free(GOPTS(O_SUCMD));
        gopts[O_SUCMD].s = xstrdup("false");
        CHECK(buf_save_sudo(b, p, &err) == -1);
        free(err);
        err = NULL;
    }
    unlink(p);
    free(bf);
    free(bdir);
    free(p);
}

/* $IO_TEST_FULLDIR: a small file system (e.g. a 1 MB tmpfs) to fill up */
static void test_disk_full(const char *fdir) {
    char *p = xsprintf("%s/full.txt", fdir), *h = xsprintf("%s/full-link.txt", fdir), *err = NULL, *data;
    char *big = xmalloc(4 << 20);
    Buffer *b = buf_new_empty();
    size_t n;
    int pass;
    memset(big, 'z', 4 << 20);
    for (pass = 0; pass < 2; pass++) {
        /* pass 0: replaced via a temporary file; pass 1: a hard-linked
           file, rewritten in place after reserving the space */
        write_raw(p, "keep me\n", 8);
        if (pass == 1) CHECK(link(p, h) == 0);
        buf_set_text(b, big, 4 << 20);
        CHECK(buf_save(b, p, &err) == -1);
        fprintf(stderr, "  disk full (pass %d): %s\n", pass, err ? err : "?");
        free(err);
        err = NULL;
        data = read_raw(p, &n);
        CHECK(data && n == 8 && !memcmp(data, "keep me\n", 8));
        free(data);
        unlink(h);
    }
    {
        /* nothing left behind */
        char **ls = plat_list_dir(fdir);
        int k;
        CHECK(ls && ls[0] && !strcmp(ls[0], "full.txt") && !ls[1]);
        for (k = 0; ls && ls[k]; k++) free(ls[k]);
        free(ls);
    }
    unlink(p);
    free(p);
    free(h);
    free(big);
    buf_free(b);
}

int main(void) {
    const char *t = getenv("TMPDIR");
    snprintf(dir, sizeof dir, "%s/micro-io-test-XXXXXX", t && *t ? t : "/tmp");
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 2; }
    options_init_defaults();
    syntax_init();
    test_text();
    test_undo();
    test_files();
    test_private();
    test_backup_names();
    test_backups();
    if (getenv("IO_TEST_FULLDIR")) test_disk_full(getenv("IO_TEST_FULLDIR"));
    printf("io_test: %d checks, %d failures (%s)\n", checks, fails, dir);
    if (!fails) {
        char *cmd = xsprintf("rm -rf '%s'", dir);
        if (system(cmd) != 0) fails++;
        free(cmd);
    }
    return fails != 0;
}
