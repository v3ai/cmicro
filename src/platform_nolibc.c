/*
 * platform_nolibc.c - the platform layer for the zero-dependency Linux
 * x86-64 build: every operation is a raw system call (see
 * nolibc/include/linux_sys.h); no C library is involved.
 */
#ifdef MICRO_NOLIBC
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "linux_sys.h"

#define PATH_MAX_NOLIBC 4096

static struct k_termios orig_termios;
static int raw_on;
static volatile int winch_flag;

static void on_winch(int sig) { (void)sig; winch_flag = 1; }

static int install_handler(int sig, void (*h)(int)) {
    struct k_sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.handler = h;
    sa.flags = K_SA_RESTORER; /* no SA_RESTART: let poll() wake up */
    sa.restorer = __nolibc_restorer;
    return (int)sys4(SYS_rt_sigaction, sig, (long)&sa, 0, 8);
}

int plat_term_init(void) {
    struct k_termios raw;
    if (sys3(SYS_ioctl, 0, K_TCGETS, (long)&orig_termios) < 0) return -1;
    raw = orig_termios;
    raw.c_iflag &= ~(unsigned)(K_BRKINT | K_ICRNL | K_INPCK | K_ISTRIP | K_IXON);
    raw.c_oflag &= ~(unsigned)K_OPOST;
    raw.c_cflag |= K_CS8;
    raw.c_lflag &= ~(unsigned)(K_ECHO | K_ICANON | K_IEXTEN | K_ISIG);
    raw.c_cc[K_VMIN] = 0;
    raw.c_cc[K_VTIME] = 0;
    if (sys3(SYS_ioctl, 0, K_TCSETSF, (long)&raw) < 0) return -1;
    raw_on = 1;
    install_handler(K_SIGWINCH, on_winch);
    return 0;
}

void plat_term_restore(void) {
    if (raw_on) {
        sys3(SYS_ioctl, 0, K_TCSETSF, (long)&orig_termios);
        raw_on = 0;
    }
}

