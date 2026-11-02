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
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <poll.h>
#if defined(__linux__) || defined(__APPLE__)
#include <sys/xattr.h>
#endif

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

static struct termios orig_termios;
static int raw_on = 0;
static volatile sig_atomic_t winch_flag = 0;
static volatile sig_atomic_t term_signal = 0;
static int handlers_installed = 0;
static int zero_reads = 0;

static void on_winch(int sig) { (void)sig; winch_flag = 1; }

/* SIGTERM / SIGHUP: let the main loop save backups and exit cleanly */
static void on_term(int sig) { term_signal = sig; }

/* A fatal signal (crash): put the terminal back the way we found it, then
   die of the same signal.  Only async-signal-safe calls in here. */
static void on_fatal(int sig) {
    /* what scr_fini sends, including autowrap back on (scr_init turns it
       off) and no scroll region; keep the two in step */
    static const char reset[] = "\x1b[0m\x1b[r\x1b[?7h\x1b[?1006l\x1b[?1002l\x1b[?1000l\x1b[?2004l\x1b[?25h\x1b[?1049l";
    if (raw_on) {
        ssize_t w = write(STDOUT_FILENO, reset, sizeof reset - 1);
        (void)w;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        raw_on = 0;
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

static void install_handlers(void) {
    static const int fatal[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGQUIT};
    struct sigaction sa;
    size_t i;
    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = on_winch;
    sigaction(SIGWINCH, &sa, NULL);
    sa.sa_handler = on_term;
    for (i = 0; i < 2; i++) {
        int sig = i ? SIGHUP : SIGTERM;
        struct sigaction old;
        /* an inherited "ignore" (nohup) stays */
        if (sigaction(sig, NULL, &old) == 0 && old.sa_handler == SIG_IGN) continue;
        sigaction(sig, &sa, NULL);
    }
    sa.sa_handler = on_fatal;
    sa.sa_flags = SA_RESETHAND;
    for (i = 0; i < sizeof fatal / sizeof fatal[0]; i++) {
        struct sigaction old;
        /* leave alone handlers someone else (a sanitizer) installed */
        if (sigaction(fatal[i], NULL, &old) == 0 && old.sa_handler != SIG_DFL) continue;
        sigaction(fatal[i], &sa, NULL);
    }
    handlers_installed = 1;
}

int plat_term_init(void) {
    struct termios raw;
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
    if (!handlers_installed) install_handlers();
    return 0;
}

void plat_term_restore(void) {
    if (raw_on) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        raw_on = 0;
    }
}

static int env_dim(const char *name, int def) {
    const char *v = getenv(name);
    long n;
    char *end;
    if (!v || !*v) return def;
    n = strtol(v, &end, 10);
    return (*end || n <= 0 || n > 4096) ? def : (int)n;
}

int plat_term_size(int *w, int *h) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0 || ws.ws_row == 0) {
        *w = env_dim("COLUMNS", 80);
        *h = env_dim("LINES", 24);
        return -1;
    }
    *w = ws.ws_col;
    *h = ws.ws_row;
    return 0;
}

