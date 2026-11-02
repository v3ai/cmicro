/*
 * bindings.c - micro's default key bindings, bindings.json, bind/unbind.
 */
#include "micro.h"
#include "conf.h"
#include "runtime.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    int key, mods;
    char *keyname;
    char *action;
} Binding;

typedef struct {
    Binding *b;
    int n, cap;
} BindTable;

static BindTable buftab, cmdtab;

static const char *const bufdefaults[][2] = {
    {"Up", "CursorUp"}, {"Down", "CursorDown"}, {"Right", "CursorRight"}, {"Left", "CursorLeft"},
    {"ShiftUp", "SelectUp"}, {"ShiftDown", "SelectDown"}, {"ShiftLeft", "SelectLeft"},
    {"ShiftRight", "SelectRight"}, {"CtrlLeft", "WordLeft"}, {"CtrlRight", "WordRight"},
    {"AltUp", "MoveLinesUp"}, {"AltDown", "MoveLinesDown"},
    {"CtrlShiftRight", "SelectWordRight"}, {"CtrlShiftLeft", "SelectWordLeft"},
    {"AltLeft", "StartOfTextToggle"}, {"AltRight", "EndOfLine"},
    {"AltShiftLeft", "SelectToStartOfTextToggle"}, {"ShiftHome", "SelectToStartOfTextToggle"},
    {"AltShiftRight", "SelectToEndOfLine"}, {"ShiftEnd", "SelectToEndOfLine"},
    {"CtrlUp", "CursorStart"}, {"CtrlDown", "CursorEnd"}, {"CtrlShiftUp", "SelectToStart"},
    {"CtrlShiftDown", "SelectToEnd"}, {"Alt-{", "ParagraphPrevious"}, {"Alt-}", "ParagraphNext"},
    {"Enter", "InsertNewline"}, {"CtrlH", "Backspace"}, {"Backspace", "Backspace"},
    {"Alt-CtrlH", "DeleteWordLeft"}, {"Alt-Backspace", "DeleteWordLeft"},
    {"Tab", "Autocomplete|IndentSelection|InsertTab"},
    {"Backtab", "CycleAutocompleteBack|OutdentSelection|OutdentLine"},
    {"Ctrl-o", "OpenFile"}, {"Ctrl-s", "Save"}, {"Ctrl-f", "Find"}, {"Alt-F", "FindLiteral"},
    {"Ctrl-n", "FindNext"}, {"Ctrl-p", "FindPrevious"}, {"Alt-[", "DiffPrevious|CursorStart"},
    {"Alt-]", "DiffNext|CursorEnd"}, {"Ctrl-z", "Undo"}, {"Ctrl-y", "Redo"},
    {"Ctrl-c", "Copy|CopyLine"}, {"Ctrl-x", "Cut|CutLine"}, {"Ctrl-k", "CutLine"},
    {"Ctrl-d", "Duplicate|DuplicateLine"}, {"Ctrl-v", "Paste"}, {"Ctrl-a", "SelectAll"},
    {"Ctrl-t", "AddTab"}, {"Alt-,", "PreviousTab|LastTab"}, {"Alt-.", "NextTab|FirstTab"},
    {"Home", "StartOfTextToggle"}, {"End", "EndOfLine"}, {"CtrlHome", "CursorStart"},
    {"CtrlEnd", "CursorEnd"}, {"CtrlShiftHome", "SelectToStart"}, {"CtrlShiftEnd", "SelectToEnd"},
    {"PageUp", "CursorPageUp"}, {"PageDown", "CursorPageDown"},
    {"CtrlPageUp", "PreviousTab|LastTab"}, {"CtrlPageDown", "NextTab|FirstTab"},
    {"ShiftPageUp", "SelectPageUp"}, {"ShiftPageDown", "SelectPageDown"},
    {"Ctrl-g", "ToggleHelp"}, {"Alt-g", "ToggleKeyMenu"}, {"Ctrl-r", "ToggleRuler"},
    {"Ctrl-l", "command-edit:goto "}, {"Delete", "Delete"}, {"Ctrl-b", "ShellMode"},
    {"Ctrl-q", "Quit"}, {"Ctrl-e", "CommandMode"}, {"Ctrl-w", "NextSplit|FirstSplit"},
    {"Ctrl-u", "ToggleMacro"}, {"Ctrl-j", "PlayMacro"}, {"Insert", "ToggleOverwriteMode"},
    {"Alt-f", "WordRight"}, {"Alt-b", "WordLeft"}, {"Alt-a", "StartOfText"}, {"Alt-e", "EndOfLine"},
    {"F2", "Save"}, {"F3", "Find"}, {"F4", "Quit"}, {"F7", "Find"}, {"F10", "Quit"},
    {"Esc", "Escape,Deselect,ClearInfo,RemoveAllMultiCursors,UnhighlightSearch"},
    {"MouseWheelUp", "ScrollUp"}, {"MouseWheelDown", "ScrollDown"}, {"MouseLeft", "MousePress"},
    {"MouseLeftDrag", "MouseDrag"}, {"MouseLeftRelease", "MouseRelease"},
    {"MouseMiddle", "PastePrimary"}, {"Ctrl-MouseLeft", "MouseMultiCursor"},
    {"Alt-n", "SpawnMultiCursor"}, {"Alt-m", "SpawnMultiCursorSelect"},
    {"AltShiftUp", "SpawnMultiCursorUp"}, {"AltShiftDown", "SpawnMultiCursorDown"},
    {"Alt-p", "RemoveMultiCursor"}, {"Alt-c", "RemoveAllMultiCursors"}, {"Alt-x", "SkipMultiCursor"},
};

