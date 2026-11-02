/*
 * platform.c - POSIX glue (Linux, macOS, BSDs, WSL, Cygwin).  See
 * platform.h; the native Windows version is platform_win32.c.
 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "platform.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>
#include <poll.h>

static struct termios orig_termios;
static int raw_on = 0;
static volatile sig_atomic_t winch_flag = 0;

static void on_winch(int sig) { (void)sig; winch_flag = 1; }

int plat_term_init(void) {
    struct termios raw;
    struct sigaction sa;
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) return -1;
    raw = orig_termios;
    raw.c_iflag &= ~(unsigned)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(unsigned)(OPOST);
    raw.c_cflag |= CS8;
    raw.c_lflag &= ~(unsigned)(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) return -1;
    raw_on = 1;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_winch;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGWINCH, &sa, NULL);
    return 0;
}

void plat_term_restore(void) {
    if (raw_on) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        raw_on = 0;
    }
}

int plat_term_size(int *w, int *h) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        const char *c = getenv("COLUMNS"), *l = getenv("LINES");
        *w = c ? atoi(c) : 80;
        *h = l ? atoi(l) : 24;
        return -1;
    }
    *w = ws.ws_col;
    *h = ws.ws_row;
    return 0;
}

int plat_read(char *buf, int n, int timeout_ms) {
    struct pollfd p;
    int r;
    p.fd = STDIN_FILENO;
    p.events = POLLIN;
    p.revents = 0;
    r = poll(&p, 1, timeout_ms);
    if (r <= 0) return (r < 0 && errno != EINTR) ? -1 : 0;
    r = (int)read(STDIN_FILENO, buf, (size_t)n);
    if (r < 0) return (errno == EAGAIN || errno == EINTR) ? 0 : -1;
    return r;
}

void plat_write(const char *buf, size_t n) {
    while (n > 0) {
        ssize_t w = write(STDOUT_FILENO, buf, n);
        if (w < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return;
        }
        buf += w;
        n -= (size_t)w;
    }
}

int plat_resized(void) {
    if (winch_flag) { winch_flag = 0; return 1; }
    return 0;
}

int plat_can_suspend(void) { return 1; }

int plat_suspend(void) {
    plat_term_restore();
    raise(SIGTSTP);
    return plat_term_init();
}

int plat_stdin_is_tty(void) { return isatty(STDIN_FILENO); }

int plat_reopen_tty(void) {
    int fd = open("/dev/tty", O_RDWR);
    if (fd < 0) return -1;
    dup2(fd, STDIN_FILENO);
    close(fd);
    return 0;
}

long long plat_now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

int plat_is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int plat_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

int plat_is_writable(const char *path) { return access(path, W_OK) == 0; }

long long plat_mtime(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (long long)st.st_mtime;
}

int plat_mkdir_p(const char *path) {
    char tmp[PATH_MAX];
    size_t i, n = strlen(path);
    if (n == 0 || n >= sizeof tmp) return -1;
    memcpy(tmp, path, n + 1);
    for (i = 1; i < n; i++) {
        if (tmp[i] == '/') {
            tmp[i] = 0;
            mkdir(tmp, 0755);
            tmp[i] = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

char *plat_getcwd(void) {
    char buf[PATH_MAX];
    char *r;
    if (!getcwd(buf, sizeof buf)) return NULL;
    r = malloc(strlen(buf) + 1);
    if (r) strcpy(r, buf);
    return r;
}

int plat_chdir(const char *path) { return chdir(path); }

char *plat_realpath(const char *path) { return realpath(path, NULL); }

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

char **plat_list_dir(const char *path) {
    DIR *d = opendir(path && *path ? path : ".");
    struct dirent *e;
    char **out = NULL;
    size_t n = 0, cap = 0;
    if (!d) return NULL;
    while ((e = readdir(d)) != NULL) {
        char full[PATH_MAX];
        size_t len;
        char *s;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(full, sizeof full, "%s/%s", path && *path ? path : ".", e->d_name);
        len = strlen(e->d_name);
        s = malloc(len + 2);
        if (!s) break;
        memcpy(s, e->d_name, len + 1);
        if (plat_is_dir(full)) { s[len] = '/'; s[len + 1] = 0; }
        if (n + 2 > cap) {
            char **nn;
            cap = cap ? cap * 2 : 32;
            nn = realloc(out, cap * sizeof *out);
            if (!nn) { free(s); break; }
            out = nn;
        }
        out[n++] = s;
    }
    closedir(d);
    if (!out) { out = malloc(sizeof *out); if (!out) return NULL; }
    out[n] = NULL;
    qsort(out, n, sizeof *out, cmp_str);
    return out;
}

/* ---------------- portability helpers ---------------- */

FILE *plat_fopen(const char *path, const char *mode) { return fopen(path, mode); }
int plat_remove(const char *path) { return remove(path); }
int plat_is_sep(int c) { return c == '/'; }
int plat_is_abs(const char *path) { return path[0] == '/'; }
int plat_system(const char *cmd) { return system(cmd); }
const char *plat_null_device(void) { return "/dev/null"; }
int plat_has_sudo(void) { return 1; }
const char *plat_default_fileformat(void) { return "unix"; }
void plat_args(int *argc, char ***argv) { (void)argc; (void)argv; }
int plat_clipboard_set(const char *s, int len) { (void)s; (void)len; return -1; }
char *plat_clipboard_get(int *len) { (void)len; return NULL; }

static char *dupstr(const char *s) {
    char *r;
    if (!s) return NULL;
    r = malloc(strlen(s) + 1);
    if (r) strcpy(r, s);
    return r;
}

char *plat_getenv(const char *name) { return dupstr(getenv(name)); }
char *plat_home_dir(void) { return dupstr(getenv("HOME")); }

char *plat_temp_dir(void) {
    const char *d = getenv("TMPDIR");
    return dupstr(d && *d ? d : "/tmp");
}

/* single quotes for sh */
char *plat_shell_quote(const char *s) {
    size_t n = 3;
    const char *p;
    char *r, *o;
    for (p = s; *p; p++) n += *p == '\'' ? 4 : 1;
    r = o = malloc(n);
    if (!r) return NULL;
    *o++ = '\'';
    for (p = s; *p; p++) {
        if (*p == '\'') { memcpy(o, "'\\''", 4); o += 4; }
        else *o++ = *p;
    }
    *o++ = '\'';
    *o = 0;
    return r;
}

char *plat_capture_cmd(const char *cmd, const char *in, const char *out) {
    char *qi = in ? plat_shell_quote(in) : NULL, *qo = plat_shell_quote(out), *r;
    const char *fi = qi ? qi : "/dev/null";
    size_t n;
    if (!qo) { free(qi); return NULL; }
    n = strlen(cmd) + strlen(fi) + strlen(qo) + 32;
    r = malloc(n);
    if (r) sprintf(r, "( %s ) < %s > %s 2>&1", cmd, fi, qo);
    free(qi);
    free(qo);
    return r;
}

char *plat_read_stdin(long *len) {
    size_t cap = 65536, n = 0, got;
    char *buf = malloc(cap + 1), *nb;
    if (!buf) return NULL;
    while ((got = fread(buf + n, 1, cap - n, stdin)) > 0) {
        n += got;
        if (n == cap) {
            cap *= 2;
            nb = realloc(buf, cap + 1);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
    }
    buf[n] = 0;
    if (len) *len = (long)n;
    return buf;
}

#endif /* !_WIN32 */

/* ISO C forbids an empty translation unit */
typedef int platform_posix_unused;
