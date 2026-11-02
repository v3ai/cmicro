/*
 * backup.c - micro's crash-recovery backups, `savecursor`, and saving
 * through `sudo` when permission is denied.
 */
#include "micro.h"
#include "platform.h"
#include "runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BACKUP_SECONDS 8

/* longest escaped file name used as is (NAME_MAX is 255 almost everywhere) */
#define MAX_NAME 200

/* buffer.c */
int buf_load_file(Buffer *b, const char *path);
int buf_serialize(const Buffer *b, int (*put)(void *ctx, const char *p, size_t n), void *ctx);
int buf_write_file(const Buffer *b, const char *path, int flags, char **err);

static const char backup_msg[] =
    "A backup was detected for:\n\n%s\n\n"
    "This likely means that micro crashed while editing this file,\n"
    "or another instance of micro is currently editing this file,\n"
    "or an error occurred while saving this file so it may be corrupted.\n\n"
    "The backup was created on %s and its path is:\n\n%s\n\n"
    "* 'recover' will apply the backup as unsaved changes to the current buffer.\n"
    "  When the buffer is closed, the backup will be removed.\n"
    "* 'ignore' will ignore the backup, discarding its changes. The backup file\n"
    "  will be removed.\n"
    "* 'abort' will abort the open operation, and instead open an empty buffer.\n\n"
    "Options: [r]ecover, [i]gnore, [a]bort: ";

/* Go's url.QueryEscape, as used by micro for backup file names.  The
   result is a single path component: '/' and '\\' are escaped, and so is
   a leading '.' (no "." / ".." / hidden names).  Names too long for the
   file system keep their start and get a hash of the whole path. */
