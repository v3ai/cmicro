/*
 * micro.h - shared types and interfaces for the editor.
 */
#ifndef MICRO_H
#define MICRO_H

#include "colors.h"
#include "regex.h"
#include "syntax.h"
#include "util.h"

#define MICRO_VERSION "2.0.14-c"

struct Buffer;
struct Tab;

/* ================================================================== */
/* Locations                                                           */

typedef struct {
    int x, y; /* x: byte offset within line (always on a UTF-8 boundary) */
} Loc;

int cm_loc_cmp(Loc a, Loc b);
Loc cm_loc_make(int x, int y);

/* ================================================================== */
/* Options                                                             */

enum { OT_BOOL, OT_INT, OT_STR };

enum {
    /* buffer-local options */
    O_AUTOCLOSE, O_AUTOINDENT, O_AUTOSU, O_BACKUP, O_BACKUPDIR, O_BASENAME,
    O_COLORCOLUMN, O_COMMENTTYPE, O_CURSORLINE, O_DETECTLIMIT, O_DIFFGUTTER,
    O_ENCODING, O_EOFNEWLINE, O_FASTDIRTY, O_FILEFORMAT, O_FILETYPE, O_HLSEARCH,
    O_HLTABERRORS, O_HLTRAILINGWS, O_IGNORECASE, O_INCSEARCH, O_INDENTCHAR,
    O_KEEPAUTOINDENT, O_MATCHBRACE, O_MATCHBRACELEFT, O_MATCHBRACESTYLE,
    O_MKPARENTS, O_PAGEOVERLAP, O_PERMBACKUP, O_READONLY, O_RELATIVERULER,
    O_RELOAD, O_RMTRAILINGWS, O_RULER, O_SAVECURSOR, O_SAVEUNDO, O_SCROLLBAR,
    O_SCROLLMARGIN, O_SCROLLSPEED, O_SHOWCHARS, O_SMARTPASTE, O_SOFTWRAP,
    O_SPLITBOTTOM, O_SPLITRIGHT, O_STATUSFORMATL, O_STATUSFORMATR, O_STATUSLINE,
    O_SYNTAX, O_TABMOVEMENT, O_TABSIZE, O_TABSTOSPACES, O_USEPRIMARY, O_WORDWRAP,
    /* global-only options */
    O_AUTOSAVE, O_CLIPBOARD, O_COLORSCHEME, O_COMMENT, O_DIVCHARS, O_DIVREVERSE,
    O_FAKECURSOR, O_FTOPTIONS, O_HELPSPLIT, O_INFOBAR, O_KEYMENU, O_LOCKBINDINGS,
    O_MOUSE, O_MULTIOPEN, O_PARSECURSOR, O_PASTE, O_SAVEHISTORY, O_SCROLLBARCHAR,
    O_SUCMD, O_TABALWAYS, O_TABHIGHLIGHT, O_TABREVERSE, O_TRUECOLOR, O_XTERM,
    O_COUNT
};
#define O_FIRST_GLOBAL O_AUTOSAVE

typedef struct {
    const char *name;
    int type;
    const char *def;
    const char *const *choices; /* NULL-terminated or NULL */
    int nonneg;                 /* int must be >= 0 (1) or > 0 (2) */
} OptDef;

typedef struct {
    int i;   /* bool / int */
    char *s; /* string */
} OptVal;

extern const OptDef cm_opt_defs[O_COUNT];
extern OptVal cm_gopts[O_COUNT];

#define GOPT(id) (cm_gopts[id].i)
#define GOPTS(id) (cm_gopts[id].s)

int   cm_opt_find(const char *name);
int   cm_opt_parse(int id, const char *value, OptVal *out, char **err);
char *cm_opt_to_string(int id, const OptVal *v);
void  cm_opt_copy(int id, OptVal *dst, const OptVal *src);
void  cm_options_init_defaults(void);
void  cm_settings_load(void);      /* settings.json */
int   cm_settings_save(void);      /* write non-default globals to settings.json */
void  cm_settings_note_set(const char *name, const char *value); /* remember for saving */
void  cm_settings_note_reset(const char *name);
void  cm_settings_apply_parsed(struct Buffer *b, const char *path); /* ft: and glob: sections */

