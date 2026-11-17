/*
 * main.c - command line handling, startup and the main loop.
 */
#include "micro.h"
#include "platform.h"
#include "runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>


int cm_term_ready;

int cm_term_prompt(const char *msg, const char *const *choices, int nchoices) {
    char line[256];
    int was = cm_term_ready, i = nchoices - 1;
    if (was) {
        cm_scr_fini();
        cm_plat_term_restore();
    }
    for (;;) {
        size_t n;
        int k;
        fputs(msg, stdout);
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) break;
        n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        for (k = 0; k < nchoices; k++)
            if (!strcmp(line, choices[k])) { i = k; goto done; }
        fputs("Invalid choice.\n", stdout);
    }
done:
    if (was) {
        cm_plat_term_init();
        cm_scr_init();
        cm_scr_set_mouse(GOPT(O_MOUSE));
        cm_scr_invalidate();
    }
    return i;
}

static void save_cursor_fn(Pane *p, void *data) {
    (void)data;
    cm_cursor_save(p);
    cm_backup_remove(p->buf); /* clean exit: unsaved changes were discarded on purpose */
}

static void cleanup(void) {
    if (cm_term_ready) {
        cm_scr_fini();
        cm_plat_term_restore();
        cm_term_ready = 0;
    }
}

/* one entry per line: C89 limits a string literal to 509 characters */
static const char *const usage_text[] = {
    "Usage: micro [OPTIONS] [FILE]...\n",
    "-clean\n",
    "    \tClean the configuration directory (not supported in this build)\n",
    "-config-dir dir\n",
    "    \tSpecify a custom location for the configuration directory\n",
    "[FILE]:LINE:COL (if the `parsecursor` option is enabled)\n",
    "+LINE:COL\n",
    "    \tSpecify a line and column to start the cursor at when opening a buffer\n",
    "-options\n",
    "    \tShow all option help\n",
    "-debug\n",
    "    \tEnable debug mode (accepted, no effect)\n",
    "-version\n",
    "    \tShow the version number and information\n",
    "\n",
    "Micro's plugins can be managed at the command line but this C build has no Lua plugins.\n",
    "\n",
    "Micro's options can also be set via command line arguments for quick\n",
    "adjustments. For real configuration, please use the settings.json\n",
    "file (see 'help options').\n",
    "\n",
    "-option value\n",
    "    \tSet `option` to `value` for this session\n",
    "    \tFor example: `micro -syntax off file.c`\n",
    "\n",
    "Use `micro -options` to see the full list of configuration options\n",
    NULL
};

static void usage(void) {
    int i;
    for (i = 0; usage_text[i]; i++) fputs(usage_text[i], stdout);
}

static void print_options(void) {
    int i;
    puts("-option value");
    for (i = 0; i < O_COUNT; i++) {
        printf("-%s value\n    \tDefault value: '%s'\n", cm_opt_defs[i].name, cm_opt_defs[i].def);
    }
}

/* like micro: $MICRO_CONFIG_HOME, $XDG_CONFIG_HOME/micro, ~/.config/micro */
static void init_config_dir(const char *override) {
    char *mch = cm_plat_getenv("MICRO_CONFIG_HOME");
    char *xdg = cm_plat_getenv("XDG_CONFIG_HOME");
    char *home = cm_plat_home_dir();
    if (override) cm_config_dir = cm_expand_home(override);
    else if (mch && *mch) cm_config_dir = cm_expand_home(mch);
    else if (xdg && *xdg) cm_config_dir = cm_xsprintf("%s/micro", xdg);
    else if (home) cm_config_dir = cm_xsprintf("%s/.config/micro", home);
    else cm_config_dir = NULL;
    free(mch);
    free(xdg);
    free(home);
    if (cm_config_dir && !cm_plat_is_dir(cm_config_dir)) {
        if (cm_plat_mkdir_p(cm_config_dir) != 0) {
            /* not fatal: run without a config dir */
        }
    }
}

