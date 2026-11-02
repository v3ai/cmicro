/*
 * prompt.c - the info bar's interactive prompts (command bar, find,
 * y/n questions) with history and tab completion.
 */
#include "micro.h"
#include "platform.h"
#include "runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *hist_names[HIST_N] = {"command", "find", "shell", "open", "goto", "other"};

int prompt_active(void) { return E.prompt.kind != PROMPT_NONE; }

static void comp_clear(Prompt *pr) {
    int i;
    for (i = 0; i < pr->ncomp; i++) free(pr->completions[i]);
    free(pr->completions);
    pr->completions = NULL;
    pr->ncomp = 0;
    pr->comp_idx = -1;
    free(pr->comp_base);
    pr->comp_base = NULL;
}

static void prompt_reset(void) {
    Prompt *pr = &E.prompt;
    free(pr->msg);
    str_free(&pr->text);
    free(pr->hist_saved);
    comp_clear(pr);
    memset(pr, 0, sizeof *pr);
    pr->kind = PROMPT_NONE;
    pr->anchor = -1;
}

void prompt_text(const char *msg, const char *initial, int hist, PromptDone done,
                 PromptChanged changed, void *data) {
    Prompt *pr = &E.prompt;
    if (prompt_active()) prompt_cancel(); /* let the old prompt free its context */
    prompt_reset();
    pr->kind = PROMPT_TEXT;
    pr->msg = xstrdup(msg);
    str_init(&pr->text);
    str_puts(&pr->text, initial ? initial : "");
    pr->cx = pr->text.len;
    pr->anchor = -1;
    pr->hist = hist;
    pr->hist_pos = E.nhistory[hist];
    pr->done = done;
    pr->changed = changed;
    pr->data = data;
    pr->comp_idx = -1;
    editor_layout();
}

void prompt_yn(const char *msg, YNDone done, void *data) {
    Prompt *pr = &E.prompt;
    if (prompt_active()) prompt_cancel();
    prompt_reset();
    pr->kind = PROMPT_YN;
    pr->msg = xstrdup(msg);
    str_init(&pr->text);
    pr->yn = done;
    pr->data = data;
    pr->anchor = -1;
    editor_layout();
}

void prompt_cancel(void) {
    Prompt *pr = &E.prompt;
    if (pr->kind == PROMPT_TEXT && pr->done) {
        PromptDone d = pr->done;
        void *data = pr->data;
        prompt_reset();
        editor_layout();
        d(NULL, 1, data);
        return;
    }
    if (pr->kind == PROMPT_YN && pr->yn) {
        YNDone d = pr->yn;
        void *data = pr->data;
        prompt_reset();
        editor_layout();
        d(0, 1, data);
        return;
    }
    prompt_reset();
    editor_layout();
}

/* ---------------- history ---------------- */

void history_add(int kind, const char *s) {
    int i;
    if (!s || !*s) return;
    /* drop an older duplicate */
    for (i = 0; i < E.nhistory[kind]; i++) {
        if (!strcmp(E.history[kind][i], s)) {
            free(E.history[kind][i]);
            memmove(E.history[kind] + i, E.history[kind] + i + 1,
                    sizeof(char *) * (size_t)(E.nhistory[kind] - i - 1));
            E.nhistory[kind]--;
            break;
        }
    }
    E.history[kind] = xrealloc(E.history[kind], sizeof(char *) * (size_t)(E.nhistory[kind] + 1));
    E.history[kind][E.nhistory[kind]++] = xstrdup(s);
    if (E.nhistory[kind] > 200) {
        free(E.history[kind][0]);
        memmove(E.history[kind], E.history[kind] + 1, sizeof(char *) * (size_t)(E.nhistory[kind] - 1));
        E.nhistory[kind]--;
    }
}

static char *history_path(void) {
    return config_dir ? xsprintf("%s/buffers/history.txt", config_dir) : NULL;
}

void history_load(void) {
    char *path = history_path(), *text, *line, *next;
    if (!path || !GOPT(O_SAVEHISTORY)) { free(path); return; }
    text = read_file(path, NULL);
    free(path);
    if (!text) return;
    for (line = text; line && *line; line = next) {
        char *e = strchr(line, '\n'), *tab;
        int k;
        next = e ? e + 1 : NULL;
        if (e) *e = 0;
        tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        for (k = 0; k < HIST_N; k++)
            if (!strcmp(line, hist_names[k])) history_add(k, tab + 1);
    }
    free(text);
}

