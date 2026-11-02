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

void editor_init_hooks(void);
void editor_check_external_changes(void);
void editor_autosave_tick(void);

int term_ready;

int term_prompt(const char *msg, const char *const *choices, int nchoices) {
    char line[256];
    int was = term_ready, i = nchoices - 1;
    if (was) {
        scr_fini();
        plat_term_restore();
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
        plat_term_init();
        scr_init();
        scr_set_mouse(GOPT(O_MOUSE));
        scr_invalidate();
    }
    return i;
}

static void save_cursor_fn(Pane *p, void *data) {
    (void)data;
    cursor_save(p);
    backup_remove(p->buf); /* clean exit: unsaved changes were discarded on purpose */
}

static void cleanup(void) {
    if (term_ready) {
        scr_fini();
        plat_term_restore();
        term_ready = 0;
    }
}

static void usage(void) {
    puts("Usage: micro [OPTIONS] [FILE]...\n"
         "-clean\n"
         "    \tClean the configuration directory (not supported in this build)\n"
         "-config-dir dir\n"
         "    \tSpecify a custom location for the configuration directory\n"
         "[FILE]:LINE:COL (if the `parsecursor` option is enabled)\n"
         "+LINE:COL\n"
         "    \tSpecify a line and column to start the cursor at when opening a buffer\n"
         "-options\n"
         "    \tShow all option help\n"
         "-debug\n"
         "    \tEnable debug mode (accepted, no effect)\n"
         "-version\n"
         "    \tShow the version number and information\n"
         "\n"
         "Micro's plugins can be managed at the command line but this C build has no Lua plugins.\n"
         "\n"
         "Micro's options can also be set via command line arguments for quick\n"
         "adjustments. For real configuration, please use the settings.json\n"
         "file (see 'help options').\n"
         "\n"
         "-option value\n"
         "    \tSet `option` to `value` for this session\n"
         "    \tFor example: `micro -syntax off file.c`\n"
         "\n"
         "Use `micro -options` to see the full list of configuration options");
}

static void print_options(void) {
    int i;
    puts("-option value");
    for (i = 0; i < O_COUNT; i++) {
        printf("-%s value\n    \tDefault value: '%s'\n", opt_defs[i].name, opt_defs[i].def);
    }
}

/* like micro: $MICRO_CONFIG_HOME, $XDG_CONFIG_HOME/micro, ~/.config/micro
   (on Windows ~ is %USERPROFILE%) */
static void init_config_dir(const char *override) {
    char *mch = plat_getenv("MICRO_CONFIG_HOME");
    char *xdg = plat_getenv("XDG_CONFIG_HOME");
    char *home = plat_home_dir();
    if (override) config_dir = expand_home(override);
    else if (mch && *mch) config_dir = expand_home(mch);
    else if (xdg && *xdg) config_dir = xsprintf("%s/micro", xdg);
    else if (home) config_dir = xsprintf("%s/.config/micro", home);
    else config_dir = NULL;
    free(mch);
    free(xdg);
    free(home);
    if (config_dir && !plat_is_dir(config_dir)) {
        if (plat_mkdir_p(config_dir) != 0) {
            /* not fatal: run without a config dir */
        }
    }
}

typedef struct {
    char *path;
    int line, col;
} FileArg;