static const char *const infodefaults[][2] = {
    {"Up", "HistoryUp"}, {"Down", "HistoryDown"}, {"Right", "CursorRight"}, {"Left", "CursorLeft"},
    {"ShiftUp", "SelectUp"}, {"ShiftDown", "SelectDown"}, {"ShiftLeft", "SelectLeft"},
    {"ShiftRight", "SelectRight"}, {"AltLeft", "StartOfTextToggle"}, {"AltRight", "EndOfLine"},
    {"AltUp", "CursorStart"}, {"AltDown", "CursorEnd"}, {"AltShiftRight", "SelectWordRight"},
    {"AltShiftLeft", "SelectWordLeft"}, {"CtrlLeft", "WordLeft"}, {"CtrlRight", "WordRight"},
    {"CtrlShiftLeft", "SelectToStartOfTextToggle"}, {"ShiftHome", "SelectToStartOfTextToggle"},
    {"CtrlShiftRight", "SelectToEndOfLine"}, {"ShiftEnd", "SelectToEndOfLine"},
    {"CtrlUp", "CursorStart"}, {"CtrlDown", "CursorEnd"}, {"CtrlShiftUp", "SelectToStart"},
    {"CtrlShiftDown", "SelectToEnd"}, {"Enter", "ExecuteCommand"}, {"CtrlH", "Backspace"},
    {"Backspace", "Backspace"}, {"Alt-CtrlH", "DeleteWordLeft"}, {"Alt-Backspace", "DeleteWordLeft"},
    {"Tab", "CommandComplete"}, {"Backtab", "CycleAutocompleteBack"}, {"Ctrl-z", "Undo"},
    {"Ctrl-y", "Redo"}, {"Ctrl-c", "Copy"}, {"Ctrl-x", "Cut"}, {"Ctrl-k", "CutLine"},
    {"Ctrl-v", "Paste"}, {"Home", "StartOfTextToggle"}, {"End", "EndOfLine"},
    {"CtrlHome", "CursorStart"}, {"CtrlEnd", "CursorEnd"}, {"CtrlShiftHome", "SelectToStart"},
    {"CtrlShiftEnd", "SelectToEnd"}, {"Delete", "Delete"}, {"Ctrl-q", "AbortCommand"},
    {"Ctrl-e", "EndOfLine"}, {"Ctrl-a", "StartOfLine"}, {"Ctrl-w", "DeleteWordLeft"},
    {"Insert", "ToggleOverwriteMode"}, {"Ctrl-b", "WordLeft"}, {"Ctrl-f", "WordRight"},
    {"Ctrl-d", "DeleteWordLeft"}, {"Ctrl-m", "ExecuteCommand"}, {"Ctrl-n", "HistoryDown"},
    {"Ctrl-p", "HistoryUp"}, {"Ctrl-u", "SelectToStart"}, {"Alt-f", "WordRight"},
    {"Alt-b", "WordLeft"}, {"Alt-a", "StartOfText"}, {"Alt-e", "EndOfLine"},
    {"F10", "AbortCommand"}, {"Esc", "AbortCommand"}, {"MouseWheelUp", "HistoryUp"},
    {"MouseWheelDown", "HistoryDown"}, {"MouseLeft", "MousePress"}, {"MouseLeftDrag", "MouseDrag"},
    {"MouseLeftRelease", "MouseRelease"}, {"MouseMiddle", "PastePrimary"},
};

static Binding *table_find(BindTable *t, int key, int mods) {
    int i;
    for (i = t->n - 1; i >= 0; i--)
        if (t->b[i].key == key && t->b[i].mods == mods) return &t->b[i];
    return NULL;
}

static int table_set(BindTable *t, const char *keyname, const char *action) {
    int key, mods;
    Binding *b;
    if (!key_parse(keyname, &key, &mods)) return -1;
    b = table_find(t, key, mods);
    if (b) {
        free(b->action);
        free(b->keyname);
        b->action = xstrdup(action);
        b->keyname = xstrdup(keyname);
        return 0;
    }
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 128;
        t->b = xrealloc(t->b, sizeof *t->b * (size_t)t->cap);
    }
    t->b[t->n].key = key;
    t->b[t->n].mods = mods;
    t->b[t->n].keyname = xstrdup(keyname);
    t->b[t->n].action = xstrdup(action);
    t->n++;
    return 0;
}