/* ================================================================== */
/* Buffer                                                              */

typedef struct Line {
    char *s;
    int len, cap;
    Region *state;          /* region open at end of this line */
    Region *hl_start;       /* start state used for hl[] */
    unsigned short *hl;     /* per-byte highlight groups */
    int hlcap;
    int match_valid;
} Line;

enum { TE_INSERT, TE_REMOVE };

typedef struct TextEvent {
    int type;
    Loc start, end;
    char *text;
    int len;
    p_s64 stamp; /* cm_plat_now_ms() when recorded */
    int id;
    Loc cursor;  /* position of the editing cursor before the event */
    int cnum;    /* index of that cursor (micro's t.C.Num) */
} TextEvent;

typedef struct {
    TextEvent *ev;
    int n, cap;
} EvStack;

enum { BT_DEFAULT, BT_HELP, BT_LOG, BT_SCRATCH, BT_RAW };

typedef struct Buffer {
    Line *lines;
    int nlines, cap;
    char *path;      /* path as given, "" for a new unnamed buffer */
    char *abspath;
    char *name;      /* display name */
    int type;
    OptVal opts[O_COUNT];
    unsigned char local[O_COUNT];
    Syntax *syn;
    int states_valid; /* lines [0, states_valid) have valid end states */
    EvStack undo, redo;
    int saved_id, cur_id, next_id;
    int recording;   /* record undo events */
    char *last_search;
    int last_search_regex;
    int hl_search;
    p_s64 mtime;
    int refs;
    int is_new;
    int bom;
    p_s64 autosave_at;
} Buffer;

#define BOPT(b, id) ((b)->opts[id].i)
#define BOPTS(b, id) ((b)->opts[id].s)

Buffer *cm_buf_new_empty(void);
Buffer *cm_buf_from_string(const char *name, const char *text, int type);
Buffer *cm_buf_open(const char *path, char **err);
void    cm_buf_free(Buffer *b);
int     cm_buf_save(Buffer *b, const char *path, char **err);
int     cm_buf_reload(Buffer *b, char **err);
int     cm_buf_modified(const Buffer *b);
void    cm_buf_set_name(Buffer *b, const char *path);
void    cm_buf_set_text(Buffer *b, const char *text, int len);

Loc   cm_buf_insert(Buffer *b, Loc at, const char *text, int len);
void  cm_buf_remove(Buffer *b, Loc a, Loc z);
char *cm_buf_get_text(const Buffer *b, Loc a, Loc z, int *len);
Loc   cm_buf_replace(Buffer *b, Loc a, Loc z, const char *text, int len);
Loc   cm_buf_end(const Buffer *b);
Loc   cm_buf_clamp(const Buffer *b, Loc l);
Loc   cm_buf_move_chars(const Buffer *b, Loc l, int n); /* +/- characters across lines */
int   cm_buf_char_at(const Buffer *b, Loc l);          /* code point or -1 at EOL */
char *cm_buf_full_text(const Buffer *b, int *len);
void  cm_buf_update_rules(Buffer *b);                  /* filetype detection */
void  cm_buf_invalidate_hl(Buffer *b, int from);
void  cm_buf_ensure_hl(Buffer *b, int line);           /* highlight line (and states before) */
int   cm_buf_undo(Buffer *b);
int   cm_buf_redo(Buffer *b);
void  cm_buf_mark_saved(Buffer *b);
void  cm_buf_set_option(Buffer *b, int id, const OptVal *v, int local);
void  cm_buf_option_changed(Buffer *b, int id);
const char *cm_buf_leading_ws(const Buffer *b, int y, int *len);
char *cm_buf_indent_string(const Buffer *b);

extern void (*cm_buf_edit_hook)(Buffer *b, int type, Loc start, Loc end);
extern Loc (*cm_buf_get_cursor_hook)(Buffer *b, int *num); /* editing cursor for undo records */
extern void (*cm_buf_set_cursor_hook)(Buffer *b, Loc l, int num);

/* ================================================================== */
/* Cursors and panes                                                   */

