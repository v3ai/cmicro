/*
 * options.c - micro's option table, settings.json loading/saving.
 */
#include "micro.h"
#include "platform.h"
#include "conf.h"
#include "runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const ch_clipboard[] = {"internal", "external", "terminal", NULL};
static const char *const ch_fileformat[] = {"unix", "dos", NULL};
static const char *const ch_helpsplit[] = {"hsplit", "vsplit", NULL};
static const char *const ch_mbstyle[] = {"underline", "highlight", NULL};
static const char *const ch_multiopen[] = {"tab", "hsplit", "vsplit", NULL};
static const char *const ch_reload[] = {"prompt", "auto", "disabled", NULL};
static const char *const ch_truecolor[] = {"auto", "on", "off", NULL};
static const char *const ch_encoding[] = {"utf-8", "utf8", NULL};

const OptDef opt_defs[O_COUNT] = {
    {"autoclose", OT_BOOL, "true", NULL, 0},
    {"autoindent", OT_BOOL, "true", NULL, 0},
    {"autosu", OT_BOOL, "false", NULL, 0},
    {"backup", OT_BOOL, "true", NULL, 0},
    {"backupdir", OT_STR, "", NULL, 0},
    {"basename", OT_BOOL, "false", NULL, 0},
    {"colorcolumn", OT_INT, "0", NULL, 1},
    {"commenttype", OT_STR, "", NULL, 0},
    {"cursorline", OT_BOOL, "true", NULL, 0},
    {"detectlimit", OT_INT, "100", NULL, 1},
    {"diffgutter", OT_BOOL, "false", NULL, 0},
    {"encoding", OT_STR, "utf-8", ch_encoding, 0},
    {"eofnewline", OT_BOOL, "true", NULL, 0},
    {"fastdirty", OT_BOOL, "false", NULL, 0},
    {"fileformat", OT_STR, "unix", ch_fileformat, 0},
    {"filetype", OT_STR, "unknown", NULL, 0},
    {"hlsearch", OT_BOOL, "false", NULL, 0},
    {"hltaberrors", OT_BOOL, "false", NULL, 0},
    {"hltrailingws", OT_BOOL, "false", NULL, 0},
    {"ignorecase", OT_BOOL, "true", NULL, 0},
    {"incsearch", OT_BOOL, "true", NULL, 0},
    {"indentchar", OT_STR, " ", NULL, 0},
    {"keepautoindent", OT_BOOL, "false", NULL, 0},
    {"matchbrace", OT_BOOL, "true", NULL, 0},
    {"matchbraceleft", OT_BOOL, "true", NULL, 0},
    {"matchbracestyle", OT_STR, "underline", ch_mbstyle, 0},
    {"mkparents", OT_BOOL, "false", NULL, 0},
    {"pageoverlap", OT_INT, "2", NULL, 1},
    {"permbackup", OT_BOOL, "false", NULL, 0},
    {"readonly", OT_BOOL, "false", NULL, 0},
    {"relativeruler", OT_BOOL, "false", NULL, 0},
    {"reload", OT_STR, "prompt", ch_reload, 0},
    {"rmtrailingws", OT_BOOL, "false", NULL, 0},
    {"ruler", OT_BOOL, "true", NULL, 0},
    {"savecursor", OT_BOOL, "false", NULL, 0},
    {"saveundo", OT_BOOL, "false", NULL, 0},
    {"scrollbar", OT_BOOL, "false", NULL, 0},
    {"scrollmargin", OT_INT, "3", NULL, 1},
    {"scrollspeed", OT_INT, "2", NULL, 1},
    {"showchars", OT_STR, "", NULL, 0},
    {"smartpaste", OT_BOOL, "true", NULL, 0},
    {"softwrap", OT_BOOL, "false", NULL, 0},
    {"splitbottom", OT_BOOL, "true", NULL, 0},
    {"splitright", OT_BOOL, "true", NULL, 0},
    {"statusformatl", OT_STR,
     "$(filename) $(modified)$(overwrite)($(line),$(col)) $(status.paste)| ft:$(opt:filetype) | "
     "$(opt:fileformat) | $(opt:encoding)",
     NULL, 0},
    {"statusformatr", OT_STR, "$(bind:ToggleKeyMenu): bindings, $(bind:ToggleHelp): help", NULL, 0},
    {"statusline", OT_BOOL, "true", NULL, 0},
    {"syntax", OT_BOOL, "true", NULL, 0},
    {"tabmovement", OT_BOOL, "false", NULL, 0},
    {"tabsize", OT_INT, "4", NULL, 2},
    {"tabstospaces", OT_BOOL, "false", NULL, 0},
    {"useprimary", OT_BOOL, "true", NULL, 0},
    {"wordwrap", OT_BOOL, "false", NULL, 0},
    /* global */
    {"autosave", OT_INT, "0", NULL, 1},
    {"clipboard", OT_STR, "external", ch_clipboard, 0},
    {"colorscheme", OT_STR, "default", NULL, 0},
    {"comment", OT_BOOL, "true", NULL, 0},
    {"divchars", OT_STR, "|-", NULL, 0},
    {"divreverse", OT_BOOL, "true", NULL, 0},
    {"fakecursor", OT_BOOL, "false", NULL, 0},
    {"ftoptions", OT_BOOL, "true", NULL, 0},
    {"helpsplit", OT_STR, "hsplit", ch_helpsplit, 0},
    {"infobar", OT_BOOL, "true", NULL, 0},
    {"keymenu", OT_BOOL, "false", NULL, 0},
    {"lockbindings", OT_BOOL, "false", NULL, 0},
    {"mouse", OT_BOOL, "true", NULL, 0},
    {"multiopen", OT_STR, "tab", ch_multiopen, 0},
    {"parsecursor", OT_BOOL, "false", NULL, 0},
    {"paste", OT_BOOL, "false", NULL, 0},
    {"savehistory", OT_BOOL, "true", NULL, 0},
    {"scrollbarchar", OT_STR, "|", NULL, 0},
    {"sucmd", OT_STR, "sudo", NULL, 0},
    {"tabalways", OT_BOOL, "false", NULL, 0},
    {"tabhighlight", OT_BOOL, "false", NULL, 0},
    {"tabreverse", OT_BOOL, "true", NULL, 0},
    {"truecolor", OT_STR, "auto", ch_truecolor, 0},
    {"xterm", OT_BOOL, "false", NULL, 0},
};