int plat_term_size(int *w, int *h) {
    struct k_winsize ws;
    if (sys3(SYS_ioctl, 1, K_TIOCGWINSZ, (long)&ws) < 0 || ws.ws_col == 0) {
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
    struct k_pollfd p;
    long r;
    p.fd = 0;
    p.events = K_POLLIN;
    p.revents = 0;
    r = sys3(SYS_poll, (long)&p, 1, timeout_ms);
    if (r <= 0) return (r < 0 && r != -K_EINTR) ? -1 : 0;
    r = sys3(SYS_read, 0, (long)buf, n);
    if (r < 0) return (r == -K_EAGAIN || r == -K_EINTR) ? 0 : -1;
    return (int)r;
}

void plat_write(const char *buf, size_t n) {
    while (n > 0) {
        long w = sys3(SYS_write, 1, (long)buf, (long)n);
        if (w == -K_EINTR || w == -K_EAGAIN) continue;
        if (w <= 0) return;
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
    sys2(SYS_kill, sys0(SYS_getpid), K_SIGTSTP);
    return plat_term_init();
}

int plat_stdin_is_tty(void) {
    struct k_termios t;
    return sys3(SYS_ioctl, 0, K_TCGETS, (long)&t) == 0;
}

int plat_reopen_tty(void) {
    long fd = sys3(SYS_open, (long)"/dev/tty", K_O_RDWR, 0);
    if (fd < 0) return -1;
    sys2(SYS_dup2, fd, 0);
    sys1(SYS_close, fd);
    return 0;
}

char *plat_read_stdin(long *len) {
    size_t cap = 65536, n = 0;
    char *buf = malloc(cap + 1), *nb;
    if (!buf) return NULL;
    for (;;) {
        long r = sys3(SYS_read, 0, (long)(buf + n), (long)(cap - n));
        if (r == -K_EINTR) continue;
        if (r <= 0) break;
        n += (size_t)r;
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

void plat_args(int *argc, char ***argv) { (void)argc; (void)argv; }

long long plat_now_ms(void) {
    struct k_timespec ts;
    sys2(SYS_clock_gettime, K_CLOCK_REALTIME, (long)&ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---------------- files ---------------- */

FILE *plat_fopen(const char *path, const char *mode) { return fopen(path, mode); }
int plat_remove(const char *path) { return remove(path); }

static int do_stat(const char *path, struct k_stat *st) {
    return sys4(SYS_newfstatat, K_AT_FDCWD, (long)path, (long)st, 0) == 0;
}

int plat_is_dir(const char *path) {
    struct k_stat st;
    return do_stat(path, &st) && (st.st_mode & K_S_IFMT) == K_S_IFDIR;
}

int plat_exists(const char *path) {
    struct k_stat st;
    return do_stat(path, &st);
}

int plat_is_writable(const char *path) {
    return sys4(SYS_faccessat, K_AT_FDCWD, (long)path, K_W_OK, 0) == 0;
}

long long plat_mtime(const char *path) {
    struct k_stat st;
    return do_stat(path, &st) ? (long long)st.st_mtime_ : 0;
}

int plat_mkdir_p(const char *path) {
    char tmp[PATH_MAX_NOLIBC];
    size_t i, n = strlen(path);
    long r;
    if (n == 0 || n >= sizeof tmp) return -1;
    memcpy(tmp, path, n + 1);
    for (i = 1; i < n; i++) {
        if (tmp[i] == '/') {
            tmp[i] = 0;
            sys2(SYS_mkdir, (long)tmp, 0755);
            tmp[i] = '/';
        }
    }
    r = sys2(SYS_mkdir, (long)tmp, 0755);
    return (r == 0 || r == -K_EEXIST) ? 0 : -1;
}

static char *dupstr(const char *s) {
    char *r;
    if (!s) return NULL;
    r = malloc(strlen(s) + 1);
    if (r) strcpy(r, s);
    return r;
}

char *plat_getcwd(void) {
    char buf[PATH_MAX_NOLIBC];
    if (sys2(SYS_getcwd, (long)buf, sizeof buf) < 0) return NULL;
    return dupstr(buf);
}

int plat_chdir(const char *path) { return sys1(SYS_chdir, (long)path) < 0 ? -1 : 0; }

/* lexically normalize an absolute path ("." and ".." components) */
static void normalize(char *p) {
    char *out = p, *s = p;
    while (*s) {
        while (*s == '/') s++;
        if (!*s) break;
        if (s[0] == '.' && (s[1] == '/' || !s[1])) { s++; continue; }
        if (s[0] == '.' && s[1] == '.' && (s[2] == '/' || !s[2])) {
            while (out > p && *--out != '/')
                ;
            s += 2;
            continue;
        }
        *out++ = '/';
        while (*s && *s != '/') *out++ = *s++;
    }
    if (out == p) *out++ = '/';
    *out = 0;
}

/* The kernel resolves the path when we open it; /proc/self/fd/N then
   reads back the canonical name.  Without /proc, normalize lexically. */
char *plat_realpath(const char *path) {
    char link[64], buf[PATH_MAX_NOLIBC];
    long fd = sys3(SYS_open, (long)path, K_O_PATH | K_O_CLOEXEC, 0), n;
    if (fd < 0) return NULL;
    snprintf(link, sizeof link, "/proc/self/fd/%ld", fd);
    n = sys3(SYS_readlink, (long)link, (long)buf, sizeof buf - 1);
    sys1(SYS_close, fd);
    if (n > 0 && buf[0] == '/') {
        buf[n] = 0;
        return dupstr(buf);
    }
    if (path[0] == '/') {
        if (strlen(path) >= sizeof buf) return NULL;
        strcpy(buf, path);
    } else {
        char *cwd = plat_getcwd();
        if (!cwd || strlen(cwd) + strlen(path) + 2 > sizeof buf) { free(cwd); return NULL; }
        snprintf(buf, sizeof buf, "%s/%s", cwd, path);
        free(cwd);
    }
    normalize(buf);
    return dupstr(buf);
}

int plat_is_sep(int c) { return c == '/'; }
int plat_is_abs(const char *path) { return path[0] == '/'; }

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

char **plat_list_dir(const char *path) {
    const char *dir = path && *path ? path : ".";
    long fd = sys3(SYS_open, (long)dir, K_O_RDONLY | K_O_DIRECTORY | K_O_CLOEXEC, 0);
    static char dbuf[16384];
    char **out = NULL;
    size_t n = 0, cap = 0;
    if (fd < 0) return NULL;
    for (;;) {
        long got = sys3(SYS_getdents64, fd, (long)dbuf, sizeof dbuf), off = 0;
        if (got <= 0) break;
        while (off < got) {
            struct k_dirent64 *d = (struct k_dirent64 *)(void *)(dbuf + off);
            const char *name = d->d_name;
            int is_dir = d->d_type == K_DT_DIR;
            size_t len;
            char *s;
            off += d->d_reclen;
            if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
            if (d->d_type == K_DT_UNKNOWN || d->d_type == K_DT_LNK) {
                char full[PATH_MAX_NOLIBC];
                snprintf(full, sizeof full, "%s/%s", dir, name);
                is_dir = plat_is_dir(full);
            }
            len = strlen(name);
            s = malloc(len + 2);
            if (!s) break;
            memcpy(s, name, len + 1);
            if (is_dir) { s[len] = '/'; s[len + 1] = 0; }
            if (n + 2 > cap) {
                char **nn;
                cap = cap ? cap * 2 : 32;
                nn = realloc(out, cap * sizeof *out);
                if (!nn) { free(s); break; }
                out = nn;
            }
            out[n++] = s;
        }
    }
    sys1(SYS_close, fd);
    if (!out) { out = malloc(sizeof *out); if (!out) return NULL; }
    out[n] = NULL;
    qsort(out, n, sizeof *out, cmp_str);
    return out;
}

/* ---------------- environment and processes ---------------- */

char *plat_getenv(const char *name) { return dupstr(getenv(name)); }
char *plat_home_dir(void) { return dupstr(getenv("HOME")); }

char *plat_temp_dir(void) {
    const char *d = getenv("TMPDIR");
    return dupstr(d && *d ? d : "/tmp");
}

int plat_system(const char *cmd) { return system(cmd); }
const char *plat_null_device(void) { return "/dev/null"; }
int plat_has_sudo(void) { return 1; }
const char *plat_default_fileformat(void) { return "unix"; }

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
    if (r) snprintf(r, n, "( %s ) < %s > %s 2>&1", cmd, fi, qo);
    free(qi);
    free(qo);
    return r;
}

int plat_clipboard_set(const char *s, int len) { (void)s; (void)len; return -1; }
char *plat_clipboard_get(int *len) { (void)len; return NULL; }

#endif /* MICRO_NOLIBC */

/* ISO C forbids an empty translation unit */
typedef int platform_nolibc_unused;
