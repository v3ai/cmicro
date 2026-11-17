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

static const char backup_msg[] =
    "A backup was detected for:\n\n%s\n\n"
    "This likely means that micro crashed while editing this file,\n"
    "or another instance of micro is currently editing this file,\n"
    "or an error occurred while saving this file so it may be corrupted.\n\n"
    "The backup was created on %s and its path is:\n\n%s\n\n";

/* second half kept separate: C89 limits a string literal to 509 characters */
static const char backup_msg_options[] =
    "* 'recover' will apply the backup as unsaved changes to the current buffer.\n"
    "  When the buffer is closed, the backup will be removed.\n"
    "* 'ignore' will ignore the backup, discarding its changes. The backup file\n"
    "  will be removed.\n"
    "* 'abort' will abort the open operation, and instead open an empty buffer.\n\n"
    "Options: [r]ecover, [i]gnore, [a]bort: ";

/* Go's url.QueryEscape, as used by micro for backup file names */
static char *escape_path(const char *p) {
    Str o;
    cm_str_init(&o);
    cm_str_reserve(&o, 0);
    for (; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
            c == '_' || c == '.' || c == '~')
            cm_str_putc(&o, (char)c);
        else if (c == ' ')
            cm_str_putc(&o, '+');
        else
            cm_str_printf(&o, "%%%02X", c);
    }
    return cm_str_detach(&o);
}

static char *backup_dir(Buffer *b) {
    const char *bd = BOPTS(b, O_BACKUPDIR);
    if (bd && *bd) return cm_expand_home(bd);
    return cm_config_dir ? cm_xsprintf("%s/backups", cm_config_dir) : NULL;
}

static char *backup_file(Buffer *b) {
    char *dir, *esc, *r;
    if (!b->abspath || !*b->path || b->type != BT_DEFAULT) return NULL;
    dir = backup_dir(b);
    if (!dir) return NULL;
    esc = escape_path(b->abspath);
    r = cm_xsprintf("%s/%s", dir, esc);
    free(esc);
    free(dir);
    return r;
}

static void write_backup(Buffer *b) {
    char *path = backup_file(b), *dir, *text;
    FILE *f;
    int len;
    if (!path) return;
    dir = backup_dir(b);
    cm_plat_mkdir_p(dir);
    free(dir);
    f = fopen(path, "wb");
    if (f) {
        text = cm_buf_full_text(b, &len);
        fwrite(text, 1, (size_t)len, f);
        fclose(f);
        free(text);
    }
    free(path);
}

void cm_backup_remove(Buffer *b) {
    char *path;
    if (!BOPT(b, O_BACKUP) || BOPT(b, O_PERMBACKUP)) return;
    path = backup_file(b);
    if (path) remove(path);
    free(path);
}

static void backup_fn(Pane *p, void *data) {
    Buffer *b = p->buf;
    p_s64 now = *(p_s64 *)data;
    if (!BOPT(b, O_BACKUP) || b->type != BT_DEFAULT || !*b->path) return;
    if (!cm_buf_modified(b)) return;
    if (b->autosave_at != 0 && now - b->autosave_at < BACKUP_SECONDS * 1000) return;
    b->autosave_at = now;
    write_backup(b);
}

void cm_backup_tick(void) {
    p_s64 now = cm_plat_now_ms();
    cm_editor_all_panes(backup_fn, &now);
}

/* ask a question on the normal terminal (micro's TermPrompt) */
int cm_term_prompt(const char *msg, const char *const *choices, int nchoices);

/* returns 1 if recovered, 0 if no backup / ignored, -1 if aborted */
int cm_backup_check_on_open(Buffer *b) {
    char *path, *data, *msg;
    long len;
    static const char *const choices[] = {"r", "i", "a", "recover", "ignore", "abort"};
    int ch;
    if (!BOPT(b, O_BACKUP) || BOPT(b, O_PERMBACKUP) || !*b->path || b->type != BT_DEFAULT) return 0;
    path = backup_file(b);
    if (!path || !cm_plat_exists(path)) { free(path); return 0; }
    {
        time_t t = (time_t)cm_plat_mtime(path);
        char when[64];
        Str m;
        strftime(when, sizeof when, "%a %b %d at %H:%M, %Y", localtime(&t));
        cm_str_init(&m);
        cm_str_printf(&m, backup_msg, b->path, when, path);
        cm_str_puts(&m, backup_msg_options);
        msg = cm_str_detach(&m);
    }
    ch = cm_term_prompt(msg, choices, 6);
    free(msg);
    if (ch % 3 == 0) {
        data = cm_read_file(path, &len);
        if (data) {
            cm_buf_set_text(b, data, (int)len);
            free(data);
            b->saved_id = -1; /* unsaved changes */
            cm_buf_invalidate_hl(b, 0);
        }
        free(path);
        return 1;
    }
    if (ch % 3 == 1) {
        remove(path);
        free(path);
        return 0;
    }
    free(path);
    return -1;
}

