/*
 * platform.h - the only non-ISO-C surface of the editor.
 *
 * ISO C has no notion of a terminal, raw keyboard input, window size,
 * directories or wall-clock milliseconds.  Everything that needs those
 * lives behind this interface, implemented for POSIX systems (Linux,
 * macOS, the BSDs, WSL, Cygwin) in platform.c.  Every other file uses
 * only the C89 standard library.  All strings crossing this interface
 * are UTF-8.
 */
#ifndef PLATFORM_H
#define PLATFORM_H

#include <stddef.h>
#include <stdio.h>

#include "port.h"

#if !P_HAS_64
#  error "cmicro needs a 64-bit integer type for timestamps"
#endif

/* terminal */
int  cm_plat_term_init(void);              /* enter raw mode; 0 on success */
void cm_plat_term_restore(void);           /* leave raw mode */
int  cm_plat_term_size(int *w, int *h);    /* 0 on success */
int  cm_plat_read(char *buf, int n, int timeout_ms); /* bytes read, 0 on timeout, -1 err */
void cm_plat_write(const char *buf, size_t n);
int  cm_plat_resized(void);                /* returns 1 once after a resize */
int  cm_plat_suspend(void);                /* Ctrl-Z style suspend (SIGTSTP) */
int  cm_plat_stdin_is_tty(void);
int  cm_plat_reopen_tty(void);             /* after reading piped stdin */
char *cm_plat_read_stdin(long *len);       /* read all of (piped) stdin */

p_s64 cm_plat_now_ms(void);                /* wall clock, milliseconds */

/* files and directories */
int   cm_plat_is_dir(const char *path);
int   cm_plat_exists(const char *path);
int   cm_plat_is_writable(const char *path);
p_s64 cm_plat_mtime(const char *path);     /* seconds; 0 if it doesn't exist */
int   cm_plat_mkdir_p(const char *path);
char *cm_plat_getcwd(void);                /* malloc'd */
int   cm_plat_chdir(const char *path);
char *cm_plat_realpath(const char *path);  /* malloc'd, NULL if it doesn't exist */

/* list directory entries (malloc'd array of malloc'd names, NULL terminated).
   Directories get a trailing '/'. */
char **cm_plat_list_dir(const char *path);

/* environment and processes */
char *cm_plat_getenv(const char *name);    /* malloc'd or NULL */
char *cm_plat_home_dir(void);              /* malloc'd or NULL */
char *cm_plat_temp_dir(void);              /* malloc'd */
/* sh command line running `cmd` with stdin from `in` (NULL: /dev/null)
   and stdout+stderr to `out`; malloc'd */
char *cm_plat_capture_cmd(const char *cmd, const char *in, const char *out);

#endif