int main(int argc, char **argv) {
    FileArg *files = xcalloc((size_t)argc + 1, sizeof *files);
    int nfiles = 0, i, pend_line = -1, pend_col = -1;
    const char *cfgdir = NULL;
    char **optnames = xcalloc((size_t)argc + 1, sizeof(char *));
    char **optvals = xcalloc((size_t)argc + 1, sizeof(char *));
    int nopts = 0;
    char *stdin_text = NULL;
    long stdin_len = 0;
    char *err = NULL;

    srand((unsigned)time(NULL));
    plat_args(&argc, &argv); /* UTF-8 arguments on Windows */

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-version") || !strcmp(a, "--version") || !strcmp(a, "-v")) {
            printf("Version: %s\nCommit hash: n/a (C rewrite)\nCompiled on " __DATE__ "\n", MICRO_VERSION);
            return 0;
        } else if (!strcmp(a, "-help") || !strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage();
            return 0;
        } else if (!strcmp(a, "-options")) {
            print_options();
            return 0;
        } else if (!strcmp(a, "-config-dir")) {
            if (i + 1 < argc) cfgdir = argv[++i];
        } else if (!strcmp(a, "-debug") || !strcmp(a, "-profile")) {
            /* accepted for compatibility */
        } else if (!strcmp(a, "-clean") || !strcmp(a, "-plugin")) {
            fprintf(stderr, "micro: %s is not supported in this build\n", a);
            return 1;
        } else if (a[0] == '+' && a[1] >= '0' && a[1] <= '9') {
            const char *c = strchr(a, ':');
            pend_line = atoi(a + 1);
            pend_col = c ? atoi(c + 1) : -1;
        } else if (a[0] == '-' && a[1] && strcmp(a, "--") != 0 && opt_find(a + 1) >= 0 && i + 1 < argc) {
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
    }

    init_config_dir(cfgdir);
    options_init_defaults();
    settings_load();
    for (i = 0; i < nopts; i++) {
        int id = opt_find(optnames[i]);
        OptVal v;
        if (opt_parse(id, optvals[i], &v, &err) == 0) {
            if (opt_defs[id].type == OT_STR) free(gopts[id].s);
            gopts[id] = v;
        } else {
            fprintf(stderr, "micro: -%s: %s\n", optnames[i], err);
            free(err);
            err = NULL;
        }
    }
    if (colorscheme_load(GOPTS(O_COLORSCHEME), &err) != 0) {
        free(err);
        err = NULL;
        colorscheme_load("default", NULL);
        editor_set_error("%s is not a valid colorscheme", GOPTS(O_COLORSCHEME));
    }
    scr_truecolor = scr_detect_truecolor(GOPTS(O_TRUECOLOR));
    syntax_init();
    group_styles_refresh();
    bindings_init();
    bindings_load_user();
    history_load();
    editor_init_hooks();

    if (!plat_stdin_is_tty()) {
        stdin_text = plat_read_stdin(&stdin_len);
        if (plat_reopen_tty() != 0) {
            fputs("micro: could not open the terminal\n", stderr);
            return 1;
        }
    }

    /* open buffers */
    for (i = 0; i < nfiles; i++) {
        Pane *p = editor_open_file_pane(files[i].path, &err);
        if (!p) {
            fprintf(stderr, "micro: %s\n", err ? err : "error opening file");
            free(err);
            err = NULL;
            continue;
        }
        if (files[i].line > 0) {
            Cursor *c = ACUR(p);
            Buffer *b = p->buf;
            c->loc.y = clampi(files[i].line - 1, 0, b->nlines - 1);
            c->loc.x = files[i].col > 0 ? utf8_offset(b->lines[c->loc.y].s, b->lines[c->loc.y].len, files[i].col - 1) : 0;
            c->sel[0] = c->sel[1] = c->orig[0] = c->orig[1] = c->loc;
        }
        if (E.ntabs == 0 || !strcmp(GOPTS(O_MULTIOPEN), "tab")) {
            editor_add_tab(p);
        } else {
            /* hsplit / vsplit into the first tab */
            Tab *t = E.tabs[0];
            Pane *np = tab_split(t, t->active, p->buf, !strcmp(GOPTS(O_MULTIOPEN), "vsplit"));
            np->cur[0] = p->cur[0];
            pane_free(p);
        }
    }
    if (stdin_text) {
        Buffer *b = buf_from_string("", stdin_text, BT_DEFAULT);
        free(b->path);
        b->path = xstrdup("");
        free(b->name);
        b->name = xstrdup("");
        /* piped text counts as unsaved */
        b->saved_id = -1;
        editor_add_tab(pane_new(b));
        free(stdin_text);
    }
    if (E.ntabs == 0) editor_add_tab(pane_new(buf_new_empty()));
    E.curtab = 0;
    free(files);
    free(optnames);
    free(optvals);

    if (plat_term_init() != 0) {
        fputs("micro: could not set up the terminal (on Windows, this needs Windows 10 1809 or later)\n", stderr);
        return 1;
    }
    term_ready = 1;
    atexit(cleanup);
    scr_init();
    scr_set_mouse(GOPT(O_MOUSE));
    editor_layout();
    {
        int k;
        for (k = 0; k < E.ntabs; k++) {
            int n, j;
            Pane **ps = tab_panes(E.tabs[k], &n);
            for (j = 0; j < n; j++) {
                cur_store_vx(ps[j], ACUR(ps[j]));
                pane_relocate(ps[j]);
            }
            free(ps);
        }
    }

    {
        long long last_check = 0;
        while (!E.quit && E.ntabs > 0) {
            InEvent ev;
            int handled = 0;
            editor_layout();
            editor_render();
            if (input_poll(&ev, 500) > 0) {
                editor_handle_event(&ev);
                input_free(&ev);
                handled = 1;
                /* drain queued input before redrawing (fast typing / key repeat) */
                while (!E.quit && E.ntabs > 0 && handled < 64 && input_poll(&ev, 0) > 0) {
                    editor_handle_event(&ev);
                    input_free(&ev);
                    handled++;
                }
            }
            editor_autosave_tick();
            if (plat_now_ms() - last_check > 1000) {
                last_check = plat_now_ms();
                editor_check_external_changes();
                backup_tick();
            }
        }
    }
    history_save();
    editor_all_panes(save_cursor_fn, NULL);
    cleanup();
    return 0;
}