OptVal gopts[O_COUNT];

/* Integer options stay far from INT_MAX so arithmetic on them (scroll
   margins, autosave seconds * 1000, tab stops) can't overflow; tabsize
   is also the length of an indent string. */
#define OPT_INT_MAX 1000000
#define TABSIZE_MAX 256
static CNode *parsed; /* settings.json contents */
static int parse_error; /* never overwrite a settings.json we failed to parse */

int opt_find(const char *name) {
    int i;
    for (i = 0; i < O_COUNT; i++)
        if (!strcmp(opt_defs[i].name, name)) return i;
    return -1;
}

int opt_parse(int id, const char *value, OptVal *out, char **err) {
    const OptDef *d = &opt_defs[id];
    if (err) *err = NULL;
    out->i = 0;
    out->s = NULL;
    switch (d->type) {
    case OT_BOOL:
        if (!strcmp(value, "on") || !strcmp(value, "true")) out->i = 1;
        else if (!strcmp(value, "off") || !strcmp(value, "false")) out->i = 0;
        else { if (err) *err = xstrdup("Invalid value"); return -1; }
        return 0;
    case OT_INT: {
        char *end;
        double v = strtod(value, &end);
        if (end == value || *end || v != v || v > OPT_INT_MAX || v < -OPT_INT_MAX) {
            if (err) *err = xstrdup("Invalid value");
            return -1;
        }
        if (id == O_TABSIZE && v > TABSIZE_MAX) {
            if (err) *err = xsprintf("tabsize must be at most %d", TABSIZE_MAX);
            return -1;
        }
        if (d->nonneg == 1 && v < 0) { if (err) *err = xsprintf("%s must be non-negative", d->name); return -1; }
        if (d->nonneg == 2 && v <= 0) { if (err) *err = xsprintf("%s must be positive", d->name); return -1; }
        out->i = (int)v;
        return 0;
    }
    default:
        if (d->choices) {
            int k, ok = 0;
            for (k = 0; d->choices[k]; k++) if (!strcmp(d->choices[k], value)) ok = 1;
            if (!ok) {
                Str m;
                str_init(&m);
                str_printf(&m, "Invalid value for %s, choices are:", d->name);
                for (k = 0; d->choices[k]; k++) str_printf(&m, " %s", d->choices[k]);
                if (err) *err = str_detach(&m); else str_free(&m);
                return -1;
            }
        }
        out->s = xstrdup(value);
        return 0;
    }
}

