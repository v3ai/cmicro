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

int loc_cmp(Loc a, Loc b);
Loc loc_make(int x, int y);

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

extern const OptDef opt_defs[O_COUNT];
extern OptVal gopts[O_COUNT];

#define GOPT(id) (gopts[id].i)
#define GOPTS(id) (gopts[id].s)

int   opt_find(const char *name);
int   opt_parse(int id, const char *value, OptVal *out, char **err);
char *opt_to_string(int id, const OptVal *v);
void  opt_copy(int id, OptVal *dst, const OptVal *src);
void  options_init_defaults(void);
void  settings_load(void);      /* settings.json */
int   settings_save(void);      /* write non-default globals to settings.json */
void  settings_note_set(const char *name, const char *value); /* remember for saving */
void  settings_note_reset(const char *name);
void  settings_apply_parsed(struct Buffer *b, const char *path); /* ft: and glob: sections */

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

enum { EV_INSERT, EV_REMOVE };

typedef struct TextEvent {
    int type;
    Loc start, end;
    char *text;
    int len;
    long long time;
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
    long long mtime;
    int refs;
    int is_new;
    int bom;
    long long autosave_at;
} Buffer;

#define BOPT(b, id) ((b)->opts[id].i)
#define BOPTS(b, id) ((b)->opts[id].s)

Buffer *buf_new_empty(void);
Buffer *buf_from_string(const char *name, const char *text, int type);
Buffer *buf_open(const char *path, char **err);
void    buf_free(Buffer *b);
int     buf_save(Buffer *b, const char *path, char **err);
int     buf_reload(Buffer *b, char **err);
int     buf_modified(const Buffer *b);
void    buf_set_name(Buffer *b, const char *path);
void    buf_set_text(Buffer *b, const char *text, int len);

Loc   buf_insert(Buffer *b, Loc at, const char *text, int len);
void  buf_remove(Buffer *b, Loc a, Loc z);
char *buf_get_text(const Buffer *b, Loc a, Loc z, int *len);
Loc   buf_replace(Buffer *b, Loc a, Loc z, const char *text, int len);
Loc   buf_end(const Buffer *b);
Loc   buf_clamp(const Buffer *b, Loc l);
Loc   buf_move_chars(const Buffer *b, Loc l, int n); /* +/- characters across lines */
int   buf_char_at(const Buffer *b, Loc l);          /* code point or -1 at EOL */
char *buf_full_text(const Buffer *b, int *len);
void  buf_update_rules(Buffer *b);                  /* filetype detection */
void  buf_invalidate_hl(Buffer *b, int from);
void  buf_ensure_hl(Buffer *b, int line);           /* highlight line (and states before) */
int   buf_undo(Buffer *b);
int   buf_redo(Buffer *b);
void  buf_mark_saved(Buffer *b);
void  buf_set_option(Buffer *b, int id, const OptVal *v, int local);
void  buf_option_changed(Buffer *b, int id);
const char *buf_leading_ws(const Buffer *b, int y, int *len);
char *buf_indent_string(const Buffer *b);

extern void (*buf_edit_hook)(Buffer *b, int type, Loc start, Loc end);
extern Loc (*buf_get_cursor_hook)(Buffer *b, int *num); /* editing cursor for undo records */
extern void (*buf_set_cursor_hook)(Buffer *b, Loc l, int num);

/* ================================================================== */
/* Cursors and panes                                                   */

typedef struct {
    Loc loc;
    int lastvx;     /* remembered visual column for up/down */
    Loc sel[2];     /* current selection (sel[0] <= sel[1]) */
    Loc orig[2];    /* anchor; orig[0]..orig[1] for word/line selection */
} Cursor;

int  cur_has_sel(const Cursor *c);
void cur_deselect(Cursor *c, int start);
void cur_set_sel_to(Cursor *c, Loc l);

enum { DRAG_CHAR, DRAG_WORD, DRAG_LINE };

typedef struct Pane {
    Buffer *buf;
    Cursor *cur;
    int ncur, ccap, active;
    int x, y, w, h;    /* full rect, statusline included */
    int top, toprow;   /* first visible line and softwrap row within it */
    int left;          /* horizontal scroll in columns */
    int overwrite;
    long long last_click;
    int clicks;
    Loc click_loc;
    int dragging, drag_mode;
    int multi_word;    /* SpawnMultiCursor searching whole words */
    Loc search_origin; /* for incremental search */
    int search_origin_top;
    struct Tab *tab;
} Pane;

#define ACUR(p) (&(p)->cur[(p)->active])

Pane *pane_new(Buffer *b);
void  pane_free(Pane *p);
void  pane_set_buffer(Pane *p, Buffer *b);
int   pane_add_cursor(Pane *p, Loc l);
void  pane_remove_cursor(Pane *p, int i);
void  pane_merge_cursors(Pane *p);
void  pane_reset_cursors(Pane *p);

/* geometry */
int  pane_gutter_width(const Pane *p);
int  pane_text_width(const Pane *p);
int  pane_text_height(const Pane *p);
int  pane_visual_x(const Pane *p, int y, int x);      /* byte offset -> column */
int  pane_x_from_visual(const Pane *p, int y, int vx); /* column -> byte offset */
int  pane_line_rows(const Pane *p, int y);            /* softwrap rows */
void pane_loc_to_row(const Pane *p, Loc l, int *row, int *rvx);
Loc  pane_row_to_loc(const Pane *p, int y, int row, int rvx);
void pane_relocate(Pane *p);
void pane_center(Pane *p);
void pane_scroll(Pane *p, int n);
int  pane_screen_to_loc(Pane *p, int sx, int sy, Loc *out);
void cur_store_vx(Pane *p, Cursor *c);
void cur_up(Pane *p, Cursor *c, int n);
void cur_down(Pane *p, Cursor *c, int n);
void cur_left(Pane *p, Cursor *c);
void cur_right(Pane *p, Cursor *c);