static char *escape_path(const char *p) {
    Str o;
    const char *s;
    unsigned long long h = 14695981039346656037ULL; /* FNV-1a */
    str_init(&o);
    str_reserve(&o, 0);
    for (s = p; *s; s++) {
        unsigned char c = (unsigned char)*s;
        h = (h ^ c) * 1099511628211ULL;
        if (((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
             c == '_' || c == '.' || c == '~') && !(c == '.' && s == p))
            str_putc(&o, (char)c);
        else if (c == ' ')
            str_putc(&o, '+');
        else
            str_printf(&o, "%%%02X", c);
    }
    if (o.len > MAX_NAME) {
        int cut = MAX_NAME - 17;
        /* don't split a %XX escape */
        if (o.s[cut - 1] == '%') cut -= 1;
        else if (o.s[cut - 2] == '%') cut -= 2;
        o.len = cut;
        o.s[cut] = 0;
        str_printf(&o, "-%016llx", h);
    }
    return str_detach(&o);
}

static char *backup_dir(Buffer *b) {
    const char *bd = BOPTS(b, O_BACKUPDIR);
    if (bd && *bd) return expand_home(bd);
    return config_dir ? xsprintf("%s/backups", config_dir) : NULL;
}

static char *backup_file(Buffer *b) {
    char *dir, *esc, *r;
    if (!b->abspath || !*b->path || b->type != BT_DEFAULT) return NULL;
    dir = backup_dir(b);
    if (!dir) return NULL;
    esc = escape_path(b->abspath);
    r = xsprintf("%s/%s", dir, esc);
    free(esc);
    free(dir);
    return r;
}

/* Backups hold file contents: they are written privately (0600, in a
   0700 directory if we create it, replacing rather than following any
   symlink), atomically, and with the file's own line endings so that
   recovering one gives back the same file format. */
static void write_backup(Buffer *b) {
    char *path = backup_file(b), *dir, *err = NULL;
    if (!path) return;
    dir = backup_dir(b);
    plat_mkdir_private(dir);
    free(dir);
    if (buf_write_file(b, path, PLAT_SAVE_PRIVATE, &err) != 0) {
        editor_log("could not write backup: %s", err ? err : path);
        free(err);
    }
    free(path);
}

void backup_remove(Buffer *b) {
    char *path;
    if (!BOPT(b, O_BACKUP) || BOPT(b, O_PERMBACKUP)) return;
    path = backup_file(b);
    if (path) plat_remove(path);
    free(path);
}

static void backup_fn(Pane *p, void *data) {
    Buffer *b = p->buf;
    long long now = *(long long *)data;
    if (!BOPT(b, O_BACKUP) || b->type != BT_DEFAULT || !*b->path) return;
    if (!buf_modified(b)) return;
    /* now == 0: write regardless of the interval (exiting) */
    if (now && b->autosave_at != 0 && now - b->autosave_at < BACKUP_SECONDS * 1000) return;
    b->autosave_at = now ? now : plat_now_ms();
    write_backup(b);
}

void backup_tick(void) {
    long long now = plat_now_ms();
    if (now == 0) now = 1;
    editor_all_panes(backup_fn, &now);
}

/* write backups of every modified buffer now (before an unclean exit) */
void backup_flush_all(void) {
    long long now = 0;
    editor_all_panes(backup_fn, &now);
}

/* ask a question on the normal terminal (micro's TermPrompt) */
int term_prompt(const char *msg, const char *const *choices, int nchoices);

/* returns 1 if recovered, 0 if no backup / ignored, -1 if aborted */
int backup_check_on_open(Buffer *b) {
    char *path, *msg;
    static const char *const choices[] = {"r", "i", "a", "recover", "ignore", "abort"};
    int ch;
    if (!BOPT(b, O_BACKUP) || BOPT(b, O_PERMBACKUP) || !*b->path || b->type != BT_DEFAULT) return 0;
    path = backup_file(b);
    /* in a shared backupdir, someone else's file (or a symlink) is no
       backup of ours: never offer to load it into the buffer */
    if (!path || !plat_owns_file(path)) { free(path); return 0; }
    {
        time_t t = (time_t)plat_mtime(path);
        struct tm *tm = localtime(&t);
        char when[64];
        if (!tm || !strftime(when, sizeof when, "%a %b %d at %H:%M, %Y", tm)) strcpy(when, "an unknown date");
        msg = xsprintf(backup_msg, b->path, when, path);
    }
    ch = term_prompt(msg, choices, 6);
    free(msg);
    if (ch % 3 == 0) {
        /* recover into a scratch buffer first: a failed read must not
           leave the real one half replaced */
        Buffer *tmp = buf_new_empty();
        if (buf_load_file(tmp, path) == 0) {
            char *text;
            int len, bom = tmp->bom;
            text = buf_full_text(tmp, &len);
            buf_set_text(b, text, len);
            if (bom) b->bom = 1;
            if (tmp->local[O_FILEFORMAT]) {
                free(b->opts[O_FILEFORMAT].s);
                b->opts[O_FILEFORMAT].s = xstrdup(BOPTS(tmp, O_FILEFORMAT));
                b->local[O_FILEFORMAT] = 1;
            }
            free(text);
            b->saved_id = -1; /* unsaved changes */
            buf_invalidate_hl(b, 0);
        }
        buf_free(tmp);
        free(path);
        return 1;
    }
    if (ch % 3 == 1) {
        plat_remove(path);
        free(path);
        return 0;
    }
    free(path);
    return -1;
}

/* ---------------- savecursor ---------------- */

static char *cursor_file(Buffer *b) {
    char *esc, *r;
    if (!config_dir || !b->abspath || !*b->path) return NULL;
    esc = escape_path(b->abspath);
    r = xsprintf("%s/buffers/%s.cursor", config_dir, esc);
    free(esc);
    return r;
}

void cursor_save(Pane *p) {
    char *path, *dir, *line;
    PlatSave *s;
    int denied;
    if (!BOPT(p->buf, O_SAVECURSOR) || p->buf->type != BT_DEFAULT) return;
    path = cursor_file(p->buf);
    if (!path) return;
    dir = xsprintf("%s/buffers", config_dir);
    plat_mkdir_private(dir);
    free(dir);
    line = xsprintf("%d %d\n", ACUR(p)->loc.y, ACUR(p)->loc.x);
    s = plat_save_open(path, (long long)strlen(line), PLAT_SAVE_PRIVATE, &denied);
    if (s) {
        int ok = plat_save_write(s, line, strlen(line)) == 0;
        plat_save_close(s, ok);
    }
    free(line);
    free(path);
}

void cursor_restore(Pane *p) {
    char *path, *data;
    int y, x;
    if (!BOPT(p->buf, O_SAVECURSOR)) return;
    path = cursor_file(p->buf);
    if (!path) return;
    data = plat_owns_file(path) ? read_file(path, NULL) : NULL;
    free(path);
    if (!data) return;
    if (sscanf(data, "%9d %9d", &y, &x) == 2) {
        Cursor *c = ACUR(p);
        c->loc = buf_clamp(p->buf, loc_make(x, y));
        c->sel[0] = c->sel[1] = c->orig[0] = c->orig[1] = c->loc;
    }
    free(data);
}

/* ---------------- sudo save ---------------- */

extern int term_ready;

static int put_file(void *ctx, const char *p, size_t n) {
    return fwrite(p, 1, n, (FILE *)ctx) == n ? 0 : -1;
}

/* Runs `sucmd dd bs=4k of=FILE` (like micro) with the text on stdin.  No
   shell is involved, so nothing in the file name or in `sucmd` (split
   into words like a command line) is ever interpreted. */
int buf_save_sudo(Buffer *b, const char *path, char **err) {
    char *tmp = NULL, *of, **words;
    const char **argv;
    int nwords, rc, i, ok;
    FILE *f;
    char *exp = expand_home(path);
    if (err) *err = NULL;
    words = shell_split(GOPTS(O_SUCMD), &nwords);
    if (nwords == 0) {
        if (err) *err = xstrdup("The sucmd option is empty");
        free_strv(words);
        free(exp);
        return -1;
    }
    f = plat_temp_file("sudo", &tmp);
    if (!f) {
        if (err) *err = xstrdup("Could not create a temporary file");
        free_strv(words);
        free(exp);
        return -1;
    }
    ok = buf_serialize(b, put_file, f) == 0;
    ok = fflush(f) == 0 && ok;
    ok = !ferror(f) && ok;
    if (fclose(f) != 0) ok = 0;
    if (!ok) {
        if (err) *err = xstrdup("Could not write a temporary file");
        plat_remove(tmp);
        free(tmp);
        free_strv(words);
        free(exp);
        return -1;
    }
    of = xsprintf("of=%s", exp);
    argv = xmalloc(sizeof *argv * (size_t)(nwords + 4));
    for (i = 0; i < nwords; i++) argv[i] = words[i];
    argv[i++] = "dd";
    argv[i++] = "bs=4k";
    argv[i++] = of;
    argv[i] = NULL;
    if (term_ready) {
        scr_fini();
        plat_term_restore();
    }
    rc = plat_run(argv, tmp);
    if (term_ready) {
        plat_term_init();
        scr_init();
        scr_set_mouse(GOPT(O_MOUSE));
        scr_invalidate();
    }
    plat_remove(tmp);
    free(tmp);
    free(argv);
    free(of);
    free_strv(words);
    if (rc != 0) {
        if (err) *err = xsprintf("Error saving %s with %s", exp, GOPTS(O_SUCMD));
        free(exp);
        return -1;
    }
    free(exp);
    if (strcmp(b->path, path) != 0) buf_set_name(b, path);
    b->is_new = 0;
    b->mtime = plat_mtime(b->path);
    buf_mark_saved(b);
    backup_remove(b);
    return 0;
}