char *opt_to_string(int id, const OptVal *v) {
    switch (opt_defs[id].type) {
    case OT_BOOL: return xstrdup(v->i ? "true" : "false");
    case OT_INT: return xsprintf("%d", v->i);
    default: return xstrdup(v->s ? v->s : "");
    }
}

void opt_copy(int id, OptVal *dst, const OptVal *src) {
    if (opt_defs[id].type == OT_STR) {
        char *n = xstrdup(src->s ? src->s : "");
        free(dst->s);
        dst->s = n;
    } else {
        dst->i = src->i;
    }
}

void options_init_defaults(void) {
    int i;
    for (i = 0; i < O_COUNT; i++) {
        OptVal v;
        opt_parse(i, i == O_FILEFORMAT ? plat_default_fileformat() : opt_defs[i].def, &v, NULL);
        gopts[i] = v;
    }
}

/* apply a JSON value to an OptVal */
static int json_to_opt(int id, CNode *v, OptVal *out) {
    if (!v) return -1;
    if (opt_defs[id].type == OT_BOOL && v->type == CN_BOOL) { out->i = v->b; out->s = NULL; return 0; }
    if (opt_defs[id].type == OT_INT && v->type == CN_NUM) return v->str ? opt_parse(id, v->str, out, NULL) : -1;
    if (opt_defs[id].type == OT_INT && v->type == CN_BOOL && id == O_AUTOSAVE) {
        out->i = v->b ? 8 : 0; out->s = NULL; return 0;
    }
    if ((v->type == CN_STR || v->type == CN_NUM || v->type == CN_BOOL) && v->str)
        return opt_parse(id, v->str, out, NULL);
    return -1;
}

void settings_load(void) {
    char *path, *text, *err = NULL;
    int i;
    if (!config_dir) return;
    path = xsprintf("%s/settings.json", config_dir);
    text = read_file(path, NULL);
    free(path);
    if (!text) return;
    if (starts_with(text, "null")) { free(text); return; }
    parsed = json_parse(text, &err);
    free(text);
    if (err) {
        editor_set_error("Error reading settings.json: %s", err);
        free(err);
        parse_error = 1;
    }
    if (!parsed || parsed->type != CN_MAP) {
        parse_error = 1;
        cnode_free(parsed);
        parsed = NULL;
        return;
    }
    for (i = 0; i < parsed->n; i++) {
        int id;
        OptVal v;
        if (parsed->vals[i]->type == CN_MAP) continue;
        id = opt_find(parsed->keys[i]);
        if (id < 0) continue;
        if (json_to_opt(id, parsed->vals[i], &v) == 0) {
            if (opt_defs[id].type == OT_STR) free(gopts[id].s);
            gopts[id] = v;
        } else {
            editor_set_error("Error: setting '%s' has incorrect type, using default value", parsed->keys[i]);
        }
    }
}

void settings_apply_parsed(struct Buffer *b, const char *path) {
    int i, k;
    if (!parsed) return;
    for (i = 0; i < parsed->n; i++) {
        CNode *m = parsed->vals[i];
        const char *key = parsed->keys[i];
        int match = 0;
        if (m->type != CN_MAP) continue;
        if (starts_with(key, "ft:")) {
            match = !strcmp(key + 3, BOPTS(b, O_FILETYPE));
        } else {
            const char *g = starts_with(key, "glob:") ? key + 5 : key;
            if (path && *path) {
                char *ap = b->abspath ? b->abspath : (char *)path;
                match = glob_match(g, path) || glob_match(g, ap) || glob_match(g, path_base(path));
            }
        }
        if (!match) continue;
        for (k = 0; k < m->n; k++) {
            int id = opt_find(m->keys[k]);
            OptVal v;
            if (id < 0 || id >= O_FIRST_GLOBAL) continue;
            if (json_to_opt(id, m->vals[k], &v) == 0) {
                buf_set_option(b, id, &v, 1);
                free(v.s);
            }
        }
    }
}

static void json_write_str(Str *o, const char *s) {
    str_putc(o, '"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { str_putc(o, '\\'); str_putc(o, (char)c); }
        else if (c == '\n') str_puts(o, "\\n");
        else if (c == '\t') str_puts(o, "\\t");
        else if (c < 32 || c == 127) str_printf(o, "\\u%04x", c);
        else str_putc(o, (char)c);
    }
    str_putc(o, '"');
}

