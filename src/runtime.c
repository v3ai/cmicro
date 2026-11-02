#include "runtime.h"
#include "platform.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

char *config_dir = NULL;

static const char *kind_dir[RT_NKINDS] = {"syntax", "colorschemes", "help"};
static const char *kind_ext[RT_NKINDS] = {".yaml", ".micro", ".md"};

static const RtFile *kind_table(int kind) {
    switch (kind) {
    case RT_SYNTAX: return rt_syntax_files;
    case RT_COLORSCHEME: return rt_colorscheme_files;
    default: return rt_help_files;
    }
}

char *runtime_read(int kind, const char *name) {
    const RtFile *t;
    if (kind < 0 || kind >= RT_NKINDS || !name) return NULL;
    if (config_dir) {
        char *p = xsprintf("%s/%s/%s%s", config_dir, kind_dir[kind], name, kind_ext[kind]);
        char *data = read_file(p, NULL);
        free(p);
        if (data) return data;
    }
    for (t = kind_table(kind); t->name; t++)
        if (!strcmp(t->name, name)) return xstrdup(t->data);
    return NULL;
}

static int cmpstr(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* sorted, de-duplicated names of the embedded files plus the user's */
char **runtime_list(int kind) {
    char **out = NULL;
    int n = 0, cap = 0, i, k;
    const RtFile *t;
    char **user = NULL;
    if (kind < 0 || kind >= RT_NKINDS) {
        out = xmalloc(sizeof *out);
        out[0] = NULL;
        return out;
    }
    if (config_dir) {
        char *d = xsprintf("%s/%s", config_dir, kind_dir[kind]);
        user = plat_list_dir(d);
        free(d);
    }
#define ADD(s)                                                                                      \
    do {                                                                                            \
        if (n + 2 > cap) { cap = cap ? cap * 2 : 64; out = xrealloc(out, sizeof *out * (size_t)cap); } \
        out[n++] = (s);                                                                             \
    } while (0)
    for (t = kind_table(kind); t->name; t++) ADD(xstrdup(t->name));
    if (user) {
        size_t el = strlen(kind_ext[kind]);
        for (i = 0; user[i]; i++) {
            size_t ul = strlen(user[i]);
            if (ul > el && ends_with(user[i], kind_ext[kind])) ADD(xstrndup(user[i], ul - el));
        }
        free_strv(user);
    }
#undef ADD
    if (!out) out = xmalloc(sizeof *out);
    qsort(out, (size_t)n, sizeof *out, cmpstr);
    for (i = k = 0; i < n; i++) { /* drop duplicates (sorted, so adjacent) */
        if (k > 0 && !strcmp(out[k - 1], out[i])) free(out[i]);
        else out[k++] = out[i];
    }
    out[k] = NULL;
    return out;
}