static int table_unset(BindTable *t, const char *keyname) {
    int key, mods, i;
    if (!key_parse(keyname, &key, &mods)) return -1;
    for (i = 0; i < t->n; i++) {
        if (t->b[i].key == key && t->b[i].mods == mods) {
            free(t->b[i].action);
            free(t->b[i].keyname);
            memmove(t->b + i, t->b + i + 1, sizeof *t->b * (size_t)(t->n - i - 1));
            t->n--;
            return 0;
        }
    }
    return 0;
}

void bindings_init(void) {
    size_t i;
    for (i = 0; i < sizeof bufdefaults / sizeof bufdefaults[0]; i++)
        table_set(&buftab, bufdefaults[i][0], bufdefaults[i][1]);
    for (i = 0; i < sizeof infodefaults / sizeof infodefaults[0]; i++)
        table_set(&cmdtab, infodefaults[i][0], infodefaults[i][1]);
    if (GOPT(O_COMMENT)) {
        table_set(&buftab, "Alt-/", "lua:comment.comment");
        table_set(&buftab, "CtrlUnderscore", "lua:comment.comment");
    }
}

static char *bindings_path(void) {
    return config_dir ? xsprintf("%s/bindings.json", config_dir) : NULL;
}

static void apply_map(BindTable *t, CNode *m) {
    int i;
    for (i = 0; i < m->n; i++) {
        CNode *v = m->vals[i];
        if (v->type != CN_STR) continue;
        if (table_set(t, m->keys[i], v->str) != 0)
            editor_set_error("Error reading bindings.json: invalid key: %s", m->keys[i]);
    }
}

void bindings_load_user(void) {
    char *path = bindings_path(), *text, *err = NULL;
    CNode *root;
    int i;
    if (!path) return;
    text = read_file(path, NULL);
    free(path);
    if (!text) return;
    root = json_parse(text, &err);
    free(text);
    if (err) {
        editor_set_error("Error reading bindings.json: %s", err);
        free(err);
    }
    if (root && root->type == CN_MAP) {
        for (i = 0; i < root->n; i++) {
            CNode *v = root->vals[i];
            if (v->type == CN_MAP) {
                if (!strcmp(root->keys[i], "buffer")) apply_map(&buftab, v);
                else if (!strcmp(root->keys[i], "command")) apply_map(&cmdtab, v);
            } else if (v->type == CN_STR) {
                if (table_set(&buftab, root->keys[i], v->str) != 0)
                    editor_set_error("Error reading bindings.json: invalid key: %s", root->keys[i]);
            }
        }
    }
    cnode_free(root);
}

static int save_binding(const char *keyname, const char *action) {
    char *path = bindings_path(), *text;
    CNode *root = NULL;
    int rc;
    if (!path) return -1;
    text = read_file(path, NULL);
    if (text) {
        root = json_parse(text, NULL);
        free(text);
    }
    if (!root || root->type != CN_MAP) {
        cnode_free(root);
        root = cnode_new(CN_MAP);
    }
    if (action) {
        CNode *v = cnode_new(CN_STR);
        v->str = xstrdup(action);
        cnode_set(root, keyname, v);
    } else {
        cnode_del(root, keyname);
    }
    rc = json_write_file(path, root);
    cnode_free(root);
    free(path);
    return rc;
}

int bind_key(const char *keyname, const char *action, int save) {
    if (GOPT(O_LOCKBINDINGS)) return -1;
    if (table_set(&buftab, keyname, action) != 0) return -1;
    if (save) save_binding(keyname, action);
    return 0;
}

int unbind_key(const char *keyname, int save) {
    size_t i;
    if (GOPT(O_LOCKBINDINGS)) return -1;
    table_unset(&buftab, keyname);
    /* restore the default, as micro does */
    for (i = 0; i < sizeof bufdefaults / sizeof bufdefaults[0]; i++) {
        int k1, m1, k2, m2;
        if (key_parse(bufdefaults[i][0], &k1, &m1) && key_parse(keyname, &k2, &m2) && k1 == k2 && m1 == m2)
            table_set(&buftab, bufdefaults[i][0], bufdefaults[i][1]);
    }
    if (save) save_binding(keyname, NULL);
    return 0;
}

const char *binding_for_event(InEvent *ev, int prompt) {
    BindTable *t = prompt ? &cmdtab : &buftab;
    Binding *b = table_find(t, ev->key, ev->mods);
    if (!b && ev->key < KEY_SPECIAL && (ev->mods & MOD_SHIFT)) b = table_find(t, ev->key, ev->mods & ~MOD_SHIFT);
    return b ? b->action : NULL;
}

const char *binding_key_for_action(const char *action) {
    int i;
    for (i = 0; i < buftab.n; i++)
        if (!strcmp(buftab.b[i].action, action)) return buftab.b[i].keyname;
    return NULL;
}