typedef struct {
    Loc loc;
    int lastvx;     /* remembered visual column for up/down */
    Loc sel[2];     /* current selection (sel[0] <= sel[1]) */
    Loc orig[2];    /* anchor; orig[0]..orig[1] for word/line selection */
} Cursor;

int  cm_cur_has_sel(const Cursor *c);
void cm_cur_deselect(Cursor *c, int start);
void cm_cur_set_sel_to(Cursor *c, Loc l);

enum { DRAG_CHAR, DRAG_WORD, DRAG_LINE };

typedef struct Pane {
    Buffer *buf;
    Cursor *cur;
    int ncur, ccap, active;
    int x, y, w, h;    /* full rect, statusline included */
    int first_line, first_row;   /* first visible line and softwrap row within it */
    int left;          /* horizontal scroll in columns */
    int overwrite;
    p_s64 last_click;
    int clicks;
    Loc click_loc;
    int dragging, drag_mode;
    int multi_word;    /* SpawnMultiCursor searching whole words */
    Loc search_origin; /* for incremental search */
    int search_origin_top;
    struct Tab *tab;
} Pane;

#define ACUR(p) (&(p)->cur[(p)->active])

Pane *cm_pane_new(Buffer *b);
void  cm_pane_free(Pane *p);
void  cm_pane_set_buffer(Pane *p, Buffer *b);
int   cm_pane_add_cursor(Pane *p, Loc l);
void  cm_pane_remove_cursor(Pane *p, int i);
void  cm_pane_merge_cursors(Pane *p);
void  cm_pane_reset_cursors(Pane *p);

/* geometry */
int  cm_pane_gutter_width(const Pane *p);
int  cm_pane_text_width(const Pane *p);
int  cm_pane_text_height(const Pane *p);
int  cm_pane_visual_x(const Pane *p, int y, int x);      /* byte offset -> column */
int  cm_pane_x_from_visual(const Pane *p, int y, int vx); /* column -> byte offset */
int  cm_pane_line_rows(const Pane *p, int y);            /* softwrap rows */
void cm_pane_loc_to_row(const Pane *p, Loc l, int *row, int *rvx);
Loc  cm_pane_row_to_loc(const Pane *p, int y, int row, int rvx);
void cm_pane_relocate(Pane *p);
void cm_pane_center(Pane *p);
void cm_pane_scroll(Pane *p, int n);
int  cm_pane_screen_to_loc(Pane *p, int sx, int sy, Loc *out);
void cm_cur_store_vx(Pane *p, Cursor *c);
void cm_cur_up(Pane *p, Cursor *c, int n);
void cm_cur_down(Pane *p, Cursor *c, int n);
void cm_cur_left(Pane *p, Cursor *c);
void cm_cur_right(Pane *p, Cursor *c);

/* ================================================================== */
/* Tabs and splits                                                     */

enum { NODE_LEAF, NODE_VSPLIT, NODE_HSPLIT };

typedef struct Node {
    int kind;
    Pane *pane;
    struct Node *a, *b, *parent;
    int ratio;         /* share of the first child, in thousandths (integer: exact everywhere) */
    int x, y, w, h;
} Node;

typedef struct Tab {
    Node *root;
    Pane *active;
} Tab;

Tab  *cm_tab_new(Pane *p);
void  cm_tab_free(Tab *t);
void  cm_tab_layout(Tab *t, int x, int y, int w, int h);
Pane *cm_tab_split(Tab *t, Pane *p, Buffer *b, int vertical);
void  cm_tab_close_pane(Tab *t, Pane *p); /* frees pane */
int   cm_tab_npanes(Tab *t);
Pane **cm_tab_panes(Tab *t, int *n);     /* malloc'd array in order */
Node *cm_tab_find_node(Node *n, Pane *p);
Pane *cm_tab_pane_at(Tab *t, int x, int y);

/* ================================================================== */
/* Input events                                                        */

