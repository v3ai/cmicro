/*
 * platform.h - the only non-ISO-C surface of the editor.
 *
 * ISO C has no notion of a terminal, raw keyboard input, window size,
 * directories or wall-clock milliseconds, and its file functions don't
 * define how file names are encoded.  Everything that needs those lives
 * behind this interface, implemented twice:
 *   platform.c        POSIX (Linux, macOS, BSDs, WSL, Cygwin)
 *   platform_win32.c  native Windows (console API, UTF-16 file APIs)
 * Each file compiles to (almost) nothing on the other platform, so
 * compiling every .c file in src/ with one cc command
 * works everywhere.  All strings crossing this interface are UTF-8.
 */
#ifndef PLATFORM_H
#define PLATFORM_H

#include <stddef.h>
#include <stdio.h>

/* terminal */
int  plat_term_init(void);              /* enter raw mode; 0 on success */
void plat_term_restore(void);           /* leave raw mode */
int  plat_term_size(int *w, int *h);    /* 0 on success */
int  plat_read(char *buf, int n, int timeout_ms); /* bytes read, 0 on timeout, -1 err */
void plat_write(const char *buf, size_t n);
int  plat_resized(void);                /* returns 1 once after a resize */
int  plat_can_suspend(void);
int  plat_suspend(void);                /* Ctrl-Z style suspend (SIGTSTP) */
int  plat_stdin_is_tty(void);
int  plat_reopen_tty(void);             /* after reading piped stdin */
char *plat_read_stdin(long *len);       /* read all of (piped) stdin */
void plat_args(int *argc, char ***argv); /* command line as UTF-8 */

long long plat_now_ms(void);

/* files and directories */
FILE *plat_fopen(const char *path, const char *mode);
int   plat_remove(const char *path);
int   plat_is_dir(const char *path);
int   plat_exists(const char *path);
int   plat_is_writable(const char *path);
long long plat_mtime(const char *path);
int   plat_mkdir_p(const char *path);
char *plat_getcwd(void);                /* malloc'd */
int   plat_chdir(const char *path);
char *plat_realpath(const char *path);  /* malloc'd, NULL if it doesn't exist */
int   plat_is_sep(int c);               /* path separator? */
int   plat_is_abs(const char *path);

/* list directory entries (malloc'd array of malloc'd names, NULL terminated).
   Directories get a trailing '/'. */
char **plat_list_dir(const char *path);

/* environment and processes */
char *plat_getenv(const char *name);    /* malloc'd or NULL */
char *plat_home_dir(void);              /* malloc'd or NULL */
char *plat_temp_dir(void);              /* malloc'd */
int   plat_system(const char *cmd);
const char *plat_null_device(void);
char *plat_shell_quote(const char *s);  /* malloc'd */
/* shell command line running `cmd` with stdin from `in` (NULL: null device)
   and stdout+stderr to `out`; malloc'd */
char *plat_capture_cmd(const char *cmd, const char *in, const char *out);
int   plat_has_sudo(void);
const char *plat_default_fileformat(void); /* "unix" or "dos" */

/* native clipboard: 0 on success, -1 if this platform has none built in */
int   plat_clipboard_set(const char *s, int len);
char *plat_clipboard_get(int *len);     /* malloc'd, NULL if unavailable */

/* ---- safe file writing (additions) ----
   plat_save_open() starts writing `path`.  When `path` is an existing
   regular file we own with a single link, the data goes to a new
   temporary file in the same directory, which atomically replaces the
   file on commit (with its mode and group); otherwise the file is
   rewritten in place, after making sure the disk space is there.
   Symbolic links are followed, so the file they point to is saved.
   `size` is the exact number of bytes that will be written (-1: unknown).
   With PLAT_SAVE_PRIVATE (backups, cursor positions) a new file gets mode
   0600 and a symlink at `path` is replaced, never followed.
   On failure it returns NULL and sets *denied when permission was the
   problem (the caller may then offer `sudo`).
   The temporary file is used only when the file is writable by its owner
   and has no extended attributes (ACLs, security labels); if the final
   rename fails (EBUSY on a bind-mounted file, EXDEV), the data is copied
   over the file in place instead. */
#define PLAT_SAVE_PRIVATE 1
typedef struct PlatSave PlatSave;
PlatSave   *plat_save_open(const char *path, long long size, int flags, int *denied);
int         plat_save_write(PlatSave *s, const void *data, size_t n); /* 0 or -1 */
int         plat_save_close(PlatSave *s, int commit); /* 0 on success; frees s */
const char *plat_save_error(void);    /* reason for the last failure */

/* new temporary file (mode 0600, created exclusively, unpredictable
   name) in the temp dir; *path gets its malloc'd name */
FILE *plat_temp_file(const char *tag, char **path);
/* run argv[0] (searched in PATH) without a shell, stdin from `in_path`
   (NULL: null device), stdout/stderr to the null device; waits for it.
   Returns its exit status, -1 if it could not be run. */
int   plat_run(const char *const *argv, const char *in_path);
int   plat_mkdir_private(const char *path); /* mkdir -p; new dirs get 0700 */
int   plat_owns_file(const char *path); /* a regular file (not a symlink) of ours */
/* a termination request (SIGTERM, SIGHUP, the terminal going away) since
   the last call: its signal number, else 0 */
int   plat_take_signal(void);

#endif