/* ================================================================== */
/* Tabs and splits                                                     */

enum { NODE_LEAF, NODE_VSPLIT, NODE_HSPLIT };

typedef struct Node {
    int kind;
    Pane *pane;
    struct Node *a, *b, *parent;
    double ratio;
    int x, y, w, h;
} Node;

typedef struct Tab {
    Node *root;
    Pane *active;
} Tab;

Tab  *tab_new(Pane *p);
void  tab_free(Tab *t);
void  tab_layout(Tab *t, int x, int y, int w, int h);
Pane *tab_split(Tab *t, Pane *p, Buffer *b, int vertical);
void  tab_close_pane(Tab *t, Pane *p); /* frees pane */
int   tab_npanes(Tab *t);
Pane **tab_panes(Tab *t, int *n);     /* malloc'd array in order */
Node *tab_find_node(Node *n, Pane *p);
Pane *tab_pane_at(Tab *t, int x, int y);

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

int   input_poll(InEvent *ev, int timeout_ms);
void  input_free(InEvent *ev);
int   key_parse(const char *name, int *key, int *mods);
char *key_name(int key, int mods);  /* malloc'd canonical name */

/* ================================================================== */
/* Screen                                                              */

void scr_init(void);
void scr_fini(void);
void scr_resize(void);
int  scr_w(void);
int  scr_h(void);
void scr_clear(void);
void scr_set(int x, int y, int cp, Style st);
int  scr_puts(int x, int y, const char *s, Style st, int maxw);
void scr_fill(int x, int y, int w, int h, int cp, Style st);
Style scr_get_style(int x, int y);
void scr_set_style(int x, int y, Style st);
void scr_show_cursor(int x, int y);
void scr_hide_cursor(void);
void scr_flush(void);
void scr_invalidate(void);
void scr_set_mouse(int on);
void scr_osc52(const char *text, int len);
extern int scr_truecolor;
int  scr_detect_truecolor(const char *opt); /* "auto", "on" or "off" */

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
    long long last_autosave;
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

extern Editor E;

Tab  *cur_tab(void);
Pane *cur_pane(void);
void  editor_add_tab(Pane *p);
void  editor_close_tab(int i);
void  editor_layout(void);
void  editor_set_message(const char *fmt, ...);
void  editor_set_error(const char *fmt, ...);
void  editor_clear_message(void);
Pane *editor_open_file_pane(const char *path, char **err);
Buffer *editor_find_buffer(const char *path);
void  editor_all_panes(void (*fn)(Pane *, void *), void *data);
void  editor_quit_pane(Pane *p, int force);
void  editor_refresh_all(void);
void  editor_render(void);
void  editor_handle_event(InEvent *ev);
void  editor_log(const char *fmt, ...);

/* rendering */
void render_pane(Pane *p, int active);
void render_tabbar(void);
void render_infobar(void);
void render_keymenu(void);
int  layout_top(void);
int  layout_bottom_reserved(void);

/* prompt */
void prompt_text(const char *msg, const char *initial, int hist, PromptDone done,
                 PromptChanged changed, void *data);
void prompt_yn(const char *msg, YNDone done, void *data);
void prompt_cancel(void);
int  prompt_active(void);
int  prompt_handle(InEvent *ev);
void history_add(int kind, const char *s);
void history_load(void);
void history_save(void);

/* actions and bindings */
typedef int (*ActionFn)(Pane *p);

ActionFn action_find(const char *name);
int      action_is_multi(const char *name);
int      run_action_string(Pane *p, const char *actions, InEvent *ev);
void     bindings_init(void);
void     bindings_load_user(void);
int      bind_key(const char *keyname, const char *action, int save);
int      unbind_key(const char *keyname, int save);
const char *binding_for_event(InEvent *ev, int prompt);
const char *binding_key_for_action(const char *action);
const char **action_names(int *n);
int      insert_rune(Pane *p, int r);
void     insert_text(Pane *p, const char *s, int len, int paste);
void     clipboard_set(const char *s, int len, int lines);
char    *clipboard_get(int *len);
void     macro_record(int kind, const char *s, int len, int rune);

/* search */
int  search_find(Pane *p, const char *pat, int regex, Loc from, int down, Loc *ms, Loc *me, char **err);
void search_highlight_ranges(Pane *p, int y, unsigned char *mask, int len);

/* backups, savecursor, sudo save (backup.c) */
void backup_tick(void);
void backup_remove(Buffer *b);
int  backup_check_on_open(Buffer *b);
void cursor_save(Pane *p);
void cursor_restore(Pane *p);
int  buf_save_sudo(Buffer *b, const char *path, char **err);
int  term_prompt(const char *msg, const char *const *choices, int nchoices);
extern int term_ready;

/* commands */
void command_run(Pane *p, const char *line);
char **command_complete(const char *input, int cursor, int *start);
const char **command_names(int *n);
void help_open(Pane *p, const char *topic);
void apply_ftoptions(Buffer *b);
void set_option_cmd(Pane *p, const char *name, const char *value, int local);

#endif