void history_save(void) {
    char *path, *dir;
    FILE *f;
    int k, i;
    if (!config_dir || !GOPT(O_SAVEHISTORY)) return;
    dir = xsprintf("%s/buffers", config_dir);
    if (!plat_is_dir(config_dir)) { free(dir); return; }
    plat_mkdir_p(dir);
    free(dir);
    path = history_path();
    f = plat_fopen(path, "w");
    free(path);
    if (!f) return;
    for (k = 0; k < HIST_N; k++)
        for (i = 0; i < E.nhistory[k]; i++)
            if (!strchr(E.history[k][i], '\n')) fprintf(f, "%s\t%s\n", hist_names[k], E.history[k][i]);
    fclose(f);
}

/* ---------------- editing helpers ---------------- */

static int has_sel(Prompt *pr) { return pr->anchor >= 0 && pr->anchor != pr->cx; }

static void sel_range(Prompt *pr, int *a, int *b) {
    *a = pr->anchor < pr->cx ? pr->anchor : pr->cx;
    *b = pr->anchor < pr->cx ? pr->cx : pr->anchor;
}

static void delete_sel(Prompt *pr) {
    int a, b;
    if (!has_sel(pr)) return;
    sel_range(pr, &a, &b);
    str_delete(&pr->text, a, b - a);
    pr->cx = a;
    pr->anchor = -1;
}

static void insert(Prompt *pr, const char *s, int n) {
    delete_sel(pr);
    str_insert(&pr->text, pr->cx, s, n);
    pr->cx += n;
}

static int word_left(Prompt *pr, int x) {
    const char *s = pr->text.s;
    int cp;
    while (x > 0) {
        int p = utf8_prev(s, x);
        utf8_decode(s + p, pr->text.len - p, &cp);
        if (is_word_char(cp)) break;
        x = p;
    }
    while (x > 0) {
        int p = utf8_prev(s, x);
        utf8_decode(s + p, pr->text.len - p, &cp);
        if (!is_word_char(cp)) break;
        x = p;
    }
    return x;
}

static int word_right(Prompt *pr, int x) {
    const char *s = pr->text.s;
    int cp, len = pr->text.len;
    while (x < len) {
        utf8_decode(s + x, len - x, &cp);
        if (is_word_char(cp)) break;
        x = utf8_next(s, len, x);
    }
    while (x < len) {
        utf8_decode(s + x, len - x, &cp);
        if (!is_word_char(cp)) break;
        x = utf8_next(s, len, x);
    }
    return x;
}

static void move_to(Prompt *pr, int x, int select) {
    if (select) {
        if (pr->anchor < 0) pr->anchor = pr->cx;
    } else {
        pr->anchor = -1;
    }
    pr->cx = x;
}

/* ---------------- completion ---------------- */

char **file_complete(const char *arg, int *n);

static void do_complete(Prompt *pr, int back) {
    if (pr->ncomp > 0) {
        int len = pr->cx - pr->comp_start;
        pr->comp_idx = back ? (pr->comp_idx - 1 + pr->ncomp) % pr->ncomp : (pr->comp_idx + 1) % pr->ncomp;
        str_delete(&pr->text, pr->comp_start, len);
        str_insert(&pr->text, pr->comp_start, pr->completions[pr->comp_idx],
                   (int)strlen(pr->completions[pr->comp_idx]));
        pr->cx = pr->comp_start + (int)strlen(pr->completions[pr->comp_idx]);
        return;
    }
    if (back) return;
    {
        int start = 0, n = 0;
        char **c;
        char *upto = xstrndup(pr->text.s ? pr->text.s : "", (size_t)pr->cx);
        if (pr->hist == HIST_COMMAND) {
            c = command_complete(upto, pr->cx, &start);
        } else {
            c = file_complete(upto, &n);
            start = 0;
        }
        free(upto);
        if (!c) return;
        for (n = 0; c[n]; n++)
            ;
        if (n == 0) { free(c); return; }
        pr->completions = c;
        pr->ncomp = n;
        pr->comp_start = start;
        pr->comp_idx = -1;
        do_complete(pr, 0);
        if (n == 1) comp_clear(pr);
    }
}