typedef struct {
    char *path;
    int line, col;
} FileArg;

int main(int argc, char **argv) {
    FileArg *files = cm_xcalloc((size_t)argc + 1, sizeof *files);
    int nfiles = 0, i, pend_line = -1, pend_col = -1;
    const char *cfgdir = NULL;
    char **optnames = cm_xcalloc((size_t)argc + 1, sizeof(char *));
    char **optvals = cm_xcalloc((size_t)argc + 1, sizeof(char *));
    int nopts = 0;
    char *stdin_text = NULL;
    long stdin_len = 0;
    char *err = NULL;

    srand((unsigned)time(NULL));

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        int done = -1; /* exit status for options that exit immediately */
        if (!strcmp(a, "-version") || !strcmp(a, "--version") || !strcmp(a, "-v")) {
            printf("Version: %s\nCommit hash: n/a (C rewrite)\nCompiled on " __DATE__ "\n", MICRO_VERSION);
            done = 0;
        } else if (!strcmp(a, "-help") || !strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage();
            done = 0;
        } else if (!strcmp(a, "-options")) {
            print_options();
            done = 0;
        } else if (!strcmp(a, "-config-dir")) {
            if (i + 1 < argc) cfgdir = argv[++i];
        } else if (!strcmp(a, "-debug") || !strcmp(a, "-profile")) {
            /* accepted for compatibility */
        } else if (!strcmp(a, "-clean") || !strcmp(a, "-plugin")) {
            fprintf(stderr, "micro: %s is not supported in this build\n", a);
            done = 1;
        } else if (a[0] == '+' && a[1] >= '0' && a[1] <= '9') {
            const char *c = strchr(a, ':');
            pend_line = cm_clamped_atoi(a + 1);
            pend_col = c ? cm_clamped_atoi(c + 1) : -1;
        } else if (a[0] == '-' && a[1] && strcmp(a, "--") != 0 && cm_opt_find(a + 1) >= 0 && i + 1 < argc) {
            optnames[nopts] = (char *)(a + 1);
            optvals[nopts] = argv[++i];
            nopts++;
        } else if (!strcmp(a, "--")) {
            for (i++; i < argc; i++) {
                files[nfiles].path = argv[i];
                files[nfiles].line = pend_line;
                files[nfiles].col = pend_col;
                pend_line = pend_col = -1;
                nfiles++;
            }
        } else {
            files[nfiles].path = (char *)a;
            files[nfiles].line = pend_line;
            files[nfiles].col = pend_col;
            pend_line = pend_col = -1;
            nfiles++;
        }
        if (done >= 0) {
            free(files);
            free(optnames);
            free(optvals);
            return done;
        }
    }

    init_config_dir(cfgdir);
    cm_options_init_defaults();
    cm_settings_load();
    for (i = 0; i < nopts; i++) {
        int id = cm_opt_find(optnames[i]);
        OptVal v;
        if (cm_opt_parse(id, optvals[i], &v, &err) == 0) {
            if (cm_opt_defs[id].type == OT_STR) free(cm_gopts[id].s);
            cm_gopts[id] = v;
        } else {
            fprintf(stderr, "micro: -%s: %s\n", optnames[i], err);
            free(err);
            err = NULL;
        }
    }
    if (cm_colorscheme_load(GOPTS(O_COLORSCHEME), &err) != 0) {
        free(err);
        err = NULL;
        cm_colorscheme_load("default", NULL);
        cm_editor_set_error("%s is not a valid colorscheme", GOPTS(O_COLORSCHEME));
    }
    cm_scr_truecolor = cm_scr_detect_truecolor(GOPTS(O_TRUECOLOR));
    cm_syntax_init();
    cm_group_styles_refresh();
    cm_bindings_init();
    cm_bindings_load_user();
    cm_history_load();
    cm_editor_init_hooks();

    if (!cm_plat_stdin_is_tty()) {
        stdin_text = cm_plat_read_stdin(&stdin_len);
        if (cm_plat_reopen_tty() != 0) {
            fputs("micro: could not open the terminal\n", stderr);
            return 1;
        }
    }

    /* open buffers */
    for (i = 0; i < nfiles; i++) {
        Pane *p = cm_editor_open_file_pane(files[i].path, &err);
        if (!p) {
            fprintf(stderr, "micro: %s\n", err ? err : "error opening file");
            free(err);
            err = NULL;
            continue;
        }
        if (files[i].line > 0) {
            Cursor *c = ACUR(p);
            Buffer *b = p->buf;
            c->loc.y = cm_clampi(files[i].line - 1, 0, b->nlines - 1);
            c->loc.x = files[i].col > 0 ? cm_utf8_offset(b->lines[c->loc.y].s, b->lines[c->loc.y].len, files[i].col - 1) : 0;
            c->sel[0] = c->sel[1] = c->orig[0] = c->orig[1] = c->loc;
        }
        if (cm_E.ntabs == 0 || !strcmp(GOPTS(O_MULTIOPEN), "tab")) {
            cm_editor_add_tab(p);
        } else {
            /* hsplit / vsplit into the first tab */
            Tab *t = cm_E.tabs[0];
            Pane *np = cm_tab_split(t, t->active, p->buf, !strcmp(GOPTS(O_MULTIOPEN), "vsplit"));
            np->cur[0] = p->cur[0];
            cm_pane_free(p);
        }
    }
    if (stdin_text) {
        Buffer *b = cm_buf_from_string("", stdin_text, BT_DEFAULT);
        free(b->path);
        b->path = cm_xstrdup("");
        free(b->name);
        b->name = cm_xstrdup("");
        /* piped text counts as unsaved */
        b->saved_id = -1;
        cm_editor_add_tab(cm_pane_new(b));
        free(stdin_text);
    }
    if (cm_E.ntabs == 0) cm_editor_add_tab(cm_pane_new(cm_buf_new_empty()));
    cm_E.curtab = 0;
    free(files);
    free(optnames);
    free(optvals);

    if (cm_plat_term_init() != 0) {
        fputs("micro: could not set up the terminal\n", stderr);
        return 1;
    }
    cm_term_ready = 1;
    atexit(cleanup);
    cm_scr_init();
    cm_scr_set_mouse(GOPT(O_MOUSE));
    cm_editor_layout();
    {
        int k;
        for (k = 0; k < cm_E.ntabs; k++) {
            int n, j;
            Pane **ps = cm_tab_panes(cm_E.tabs[k], &n);
            for (j = 0; j < n; j++) {
                cm_cur_store_vx(ps[j], ACUR(ps[j]));
                cm_pane_relocate(ps[j]);
            }
            free(ps);
        }
    }

    {
        p_s64 last_check = 0;
        while (!cm_E.quit && cm_E.ntabs > 0) {
            InEvent ev;
            int handled = 0;
            cm_editor_layout();
            cm_editor_render();
            if (cm_input_poll(&ev, 500) > 0) {
                cm_editor_handle_event(&ev);
                cm_input_free(&ev);
                handled = 1;
                /* drain queued input before redrawing (fast typing / key repeat) */
                while (!cm_E.quit && cm_E.ntabs > 0 && handled < 64 && cm_input_poll(&ev, 0) > 0) {
                    cm_editor_handle_event(&ev);
                    cm_input_free(&ev);
                    handled++;
                }
            }
            cm_editor_autosave_tick();
            if (cm_plat_now_ms() - last_check > 1000) {
                last_check = cm_plat_now_ms();
                cm_editor_check_file_changes();
                cm_backup_tick();
            }
        }
    }
    cm_history_save();
    cm_editor_all_panes(save_cursor_fn, NULL);
    cleanup();
    return 0;
}