static void json_write(Str *o, CNode *n, int ind) {
    int i, j;
    switch (n->type) {
    case CN_MAP: {
        int *ord = xmalloc(sizeof(int) * (size_t)(n->n + 1));
        for (i = 0; i < n->n; i++) ord[i] = i;
        for (i = 1; i < n->n; i++)
            for (j = i; j > 0 && strcmp(n->keys[ord[j - 1]], n->keys[ord[j]]) > 0; j--) {
                int t = ord[j]; ord[j] = ord[j - 1]; ord[j - 1] = t;
            }
        str_puts(o, "{\n");
        for (i = 0; i < n->n; i++) {
            str_printf(o, "%*s", ind + 4, "");
            json_write_str(o, n->keys[ord[i]]);
            str_puts(o, ": ");
            json_write(o, n->vals[ord[i]], ind + 4);
            str_puts(o, i + 1 < n->n ? ",\n" : "\n");
        }
        str_printf(o, "%*s}", ind, "");
        free(ord);
        break;
    }
    case CN_LIST:
        str_puts(o, "[");
        for (i = 0; i < n->n; i++) {
            if (i) str_puts(o, ", ");
            json_write(o, n->vals[i], ind);
        }
        str_puts(o, "]");
        break;
    case CN_STR: json_write_str(o, n->str ? n->str : ""); break;
    case CN_NUM: str_puts(o, n->str ? n->str : "0"); break;
    case CN_BOOL: str_puts(o, n->b ? "true" : "false"); break;
    default: str_puts(o, "null"); break;
    }
}

static CNode *mknode(int type) { CNode *n = xcalloc(1, sizeof *n); n->type = type; return n; }

void settings_note_set(const char *name, const char *value) {
    int id = opt_find(name), i;
    CNode *v;
    if (id < 0) return;
    if (!parsed) parsed = mknode(CN_MAP);
    if (opt_defs[id].type == OT_BOOL) {
        v = mknode(CN_BOOL);
        v->b = !strcmp(value, "on") || !strcmp(value, "true");
        v->str = xstrdup(v->b ? "true" : "false");
    } else if (opt_defs[id].type == OT_INT) {
        double d = atof(value);
        v = mknode(CN_NUM);
        /* (int) of an out-of-range double is undefined */
        if (!(d == d)) d = 0;
        if (d > OPT_INT_MAX) d = OPT_INT_MAX;
        if (d < -OPT_INT_MAX) d = -OPT_INT_MAX;
        v->num = d;
        v->str = xsprintf("%d", (int)d);
    } else {
        v = mknode(CN_STR);
        v->str = xstrdup(value);
    }
    for (i = 0; i < parsed->n; i++) {
        if (!strcmp(parsed->keys[i], name)) {
            cnode_free(parsed->vals[i]);
            parsed->vals[i] = v;
            return;
        }
    }
    parsed->keys = xrealloc(parsed->keys, sizeof(char *) * (size_t)(parsed->n + 1));
    parsed->vals = xrealloc(parsed->vals, sizeof(CNode *) * (size_t)(parsed->n + 1));
    parsed->cap = parsed->n + 1;
    parsed->keys[parsed->n] = xstrdup(name);
    parsed->vals[parsed->n] = v;
    parsed->n++;
}

void settings_note_reset(const char *name) {
    int i;
    if (!parsed) return;
    for (i = 0; i < parsed->n; i++) {
        if (!strcmp(parsed->keys[i], name)) {
            free(parsed->keys[i]);
            cnode_free(parsed->vals[i]);
            memmove(parsed->keys + i, parsed->keys + i + 1, sizeof(char *) * (size_t)(parsed->n - i - 1));
            memmove(parsed->vals + i, parsed->vals + i + 1, sizeof(CNode *) * (size_t)(parsed->n - i - 1));
            parsed->n--;
            return;
        }
    }
}

/* written to a temporary file that then replaces settings.json, so a
   full disk or a crash can't leave it truncated (a symlinked
   settings.json is followed and its target replaced) */
int settings_save(void) {
    char *path;
    Str o;
    PlatSave *f;
    int denied, rc;
    if (!config_dir || !parsed) return 0;
    if (parse_error) return -1;
    str_init(&o);
    str_reserve(&o, 0);
    json_write(&o, parsed, 0);
    str_putc(&o, '\n');
    path = xsprintf("%s/settings.json", config_dir);
    f = plat_save_open(path, (long long)o.len, 0, &denied);
    free(path);
    if (!f) { str_free(&o); return -1; }
    rc = plat_save_write(f, o.s, (size_t)o.len);
    rc = plat_save_close(f, rc == 0) == 0 && rc == 0 ? 0 : -1;
    str_free(&o);
    return rc;
}