enum {
    KEY_NONE = 0,
    KEY_SPECIAL = 0x200000,
    KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_HOME, KEY_END, KEY_PGUP, KEY_PGDN,
    KEY_INSERT, KEY_DELETE, KEY_BACKSPACE, KEY_TAB, KEY_BACKTAB, KEY_ENTER,
    KEY_ESC, KEY_F1, /* F1..F64 follow */
    KEY_MOUSE_FIRST = KEY_F1 + 64,
    KEY_MOUSE_LEFT = KEY_MOUSE_FIRST, KEY_MOUSE_MIDDLE, KEY_MOUSE_RIGHT,
    KEY_MOUSE_LEFT_DRAG, KEY_MOUSE_MIDDLE_DRAG, KEY_MOUSE_RIGHT_DRAG,
    KEY_MOUSE_LEFT_RELEASE, KEY_MOUSE_MIDDLE_RELEASE, KEY_MOUSE_RIGHT_RELEASE,
    KEY_WHEEL_UP, KEY_WHEEL_DOWN, KEY_WHEEL_LEFT, KEY_WHEEL_RIGHT,
    KEY_MOUSE_LAST
};

enum { MOD_SHIFT = 1, MOD_ALT = 2, MOD_CTRL = 4 };

enum { IN_NONE, IN_KEY, IN_MOUSE, IN_PASTE, IN_RESIZE };

typedef struct {
    int type;
    int key;   /* code point or KEY_* */
    int mods;
    int mx, my;
    char *paste;
    int pastelen;
    char raw[32];
    int rawlen;
} InEvent;

int   cm_input_poll(InEvent *ev, int timeout_ms);
void  cm_input_free(InEvent *ev);
int   cm_key_parse(const char *name, int *key, int *mods);
char *cm_key_name(int key, int mods);  /* malloc'd canonical name */

/* ================================================================== */
/* Screen                                                              */

void cm_scr_init(void);
void cm_scr_fini(void);
void cm_scr_resize(void);
int  cm_scr_w(void);
int  cm_scr_h(void);
void cm_scr_clear(void);
void cm_scr_set(int x, int y, int cp, Style st);
int  cm_scr_puts(int x, int y, const char *s, Style st, int maxw);
void cm_scr_fill(int x, int y, int w, int h, int cp, Style st);
Style cm_scr_get_style(int x, int y);
void cm_scr_set_style(int x, int y, Style st);
void cm_scr_show_cursor(int x, int y);
void cm_scr_hide_cursor(void);
void cm_scr_flush(void);
void cm_scr_invalidate(void);
void cm_scr_set_mouse(int on);
void cm_scr_osc52(const char *text, int len);
extern int cm_scr_truecolor;
int  cm_scr_detect_truecolor(const char *opt); /* "auto", "on" or "off" */

/* ================================================================== */
/* Editor state                                                        */

enum { PROMPT_NONE, PROMPT_TEXT, PROMPT_YN, PROMPT_RAW };
enum { HIST_COMMAND, HIST_FIND, HIST_SHELL, HIST_OPEN, HIST_GOTO, HIST_OTHER, HIST_N };

typedef void (*PromptDone)(const char *resp, int canceled, void *data);
typedef void (*PromptChanged)(const char *resp, void *data);
typedef void (*YNDone)(int yes, int canceled, void *data);

typedef struct {
    int kind;
    char *msg;
    Str text;
    int cx;          /* byte offset of cursor */
    int anchor;      /* selection anchor or -1 */
    int left;        /* scroll */
    int hist;        /* history kind */
    int hist_pos;
    char *hist_saved;
    PromptDone done;
    PromptChanged changed;
    YNDone yn;
    void *data;
    char **completions;
    int ncomp, comp_idx, comp_start;
    char *comp_base;
} Prompt;

typedef struct {
    Tab **tabs;
    int ntabs, curtab;
    int quit;
    char *msg;
    int msg_error;
    Prompt prompt;
    char **history[HIST_N];
    int nhistory[HIST_N];
    char *clipboard;
    int clipboard_len;
    int clipboard_lines; /* last copy was a whole-line copy (CopyLine/CutLine) */
    int recording_macro;
    struct MacroItem *macro;
    int nmacro, cmacro;
    int playing_macro;
    p_s64 last_autosave;
    int redraw;
    InEvent *cur_event;
    int cut_line_streak; /* consecutive CutLine appends to clipboard */
    int showkey;         /* "raw"/"showkey" waiting */
} Editor;

