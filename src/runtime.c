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

char **runtime_list(int kind) {
    char **out = NULL;
    int n = 0, cap = 0, i;
    const RtFile *t;
    char **user = NULL;
    if (config_dir) {
        char *d = xsprintf("%s/%s", config_dir, kind_dir[kind]);
        user = plat_list_dir(d);
        free(d);
    }
#define ADD(s)                                                         \
    do {                                                               \
        int dup_ = 0, k_;                                              \
        for (k_ = 0; k_ < n; k_++) if (!strcmp(out[k_], (s))) dup_ = 1; \
        if (!dup_) {                                                   \
            if (n + 2 > cap) { cap = cap ? cap * 2 : 64; out = xrealloc(out, sizeof *out * (size_t)cap); } \
            out[n++] = xstrdup(s);                                     \
        }                                                              \
    } while (0)
    for (t = kind_table(kind); t->name; t++) ADD(t->name);
    if (user) {
        for (i = 0; user[i]; i++) {
            if (ends_with(user[i], kind_ext[kind])) {
                char *nm = xstrndup(user[i], strlen(user[i]) - strlen(kind_ext[kind]));
                ADD(nm);
                free(nm);
            }
        }
        free_strv(user);
    }
#undef ADD
    if (!out) out = xmalloc(sizeof *out);
    out[n] = NULL;
    qsort(out, (size_t)n, sizeof *out, cmpstr);
    return out;
}