/* ---------------- event handling ---------------- */

static void finish(int canceled) {
    Prompt *pr = &E.prompt;
    PromptDone d = pr->done;
    void *data = pr->data;
    char *resp = xstrdup(pr->text.s ? pr->text.s : "");
    int hist = pr->hist;
    if (!canceled) history_add(hist, resp);
    prompt_reset();
    editor_layout();
    if (d) d(canceled ? NULL : resp, canceled, data);
    free(resp);
}

static void text_changed(void) {
    Prompt *pr = &E.prompt;
    if (pr->changed) pr->changed(pr->text.s ? pr->text.s : "", pr->data);
}

int prompt_handle(InEvent *ev) {
    Prompt *pr = &E.prompt;
    const char *act;
    if (pr->kind == PROMPT_YN) {
        YNDone d = pr->yn;
        void *data = pr->data;
        if (ev->type != IN_KEY || !d) return 1;
        if (ev->mods == 0 && (ev->key == 'y' || ev->key == 'Y')) {
            prompt_reset(); editor_layout(); d(1, 0, data);
        } else if (ev->mods == 0 && (ev->key == 'n' || ev->key == 'N')) {
            prompt_reset(); editor_layout(); d(0, 0, data);
        } else if (ev->key == KEY_ESC || (ev->mods == MOD_CTRL && (ev->key == 'c' || ev->key == 'q'))) {
            prompt_reset(); editor_layout(); d(0, 1, data);
        }
        return 1;
    }
    if (ev->type == IN_PASTE) {
        char *s = xstrndup(ev->paste, (size_t)ev->pastelen), *p;
        for (p = s; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
        insert(pr, s, (int)strlen(s));
        free(s);
        comp_clear(pr);
        text_changed();
        return 1;
    }
    if (ev->type == IN_MOUSE) {
        act = binding_for_event(ev, 1);
        if (act && (!strcmp(act, "HistoryUp") || !strcmp(act, "HistoryDown"))) goto dispatch;
        return 1;
    }
    if (ev->type != IN_KEY) return 1;
    act = binding_for_event(ev, 1);
    if (!act) {
        if (ev->key < KEY_SPECIAL && !(ev->mods & (MOD_CTRL | MOD_ALT))) {
            char buf[4];
            int n = utf8_encode(ev->key, buf);
            insert(pr, buf, n);
            comp_clear(pr);
            text_changed();
        }
        return 1;
    }
dispatch: {
    /* support chained actions like "A|B" by taking the first known one */
    char name[64];
    const char *s = act;
    size_t n = strcspn(s, ",|&");
    int changed = 0, keepcomp = 0;
    int len = pr->text.len;
    if (n >= sizeof name) n = sizeof name - 1;
    memcpy(name, s, n);
    name[n] = 0;
    if (!strcmp(name, "ExecuteCommand")) { finish(0); return 1; }
    if (!strcmp(name, "AbortCommand")) { finish(1); return 1; }
    if (!strcmp(name, "CursorLeft")) {
        if (has_sel(pr)) move_to(pr, pr->anchor < pr->cx ? pr->anchor : pr->cx, 0);
        else if (pr->cx > 0) move_to(pr, utf8_prev(pr->text.s, pr->cx), 0);
    } else if (!strcmp(name, "CursorRight")) {
        if (has_sel(pr)) move_to(pr, pr->anchor > pr->cx ? pr->anchor : pr->cx, 0);
        else if (pr->cx < len) move_to(pr, utf8_next(pr->text.s, len, pr->cx), 0);
    } else if (!strcmp(name, "SelectLeft")) {
        if (pr->cx > 0) move_to(pr, utf8_prev(pr->text.s, pr->cx), 1);
    } else if (!strcmp(name, "SelectRight")) {
        if (pr->cx < len) move_to(pr, utf8_next(pr->text.s, len, pr->cx), 1);
    } else if (!strcmp(name, "WordLeft")) {
        move_to(pr, word_left(pr, pr->cx), 0);
    } else if (!strcmp(name, "WordRight")) {
        move_to(pr, word_right(pr, pr->cx), 0);
    } else if (!strcmp(name, "SelectWordLeft")) {
        move_to(pr, word_left(pr, pr->cx), 1);
    } else if (!strcmp(name, "SelectWordRight")) {
        move_to(pr, word_right(pr, pr->cx), 1);
    } else if (!strcmp(name, "StartOfTextToggle") || !strcmp(name, "StartOfLine") ||
               !strcmp(name, "StartOfText") || !strcmp(name, "CursorStart")) {
        move_to(pr, 0, 0);
    } else if (!strcmp(name, "EndOfLine") || !strcmp(name, "CursorEnd")) {
        move_to(pr, len, 0);
    } else if (!strcmp(name, "SelectToStartOfTextToggle") || !strcmp(name, "SelectToStart") ||
               !strcmp(name, "SelectUp")) {
        move_to(pr, 0, 1);
    } else if (!strcmp(name, "SelectToEndOfLine") || !strcmp(name, "SelectToEnd") ||
               !strcmp(name, "SelectDown")) {
        move_to(pr, len, 1);
    } else if (!strcmp(name, "Backspace")) {
        if (has_sel(pr)) delete_sel(pr);
        else if (pr->cx > 0) {
            int p = utf8_prev(pr->text.s, pr->cx);
            str_delete(&pr->text, p, pr->cx - p);
            pr->cx = p;
        } else if (len == 0 && pr->hist == HIST_COMMAND) {
            /* micro: backspace on an empty command bar closes it */
            finish(1);
            return 1;
        }
        changed = 1;
    } else if (!strcmp(name, "Delete")) {
        if (has_sel(pr)) delete_sel(pr);
        else if (pr->cx < len) {
            int nx = utf8_next(pr->text.s, len, pr->cx);
            str_delete(&pr->text, pr->cx, nx - pr->cx);
        }
        changed = 1;
    } else if (!strcmp(name, "DeleteWordLeft")) {
        int w = word_left(pr, pr->cx);
        str_delete(&pr->text, w, pr->cx - w);
        pr->cx = w;
        pr->anchor = -1;
        changed = 1;
    } else if (!strcmp(name, "HistoryUp") || !strcmp(name, "HistoryDown")) {
        int up = !strcmp(name, "HistoryUp");
        int nh = E.nhistory[pr->hist];
        if (pr->hist_pos == nh && up) {
            free(pr->hist_saved);
            pr->hist_saved = xstrdup(pr->text.s ? pr->text.s : "");
        }
        if (up && pr->hist_pos > 0) pr->hist_pos--;
        else if (!up && pr->hist_pos < nh) pr->hist_pos++;
        else return 1;
        str_clear(&pr->text);
        str_puts(&pr->text, pr->hist_pos < nh ? E.history[pr->hist][pr->hist_pos]
                                              : (pr->hist_saved ? pr->hist_saved : ""));
        pr->cx = pr->text.len;
        pr->anchor = -1;
        changed = 1;
    } else if (!strcmp(name, "CommandComplete") || !strcmp(name, "Autocomplete")) {
        do_complete(pr, 0);
        keepcomp = 1;
        changed = 1;
    } else if (!strcmp(name, "CycleAutocompleteBack")) {
        do_complete(pr, 1);
        keepcomp = 1;
        changed = 1;
    } else if (!strcmp(name, "Copy")) {
        if (has_sel(pr)) {
            int a, b;
            sel_range(pr, &a, &b);
            clipboard_set(pr->text.s + a, b - a, 0);
        }
    } else if (!strcmp(name, "Cut")) {
        if (has_sel(pr)) {
            int a, b;
            sel_range(pr, &a, &b);
            clipboard_set(pr->text.s + a, b - a, 0);
            delete_sel(pr);
            changed = 1;
        }
    } else if (!strcmp(name, "CutLine")) {
        clipboard_set(pr->text.s ? pr->text.s : "", len, 0);
        str_clear(&pr->text);
        pr->cx = 0;
        pr->anchor = -1;
        changed = 1;
    } else if (!strcmp(name, "Paste") || !strcmp(name, "PastePrimary")) {
        int cl;
        char *c = clipboard_get(&cl), *p;
        for (p = c; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
        insert(pr, c, (int)strlen(c));
        free(c);
        changed = 1;
    } else if (!strcmp(name, "SelectAll")) {
        pr->anchor = 0;
        pr->cx = len;
    }
    if (!keepcomp) comp_clear(pr);
    if (changed) text_changed();
    return 1;
}
}