typedef struct MacroItem {
    int kind; /* 0 action string, 1 rune, 2 paste */
    char *s;
    int len;
    int rune;
} MacroItem;

extern Editor cm_E;

Tab  *cm_cur_tab(void);
Pane *cm_cur_pane(void);
void  cm_editor_add_tab(Pane *p);
void  cm_editor_close_tab(int i);
void  cm_editor_layout(void);
void  cm_editor_set_message(const char *fmt, ...);
void  cm_editor_set_error(const char *fmt, ...);
void  cm_editor_clear_message(void);
Pane *cm_editor_open_file_pane(const char *path, char **err);
Buffer *cm_editor_find_buffer(const char *path);
void  cm_editor_all_panes(void (*fn)(Pane *, void *), void *data);
void  cm_editor_quit_pane(Pane *p, int force);
void  editor_refresh_all(void);
void  cm_editor_render(void);
void  cm_editor_handle_event(InEvent *ev);
void  cm_editor_log(const char *fmt, ...);
void  cm_editor_init_hooks(void);
void  cm_editor_check_file_changes(void);
void  cm_editor_autosave_tick(void);

/* rendering */
void cm_render_pane(Pane *p, int active);
void cm_render_tabbar(void);
void cm_render_infobar(void);
void cm_render_keymenu(void);
int  cm_layout_top(void);
int  cm_layout_bottom_reserved(void);

/* prompt */
void cm_prompt_text(const char *msg, const char *initial, int hist, PromptDone done,
                 PromptChanged changed, void *data);
void cm_prompt_yn(const char *msg, YNDone done, void *data);
void cm_prompt_cancel(void);
int  cm_prompt_active(void);
int  cm_prompt_handle(InEvent *ev);
void cm_history_add(int kind, const char *s);
void cm_history_load(void);
void cm_history_save(void);

/* actions and bindings */
typedef int (*ActionFn)(Pane *p);

ActionFn cm_action_find(const char *name);
int      cm_action_is_multi(const char *name);
int      cm_run_action_string(Pane *p, const char *actions, InEvent *ev);
void     cm_bindings_init(void);
void     cm_bindings_load_user(void);
int      cm_bind_key(const char *keyname, const char *action, int save);
int      cm_unbind_key(const char *keyname, int save);
const char *cm_binding_for_event(InEvent *ev, int prompt);
const char *cm_binding_key_for_action(const char *action);
const char **cm_action_names(int *n);
int      cm_insert_rune(Pane *p, int r);
void     cm_insert_text(Pane *p, const char *s, int len, int paste);
void     cm_clipboard_set(const char *s, int len, int lines);
char    *cm_clipboard_get(int *len);
void     cm_macro_record(int kind, const char *s, int len, int rune);
int      cm_save_pane(Pane *p, int then_quit);
int      cm_autocomplete_suggestions(Pane *p, char ***out, int *idx);
int      cm_find_matching_brace(Buffer *b, Loc cur, int left, Loc *brace, Loc *match);

/* search */
int  cm_search_find(Pane *p, const char *pat, int regex, Loc from, int down, Loc *ms, Loc *me, char **err);
void cm_search_highlight_ranges(Pane *p, int y, unsigned char *mask, int len);

/* backups, savecursor, sudo save (backup.c) */
void cm_backup_tick(void);
void cm_backup_remove(Buffer *b);
int  cm_backup_check_on_open(Buffer *b);
void cm_cursor_save(Pane *p);
void cm_cursor_restore(Pane *p);
int  cm_buf_save_sudo(Buffer *b, const char *path, char **err);
int  cm_term_prompt(const char *msg, const char *const *choices, int nchoices);
extern int cm_term_ready;

/* commands */
void cm_command_run(Pane *p, const char *line);
void cm_run_shell_interactive(const char *cmd);
char **cm_file_complete(const char *arg, int *n);
char **cm_command_complete(const char *input, int cursor, int *start);
const char **cm_command_names(int *n);
void cm_help_open(Pane *p, const char *help_name);
void cm_apply_ftoptions(Buffer *b);
void cm_set_option_cmd(Pane *p, const char *name, const char *value, int local);

#endif
