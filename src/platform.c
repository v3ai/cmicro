/*
 * platform.c - POSIX glue (Linux, macOS, the BSDs, WSL, Cygwin).  See
 * platform.h.  This is the only file that includes system headers beyond
 * the C89 standard library (Universal C rules, section 11); the build
 * system selects it, nothing in the rest of the editor tests the platform.
 *
 * _POSIX_C_SOURCE asks the system headers for POSIX.1-2008.  _DEFAULT_SOURCE
 * (glibc) and _DARWIN_C_SOURCE (macOS) additionally expose SIGWINCH and
 * TIOCGWINSZ, which POSIX does not cover but every supported system has.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
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

int cm_plat_term_init(void) {
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

void cm_plat_term_restore(void) {
    if (raw_on) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        raw_on = 0;
    }
}

/* like atoi, but defined for any input: clamped to 0..10000 */
static int env_dim(const char *s, int def) {
    long v;
    if (!s) return def;
    v = strtol(s, NULL, 10);
    return v < 0 ? 0 : v > 10000 ? 10000 : (int)v;
}

int cm_plat_term_size(int *w, int *h) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        const char *c = getenv("COLUMNS"), *l = getenv("LINES");
        *w = env_dim(c, 80);
        *h = env_dim(l, 24);
        return -1;
    }
    *w = ws.ws_col;
    *h = ws.ws_row;
    return 0;
}

int cm_plat_read(char *buf, int n, int timeout_ms) {
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

void cm_plat_write(const char *buf, size_t n) {
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

int cm_plat_resized(void) {
    if (winch_flag) { winch_flag = 0; return 1; }
    return 0;
}

int cm_plat_suspend(void) {
    cm_plat_term_restore();
    raise(SIGTSTP);
    return cm_plat_term_init();
}

int cm_plat_stdin_is_tty(void) { return isatty(STDIN_FILENO); }

int cm_plat_reopen_tty(void) {
    int fd = open("/dev/tty", O_RDWR);
    if (fd < 0) return -1;
    dup2(fd, STDIN_FILENO);
    close(fd);
    return 0;
}

p_s64 cm_plat_now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (p_s64)tv.tv_sec * 1000 + (p_s64)(tv.tv_usec / 1000);
}

int cm_plat_is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int cm_plat_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

int cm_plat_is_writable(const char *path) { return access(path, W_OK) == 0; }

p_s64 cm_plat_mtime(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (p_s64)st.st_mtime;
}

int cm_plat_mkdir_p(const char *path) {
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

char *cm_plat_getcwd(void) {
    char buf[PATH_MAX];
    char *r;
    if (!getcwd(buf, sizeof buf)) return NULL;
    r = malloc(strlen(buf) + 1);
    if (r) strcpy(r, buf);
    return r;
}

int cm_plat_chdir(const char *path) { return chdir(path); }

char *cm_plat_realpath(const char *path) { return realpath(path, NULL); }

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

char **cm_plat_list_dir(const char *path) {
    const char *dir = path && *path ? path : ".";
    size_t dirlen = strlen(dir);
    DIR *d = opendir(dir);
    struct dirent *e;
    char **out = NULL;
    size_t n = 0, cap = 0;
    if (!d) return NULL;
    while ((e = readdir(d)) != NULL) {
        char *full;
        size_t len;
        char *s;
        int dir_flag;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        len = strlen(e->d_name);
        full = malloc(dirlen + len + 2);
        if (!full) break;
        memcpy(full, dir, dirlen);
        full[dirlen] = '/';
        memcpy(full + dirlen + 1, e->d_name, len + 1);
        dir_flag = cm_plat_is_dir(full);
        free(full);
        s = malloc(len + 2);
        if (!s) break;
        memcpy(s, e->d_name, len + 1);
        if (dir_flag) { s[len] = '/'; s[len + 1] = 0; }
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

/* ---------------- environment ---------------- */

static char *dupstr(const char *s) {
    char *r;
    if (!s) return NULL;
    r = malloc(strlen(s) + 1);
    if (r) strcpy(r, s);
    return r;
}

char *cm_plat_getenv(const char *name) { return dupstr(getenv(name)); }
char *cm_plat_home_dir(void) { return dupstr(getenv("HOME")); }

char *cm_plat_temp_dir(void) {
    const char *d = getenv("TMPDIR");
    return dupstr(d && *d ? d : "/tmp");
}

/* single quotes for sh */
static char *shell_quote(const char *s) {
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

char *cm_plat_capture_cmd(const char *cmd, const char *in, const char *out) {
    char *qi = in ? shell_quote(in) : NULL, *qo = shell_quote(out), *r;
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

char *cm_plat_read_stdin(long *len) {
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