/* ---------------- savecursor ---------------- */

static char *cursor_file(Buffer *b) {
    char *esc, *r;
    if (!cm_config_dir || !b->abspath || !*b->path) return NULL;
    esc = escape_path(b->abspath);
    r = cm_xsprintf("%s/buffers/%s.cursor", cm_config_dir, esc);
    free(esc);
    return r;
}

void cm_cursor_save(Pane *p) {
    char *path, *dir;
    FILE *f;
    if (!BOPT(p->buf, O_SAVECURSOR) || p->buf->type != BT_DEFAULT) return;
    path = cursor_file(p->buf);
    if (!path) return;
    dir = cm_xsprintf("%s/buffers", cm_config_dir);
    cm_plat_mkdir_p(dir);
    free(dir);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "%d %d\n", ACUR(p)->loc.y, ACUR(p)->loc.x);
        fclose(f);
    }
    free(path);
}

void cm_cursor_restore(Pane *p) {
    char *path, *data;
    int y, x;
    if (!BOPT(p->buf, O_SAVECURSOR)) return;
    path = cursor_file(p->buf);
    if (!path) return;
    data = cm_read_file(path, NULL);
    free(path);
    if (!data) return;
    if (sscanf(data, "%d %d", &y, &x) == 2) {
        Cursor *c = ACUR(p);
        c->loc = cm_buf_clamp(p->buf, cm_loc_make(x, y));
        c->sel[0] = c->sel[1] = c->orig[0] = c->orig[1] = c->loc;
    }
    free(data);
}

/* ---------------- sudo save ---------------- */

extern int cm_term_ready;

int cm_buf_save_sudo(Buffer *b, const char *path, char **err) {
    char *text, *tmp, *cmd, *qpath, *qtmp;
    char *dir = cm_plat_temp_dir();
    int len, rc;
    FILE *f;
    char *expanded = cm_expand_home(path);
    if (err) *err = NULL;
    text = cm_buf_full_text(b, &len);
    if (!strcmp(BOPTS(b, O_FILEFORMAT), "dos")) {
        Str s;
        int i;
        cm_str_init(&s);
        cm_str_reserve(&s, 0);
        for (i = 0; i < len; i++) {
            if (text[i] == '\n') cm_str_putc(&s, '\r');
            cm_str_putc(&s, text[i]);
        }
        free(text);
        len = s.len;
        text = cm_str_detach(&s);
    }
    tmp = cm_xsprintf("%s/micro-sudo-%ld-%d", dir ? dir : ".", (long)time(NULL), rand());
    free(dir);
    f = fopen(tmp, "wb");
    if (!f) {
        if (err) *err = cm_xstrdup("Could not create a temporary file");
        free(text); free(tmp); free(expanded);
        return -1;
    }
    fwrite(text, 1, (size_t)len, f);
    fclose(f);
    free(text);
    qpath = cm_xsprintf("'%s'", expanded);
    qtmp = cm_xsprintf("'%s'", tmp);
    cmd = cm_xsprintf("%s dd bs=4k of=%s < %s 2>/dev/null", GOPTS(O_SUCMD), qpath, qtmp);
    if (cm_term_ready) {
        cm_scr_fini();
        cm_plat_term_restore();
    }
    rc = system(cmd);
    if (cm_term_ready) {
        cm_plat_term_init();
        cm_scr_init();
        cm_scr_set_mouse(GOPT(O_MOUSE));
        cm_scr_invalidate();
    }
    remove(tmp);
    free(cmd);
    free(qpath);
    free(qtmp);
    free(tmp);
    if (rc != 0) {
        if (err) *err = cm_xsprintf("Error saving %s with %s", expanded, GOPTS(O_SUCMD));
        free(expanded);
        return -1;
    }
    free(expanded);
    if (strcmp(b->path, path) != 0) cm_buf_set_name(b, path);
    b->is_new = 0;
    b->mtime = cm_plat_mtime(b->path);
    cm_buf_mark_saved(b);
    cm_backup_remove(b);
    return 0;
}