int plat_read(char *buf, int n, int timeout_ms) {
    struct pollfd p;
    int r;
    if (n <= 0) return 0;
    p.fd = STDIN_FILENO;
    p.events = POLLIN;
    p.revents = 0;
    r = poll(&p, 1, timeout_ms);
    if (r <= 0) return (r < 0 && errno != EINTR) ? -1 : 0;
    if (p.revents & POLLNVAL) { term_signal = SIGHUP; return -1; }
    r = (int)read(STDIN_FILENO, buf, (size_t)n);
    if (r > 0) { zero_reads = 0; return r; }
    if (r < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
    /* In raw mode (VMIN 0) a read of 0 bytes can just mean "nothing
       yet", but a hangup, an I/O error, or poll reporting input that never
       comes means the terminal is gone: poll would return at once forever
       and the editor would spin.  Report it as SIGHUP. */
    if (r == 0 && !(p.revents & (POLLHUP | POLLERR)) && ++zero_reads < 100) return 0;
    term_signal = SIGHUP;
    return -1;
}

void plat_write(const char *buf, size_t n) {
    while (n > 0) {
        ssize_t w = write(STDOUT_FILENO, buf, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) {
                struct pollfd p;
                p.fd = STDOUT_FILENO;
                p.events = POLLOUT;
                poll(&p, 1, 100);
                continue;
            }
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

int plat_take_signal(void) {
    int s = term_signal;
    term_signal = 0;
    return s;
}

int plat_can_suspend(void) { return 1; }

int plat_suspend(void) {
    plat_term_restore();
    raise(SIGTSTP);
    return plat_term_init();
}

int plat_stdin_is_tty(void) { return isatty(STDIN_FILENO); }

int plat_reopen_tty(void) {
    int fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;
    if (fd != STDIN_FILENO) {
        if (dup2(fd, STDIN_FILENO) < 0) { close(fd); return -1; }
        close(fd);
    }
    clearerr(stdin);
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

static char *dupstr(const char *s) {
    char *r;
    if (!s) return NULL;
    r = malloc(strlen(s) + 1);
    if (r) strcpy(r, s);
    return r;
}

static int mkdirs(const char *path, mode_t mode) {
    char *tmp;
    size_t i, n = strlen(path);
    int rc = 0;
    if (n == 0) return -1;
    tmp = dupstr(path);
    if (!tmp) return -1;
    for (i = 1; i < n; i++) {
        if (tmp[i] == '/' && tmp[i - 1] != '/') {
            tmp[i] = 0;
            if (mkdir(tmp, mode) != 0 && errno != EEXIST) { /* checked below */ }
            tmp[i] = '/';
        }
    }
    if (mkdir(tmp, mode) != 0 && (errno != EEXIST || !plat_is_dir(tmp))) rc = -1;
    free(tmp);
    return rc;
}

/* 0777 filtered by the umask, like mkdir -p */
int plat_mkdir_p(const char *path) { return mkdirs(path, 0777); }
int plat_mkdir_private(const char *path) { return mkdirs(path, 0700); }

int plat_owns_file(const char *path) {
    struct stat st;
    return lstat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == geteuid();
}

char *plat_getcwd(void) {
    size_t cap = 4096;
    for (;;) {
        char *buf = malloc(cap);
        if (!buf) return NULL;
        if (getcwd(buf, cap)) return buf;
        free(buf);
        if (errno != ERANGE || cap > (1u << 20)) return NULL;
        cap *= 2;
    }
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
    int dfd;
    if (!d) return NULL;
    dfd = dirfd(d);
    while ((e = readdir(d)) != NULL) {
        size_t len;
        char *s;
        struct stat st;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        len = strlen(e->d_name);
        s = malloc(len + 2);
        if (!s) break;
        memcpy(s, e->d_name, len + 1);
        /* follows symlinks, so links to directories count as directories */
        if (dfd >= 0 && fstatat(dfd, e->d_name, &st, 0) == 0 && S_ISDIR(st.st_mode)) {
            s[len] = '/';
            s[len + 1] = 0;
        }
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

char *plat_getenv(const char *name) { return dupstr(getenv(name)); }

char *plat_home_dir(void) {
    const char *h = getenv("HOME");
    if (h && *h) return dupstr(h);
    {
        /* $HOME unset or empty: ask the password database rather than
           using "" (which would put the config dir at /.config) */
        struct passwd *pw = getpwuid(getuid());
        if (pw && pw->pw_dir && *pw->pw_dir) return dupstr(pw->pw_dir);
    }
    return NULL;
}

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
    if (!qo || (in && !qi)) { free(qi); free(qo); return NULL; }
    n = strlen(cmd) + strlen(fi) + strlen(qo) + 32;
    r = malloc(n);
    /* the newline ends a trailing `# comment` in cmd */
    if (r) snprintf(r, n, "( %s\n) < %s > %s 2>&1", cmd, fi, qo);
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
            if (cap > (size_t)LONG_MAX / 2 - 1) break; /* absurdly large */
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

/* ---------------- processes ---------------- */

int plat_run(const char *const *argv, const char *in_path) {
    struct sigaction ign, old_int, old_quit;
    pid_t pid;
    int status = 0, in_fd, null_fd;
    if (!argv || !argv[0] || !*argv[0]) return -1;
    in_fd = open(in_path ? in_path : "/dev/null", O_RDONLY | O_CLOEXEC);
    if (in_fd < 0) return -1;
    null_fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (null_fd < 0) { close(in_fd); return -1; }
    /* like system(): Ctrl-C at a password prompt stops the child, not us */
    memset(&ign, 0, sizeof ign);
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    sigaction(SIGINT, &ign, &old_int);
    sigaction(SIGQUIT, &ign, &old_quit);
    fflush(NULL);
    pid = fork();
    if (pid == 0) {
        sigaction(SIGINT, &old_int, NULL);
        sigaction(SIGQUIT, &old_quit, NULL);
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        if (dup2(in_fd, STDIN_FILENO) < 0 || dup2(null_fd, STDOUT_FILENO) < 0 ||
            dup2(null_fd, STDERR_FILENO) < 0)
            _exit(127);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    close(in_fd);
    close(null_fd);
    if (pid > 0) {
        while (waitpid(pid, &status, 0) < 0) {
            if (errno != EINTR) { status = -1; break; }
        }
    }
    sigaction(SIGINT, &old_int, NULL);
    sigaction(SIGQUIT, &old_quit, NULL);
    if (pid < 0 || status == -1) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status) == 127 ? -1 : WEXITSTATUS(status);
    return -1;
}

/* ---------------- safe file writing ---------------- */

static char save_err[256];

static void set_err(const char *what, int e) {
    snprintf(save_err, sizeof save_err, "%s: %s", what, strerror(e));
}

const char *plat_save_error(void) { return save_err[0] ? save_err : "unknown error"; }

struct PlatSave {
    int fd;
    char *tmp;    /* temporary file that replaces `dest` on commit, or NULL */
    char *dest;   /* final name (symlinks resolved) */
    int failed;
    int is_reg;   /* in place on a regular file: truncate at the end */
    int priv;     /* PLAT_SAVE_PRIVATE */
    int keep_tmp; /* an in-place copy failed part-way: tmp holds the only full copy */
    size_t nbuf;
    char buf[1 << 16];
};

static int write_all(int fd, const char *p, size_t n) {
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

/* dir/.base.XXXXXX next to `dest`; the base is shortened so the name fits */
static char *temp_name_for(const char *dest) {
    const char *slash = strrchr(dest, '/'), *base = slash ? slash + 1 : dest;
    size_t dlen = slash ? (size_t)(slash - dest) + 1 : 0, blen = strlen(base);
    char *r;
    if (blen > 64) blen = 64;
    r = malloc(dlen + blen + 16);
    if (!r) return NULL;
    memcpy(r, dest, dlen);
    r[dlen] = '.';
    memcpy(r + dlen + 1, base, blen);
    strcpy(r + dlen + 1 + blen, ".micro-XXXXXX");
    return r;
}

static int is_denied(int e) { return e == EACCES || e == EPERM || e == EROFS; }

/* make room for `size` bytes before anything is overwritten, so a full
   disk can't leave the file half written */
static int reserve_space(int fd, off_t old_size, long long size) {
#if defined(__linux__) || defined(__FreeBSD__)
    if (size > (long long)old_size) {
        int e = posix_fallocate(fd, 0, (off_t)size);
        if (e == ENOSPC || e == EDQUOT || e == EFBIG) {
            if (ftruncate(fd, old_size) != 0) { /* nothing more to undo */ }
            errno = e;
            return -1;
        }
    }
#else
    (void)fd; (void)old_size; (void)size;
#endif
    return 0;
}

#ifdef __linux__
/* copy the extended attributes of `src` (ACLs, SELinux labels, file
   capabilities, user.* ...) onto the new file `fd`: 0 when all of them
   were copied (or there are none / the file system has none), -1 if any
   couldn't be, in which case the file must be rewritten in place */
static int copy_xattrs(const char *src, int fd) {
    ssize_t n = listxattr(src, NULL, 0), m;
    char *names, *p, *val = NULL;
    size_t valcap = 0;
    int rc = 0;
    if (n < 0) return errno == ENOTSUP || errno == EOPNOTSUPP ? 0 : -1;
    if (n == 0) return 0;
    names = malloc((size_t)n);
    if (!names) return -1;
    m = listxattr(src, names, (size_t)n);
    if (m < 0 || m > n) { free(names); return -1; }
    for (p = names; rc == 0 && p < names + m; p += strlen(p) + 1) {
        ssize_t v = getxattr(src, p, NULL, 0);
        if (v < 0) { rc = -1; break; }
        if ((size_t)v + 1 > valcap) {
            char *nv = realloc(val, (size_t)v + 1);
            if (!nv) { rc = -1; break; }
            val = nv;
            valcap = (size_t)v + 1;
        }
        v = getxattr(src, p, val, valcap);
        if (v < 0 || fsetxattr(fd, p, val, (size_t)v, 0) != 0) rc = -1;
    }
    free(val);
    free(names);
    return rc;
}
#endif

/* does the file carry extended attributes (ACLs, security labels, file
   capabilities, user.* attributes)?  A new inode wouldn't have them, so
   such a file is rewritten in place.  When unsure, say yes.  (Linux
   copies them to the new file instead, see copy_xattrs.) */
static int has_xattrs(const char *path) {
#if defined(__APPLE__)
    ssize_t n = listxattr(path, NULL, 0, 0);
    if (n < 0) {
#ifdef ENOTSUP
        if (errno == ENOTSUP) return 0;
#endif
#ifdef EOPNOTSUPP
        if (errno == EOPNOTSUPP) return 0;
#endif
        return 1;
    }
    return n > 0;
#else
    (void)path;
    return 0;
#endif
}

PlatSave *plat_save_open(const char *path, long long size, int flags, int *denied) {
    PlatSave *s;
    struct stat st;
    int have, priv = flags & PLAT_SAVE_PRIVATE;
    char *real = NULL;
    if (denied) *denied = 0;
    save_err[0] = 0;
    s = malloc(sizeof *s);
    if (!s) { set_err(path, ENOMEM); return NULL; }
    s->fd = -1;
    s->tmp = NULL;
    s->failed = 0;
    s->is_reg = 0;
    s->nbuf = 0;
    s->priv = priv != 0;
    s->keep_tmp = 0;
    if (priv) {
        /* never follow a symlink planted at a state-file path */
        have = lstat(path, &st) == 0;
        s->dest = dupstr(path);
    } else {
        have = stat(path, &st) == 0;
        real = have ? realpath(path, NULL) : NULL;
        s->dest = real ? real : dupstr(path);
    }
    if (!s->dest) { set_err(path, ENOMEM); free(s); return NULL; }

    /* replace through a temporary file when that changes nothing else
       about the file: a writable regular file we own, with no other hard
       links and no extended attributes (ACLs, SELinux labels...).  A
       read-only file goes to the in-place open below, which reports the
       permission problem. */
    if (priv || (have && S_ISREG(st.st_mode) && st.st_nlink == 1 && st.st_uid == geteuid() &&
                 (st.st_mode & S_IWUSR) && !has_xattrs(s->dest))) {
        char *tmp = temp_name_for(s->dest);
        int fd = tmp ? mkstemp(tmp) : -1;
        if (fd >= 0) {
            int ok;
            if (priv) {
                ok = fchmod(fd, 0600) == 0;
            } else {
                ok = fchmod(fd, st.st_mode & 07777) == 0;
                /* keep the group; if we can't, don't change the file's group */
                if (ok) {
                    struct stat ts;
                    if (fstat(fd, &ts) == 0 && ts.st_gid != st.st_gid)
                        ok = fchown(fd, (uid_t)-1, st.st_gid) == 0;
                }
#ifdef __linux__
                /* after fchmod: an ACL also sets the group mode bits */
                if (ok) ok = copy_xattrs(s->dest, fd) == 0;
#endif
            }
            if (ok) {
                s->fd = fd;
                s->tmp = tmp;
                return s;
            }
            close(fd);
            unlink(tmp);
        } else if (priv) {
            int e = errno;
            set_err(s->dest, e);
            if (denied) *denied = is_denied(e);
            free(tmp);
            free(s->dest);
            free(s);
            return NULL;
        }
        free(tmp);
    }

    /* rewrite in place (keeps hard links, ownership, ACLs) */
    s->fd = open(s->dest, O_WRONLY | O_CREAT | O_CLOEXEC | (priv ? O_NOFOLLOW : 0), priv ? 0600 : 0666);
    if (s->fd < 0) {
        int e = errno;
        set_err(s->dest, e);
        if (denied) *denied = is_denied(e);
        free(s->dest);
        free(s);
        return NULL;
    }
    {
        struct stat cur;
        if (fstat(s->fd, &cur) == 0 && S_ISREG(cur.st_mode)) {
            s->is_reg = 1;
            if (size >= 0 && reserve_space(s->fd, cur.st_size, size) != 0) {
                set_err(s->dest, errno);
                close(s->fd);
                free(s->dest);
                free(s);
                return NULL;
            }
            if (size < 0 && ftruncate(s->fd, 0) != 0) { /* reported by the writes */ }
        }
    }
    return s;
}

static int flush_buf(PlatSave *s) {
    if (s->nbuf && !s->failed && write_all(s->fd, s->buf, s->nbuf) != 0) {
        s->failed = 1;
        set_err(s->dest, errno);
    }
    s->nbuf = 0;
    return s->failed ? -1 : 0;
}

int plat_save_write(PlatSave *s, const void *data, size_t n) {
    const char *p = data;
    if (s->failed) return -1;
    if (n >= sizeof s->buf) {
        if (flush_buf(s) != 0) return -1;
        if (write_all(s->fd, p, n) != 0) {
            s->failed = 1;
            set_err(s->dest, errno);
            return -1;
        }
        return 0;
    }
    if (s->nbuf + n > sizeof s->buf && flush_buf(s) != 0) return -1;
    memcpy(s->buf + s->nbuf, p, n);
    s->nbuf += n;
    return 0;
}

/* rename() couldn't replace s->dest (EBUSY: it is a bind mount, say):
   copy the finished temporary file over it in place instead */
static int copy_into_dest(PlatSave *s) {
    int in, out, rc = -1, e = 0, touched = 0;
    struct stat ts, ds;
    in = open(s->tmp, O_RDONLY | O_CLOEXEC);
    if (in < 0) { set_err(s->dest, errno); return -1; }
    out = open(s->dest, O_WRONLY | O_CLOEXEC | (s->priv ? O_NOFOLLOW : 0));
    if (out < 0) { e = errno; goto done; }
    if (fstat(in, &ts) != 0 || fstat(out, &ds) != 0) { e = errno; goto done; }
    if (S_ISREG(ds.st_mode) && reserve_space(out, ds.st_size, (long long)ts.st_size) != 0) {
        e = errno;
        goto done;
    }
    for (;;) {
        ssize_t n = read(in, s->buf, sizeof s->buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            e = errno;
            goto done;
        }
        if (n == 0) break;
        touched = 1;
        if (write_all(out, s->buf, (size_t)n) != 0) { e = errno; goto done; }
    }
    if (S_ISREG(ds.st_mode) && ftruncate(out, ts.st_size) != 0) { e = errno; goto done; }
    if (fsync(out) != 0 && errno != EINVAL) { e = errno; goto done; }
    rc = 0;
done:
    if (out >= 0 && close(out) != 0 && rc == 0) { e = errno; rc = -1; }
    close(in);
    if (rc != 0) {
        set_err(s->dest, e);
        if (touched) {
            /* dest may now be half rewritten: keep the complete new
               contents and say where they are */
            size_t l = strlen(save_err);
            s->keep_tmp = 1;
            snprintf(save_err + l, sizeof save_err - l, "; new contents kept in %s", s->tmp);
        }
    }
    return rc;
}

int plat_save_close(PlatSave *s, int commit) {
    int rc = 0;
    if (!s) return -1;
    if (flush_buf(s) != 0) rc = -1;
    if (rc == 0 && commit) {
        off_t end = lseek(s->fd, 0, SEEK_CUR);
        /* in place: drop whatever is left of the old, longer contents */
        if (!s->tmp && s->is_reg && end >= 0 && ftruncate(s->fd, end) != 0) { set_err(s->dest, errno); rc = -1; }
        if (rc == 0 && (s->tmp || s->is_reg) && fsync(s->fd) != 0 && errno != EINVAL) {
            set_err(s->dest, errno);
            rc = -1;
        }
    }
    if (close(s->fd) != 0 && rc == 0 && commit) { set_err(s->dest, errno); rc = -1; }
    if (s->tmp) {
        int renamed = 0;
        if (rc == 0 && commit) {
            if (rename(s->tmp, s->dest) == 0) {
                renamed = 1;
            } else {
                int e = errno;
                if (e == EBUSY || e == EXDEV || e == EINVAL) {
                    rc = copy_into_dest(s);
                } else {
                    set_err(s->dest, e);
                    rc = -1;
                }
            }
        }
        if (!renamed && !s->keep_tmp) unlink(s->tmp);
        free(s->tmp);
    }
    free(s->dest);
    free(s);
    return commit ? rc : 0;
}

FILE *plat_temp_file(const char *tag, char **path) {
    char *dir = plat_temp_dir(), *name;
    size_t n;
    int fd;
    FILE *f;
    *path = NULL;
    if (!dir) return NULL;
    n = strlen(dir) + strlen(tag) + 24;
    name = malloc(n);
    if (!name) { free(dir); return NULL; }
    snprintf(name, n, "%s/micro-%s-XXXXXX", dir, tag);
    free(dir);
    fd = mkstemp(name); /* O_EXCL, mode 0600 */
    if (fd < 0) { free(name); return NULL; }
    f = fdopen(fd, "w+b");
    if (!f) { close(fd); unlink(name); free(name); return NULL; }
    *path = name;
    return f;
}

#endif /* !_WIN32 */

/* ISO C forbids an empty translation unit */
typedef int platform_posix_unused;
