/*
 * platform_win32.c - native Windows glue (Windows 10 1809+ console with
 * virtual terminal sequences: Windows Terminal, conhost, PowerShell, cmd).
 *
 * The console is put into VT mode in both directions, so the rest of the
 * editor's escape-sequence renderer and input decoder work unchanged.
 * Input is read with ReadConsoleInputW and re-encoded as UTF-8; all file
 * names, environment variables and command lines go through the UTF-16
 * ("W") APIs so non-ASCII paths work regardless of the ANSI code page.
 */
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "platform.h"

#ifndef ENABLE_VIRTUAL_TERMINAL_INPUT
#define ENABLE_VIRTUAL_TERMINAL_INPUT 0x0200
#endif
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#ifndef DISABLE_NEWLINE_AUTO_RETURN
#define DISABLE_NEWLINE_AUTO_RETURN 0x0008
#endif
#ifndef ENABLE_EXTENDED_FLAGS
#define ENABLE_EXTENDED_FLAGS 0x0080
#endif

/* ---------------- UTF-8 <-> UTF-16 ---------------- */

static wchar_t *to_w(const char *s) {
    int n;
    wchar_t *w;
    if (!s) return NULL;
    n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    w = malloc(sizeof(wchar_t) * (size_t)n);
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static char *from_w(const wchar_t *w) {
    int n;
    char *s;
    if (!w) return NULL;
    n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    s = malloc((size_t)n);
    if (!s) return NULL;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

/* ---------------- terminal ---------------- */

static HANDLE hin = INVALID_HANDLE_VALUE, hout = INVALID_HANDLE_VALUE;
static DWORD orig_in_mode, orig_out_mode;
static UINT orig_cp, orig_out_cp;
static int raw_on;
static int resized_flag;
static int last_w = -1, last_h = -1;

/* UTF-8 bytes decoded from console input but not yet returned */
static char pending[4096];
static int npending;
static WCHAR high_surrogate;

static void open_console(void) {
    if (hin == INVALID_HANDLE_VALUE)
        hin = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                          NULL, OPEN_EXISTING, 0, NULL);
    if (hout == INVALID_HANDLE_VALUE)
        hout = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
}

int plat_term_init(void) {
    DWORD in_mode, out_mode;
    open_console();
    if (hin == INVALID_HANDLE_VALUE || hout == INVALID_HANDLE_VALUE) return -1;
    if (!GetConsoleMode(hin, &orig_in_mode) || !GetConsoleMode(hout, &orig_out_mode)) return -1;
    orig_cp = GetConsoleCP();
    orig_out_cp = GetConsoleOutputCP();
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
    /* no line editing/echo/Ctrl-C processing; quick-edit off so the mouse
       goes to the editor; VT sequences for keys and mouse */
    in_mode = ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT | ENABLE_EXTENDED_FLAGS;
    out_mode = orig_out_mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING |
               DISABLE_NEWLINE_AUTO_RETURN;
    if (!SetConsoleMode(hout, out_mode)) {
        /* very old console without VT support */
        SetConsoleCP(orig_cp);
        SetConsoleOutputCP(orig_out_cp);
        return -1;
    }
    if (!SetConsoleMode(hin, in_mode)) {
        SetConsoleMode(hout, orig_out_mode);
        SetConsoleCP(orig_cp);
        SetConsoleOutputCP(orig_out_cp);
        return -1;
    }
    /* Ctrl-C is a key (Copy), never an interrupt */
    SetConsoleCtrlHandler(NULL, TRUE);
    raw_on = 1;
    plat_term_size(&last_w, &last_h);
    return 0;
}

void plat_term_restore(void) {
    if (!raw_on) return;
    SetConsoleMode(hin, orig_in_mode);
    SetConsoleMode(hout, orig_out_mode);
    SetConsoleCP(orig_cp);
    SetConsoleOutputCP(orig_out_cp);
    SetConsoleCtrlHandler(NULL, FALSE);
    raw_on = 0;
}

int plat_term_size(int *w, int *h) {
    CONSOLE_SCREEN_BUFFER_INFO info;
    open_console();
    if (!GetConsoleScreenBufferInfo(hout, &info)) {
        *w = 80;
        *h = 24;
        return -1;
    }
    *w = info.srWindow.Right - info.srWindow.Left + 1;
    *h = info.srWindow.Bottom - info.srWindow.Top + 1;
    return 0;
}

static void push_utf8(unsigned cp) {
    char b[4];
    int n;
    if (cp < 0x80) { b[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { b[0] = (char)(0xC0 | (cp >> 6)); b[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) {
        b[0] = (char)(0xE0 | (cp >> 12)); b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[2] = (char)(0x80 | (cp & 0x3F)); n = 3;
    } else {
        b[0] = (char)(0xF0 | (cp >> 18)); b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[3] = (char)(0x80 | (cp & 0x3F)); n = 4;
    }
    if (npending + n <= (int)sizeof pending) {
        memcpy(pending + npending, b, (size_t)n);
        npending += n;
    }
}

static void push_str(const char *t) {
    while (*t) push_utf8((unsigned char)*t++);
}

/* xterm modifier parameter: 1 + shift + 2*alt + 4*ctrl */
static int mod_param(DWORD st) {
    int m = 1;
    if (st & SHIFT_PRESSED) m += 1;
    if (st & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) m += 2;
    if (st & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) m += 4;
    return m;
}

/* Keys that carry no character (arrows, F-keys, ...) on consoles that don't
   do VT input themselves (older conhost, Wine): emit xterm sequences. */
static int translate_vk(WORD vk, DWORD st) {
    static const struct { WORD vk; char fin; int num; } map[] = {
        {VK_UP, 'A', 0}, {VK_DOWN, 'B', 0}, {VK_RIGHT, 'C', 0}, {VK_LEFT, 'D', 0},
        {VK_HOME, 'H', 0}, {VK_END, 'F', 0}, {VK_INSERT, '~', 2}, {VK_DELETE, '~', 3},
        {VK_PRIOR, '~', 5}, {VK_NEXT, '~', 6}, {VK_F1, 'P', 0}, {VK_F2, 'Q', 0},
        {VK_F3, 'R', 0}, {VK_F4, 'S', 0}, {VK_F5, '~', 15}, {VK_F6, '~', 17},
        {VK_F7, '~', 18}, {VK_F8, '~', 19}, {VK_F9, '~', 20}, {VK_F10, '~', 21},
        {VK_F11, '~', 23}, {VK_F12, '~', 24},
    };
    size_t i;
    char seq[32];
    int m = mod_param(st);
    for (i = 0; i < sizeof map / sizeof map[0]; i++) {
        if (map[i].vk != vk) continue;
        if (map[i].fin == '~') {
            if (m > 1) sprintf(seq, "\x1b[%d;%d~", map[i].num, m);
            else sprintf(seq, "\x1b[%d~", map[i].num);
        } else if (m > 1) {
            sprintf(seq, "\x1b[1;%d%c", m, map[i].fin);
        } else if (vk >= VK_F1 && vk <= VK_F4) {
            sprintf(seq, "\x1bO%c", map[i].fin);
        } else {
            sprintf(seq, "\x1b[%c", map[i].fin);
        }
        push_str(seq);
        return 1;
    }
    return 0;
}

/* Mouse records (classic console window) become SGR mouse sequences, the
   same thing terminals send, so the editor's decoder handles both. */
static DWORD last_buttons;

static void translate_mouse(const MOUSE_EVENT_RECORD *me) {
    CONSOLE_SCREEN_BUFFER_INFO info;
    int x = me->dwMousePosition.X, y = me->dwMousePosition.Y, mods = 0, b;
    DWORD btns = me->dwButtonState & 0xFFFF, changed;
    char seq[48];
    if (GetConsoleScreenBufferInfo(hout, &info)) {
        x -= info.srWindow.Left;
        y -= info.srWindow.Top;
    }
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (me->dwControlKeyState & SHIFT_PRESSED) mods |= 4;
    if (me->dwControlKeyState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) mods |= 8;
    if (me->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) mods |= 16;
    if (me->dwEventFlags & (MOUSE_WHEELED | MOUSE_HWHEELED)) {
        int up = (short)HIWORD(me->dwButtonState) > 0;
        b = (me->dwEventFlags & MOUSE_HWHEELED) ? (up ? 67 : 66) : (up ? 64 : 65);
        sprintf(seq, "\x1b[<%d;%d;%dM", b | mods, x + 1, y + 1);
        push_str(seq);
        return;
    }
    changed = btns ^ last_buttons;
    if (changed) {
        static const DWORD bits[3] = {FROM_LEFT_1ST_BUTTON_PRESSED, RIGHTMOST_BUTTON_PRESSED,
                                      FROM_LEFT_2ND_BUTTON_PRESSED};
        int k;
        for (k = 0; k < 3; k++) {
            if (!(changed & bits[k])) continue;
            /* SGR button codes: 0 left, 1 middle, 2 right */
            sprintf(seq, "\x1b[<%d;%d;%d%c", (k == 0 ? 0 : k == 1 ? 2 : 1) | mods, x + 1, y + 1,
                    (btns & bits[k]) ? 'M' : 'm');
            push_str(seq);
        }
    } else if ((me->dwEventFlags & MOUSE_MOVED) && btns) {
        b = (btns & FROM_LEFT_1ST_BUTTON_PRESSED) ? 0 : (btns & RIGHTMOST_BUTTON_PRESSED) ? 2 : 1;
        sprintf(seq, "\x1b[<%d;%d;%dM", (32 + b) | mods, x + 1, y + 1);
        push_str(seq);
    }
    last_buttons = btns;
}

/* drain available console input records into `pending` */
static void read_records(void) {
    INPUT_RECORD rec[128];
    DWORD got = 0, i, avail = 0;
    if (!GetNumberOfConsoleInputEvents(hin, &avail) || avail == 0) return;
    if (!ReadConsoleInputW(hin, rec, 128, &got)) return;
    for (i = 0; i < got; i++) {
        if (rec[i].EventType == WINDOW_BUFFER_SIZE_EVENT) {
            resized_flag = 1;
        } else if (rec[i].EventType == MOUSE_EVENT) {
            translate_mouse(&rec[i].Event.MouseEvent);
        } else if (rec[i].EventType == KEY_EVENT && rec[i].Event.KeyEvent.bKeyDown) {
            const KEY_EVENT_RECORD *ke = &rec[i].Event.KeyEvent;
            WCHAR ch = ke->uChar.UnicodeChar;
            DWORD st = ke->dwControlKeyState;
            WORD rep = ke->wRepeatCount, k;
            int alt = (st & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) && !(st & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED));
            for (k = 0; k < (rep ? rep : 1); k++) {
                if (ch == 0) {
                    /* no text: a special key the console didn't translate */
                    if (!translate_vk(ke->wVirtualKeyCode, st) && ke->wVirtualKeyCode == VK_SPACE &&
                        (st & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)))
                        push_utf8(0); /* Ctrl-Space */
                    continue;
                }
                if (ch == '\t' && (st & SHIFT_PRESSED) && ke->wVirtualKeyCode == VK_TAB) {
                    push_str("\x1b[Z"); /* Shift-Tab */
                    continue;
                }
                /* Alt+key on consoles without VT input: prefix ESC like a
                   terminal does (VT-mode consoles already sent the ESC) */
                if (alt && ch != 0x1b && !(npending > 0 && pending[npending - 1] == 0x1b)) push_utf8(0x1b);
                if (ch >= 0xD800 && ch <= 0xDBFF) {
                    high_surrogate = ch;
                } else if (ch >= 0xDC00 && ch <= 0xDFFF) {
                    if (high_surrogate)
                        push_utf8(0x10000u + (((unsigned)high_surrogate - 0xD800u) << 10) + ((unsigned)ch - 0xDC00u));
                    high_surrogate = 0;
                } else {
                    push_utf8(ch);
                }
            }
        }
    }
}

int plat_read(char *buf, int n, int timeout_ms) {
    ULONGLONG deadline = GetTickCount64() + (ULONGLONG)(timeout_ms < 0 ? 0 : timeout_ms);
    for (;;) {
        if (npending > 0) {
            int k = npending < n ? npending : n;
            memcpy(buf, pending, (size_t)k);
            memmove(pending, pending + k, (size_t)(npending - k));
            npending -= k;
            return k;
        }
        {
            ULONGLONG now = GetTickCount64();
            DWORD wait = timeout_ms < 0 ? INFINITE : (now >= deadline ? 0 : (DWORD)(deadline - now));
            DWORD r = WaitForSingleObject(hin, wait);
            if (r == WAIT_FAILED) return -1;
            if (r != WAIT_OBJECT_0) return 0;
            read_records();
            if (npending == 0 && resized_flag) return 0; /* let the caller handle it */
            if (npending == 0 && timeout_ms >= 0 && GetTickCount64() >= deadline) return 0;
        }
    }
}

void plat_write(const char *buf, size_t n) {
    while (n > 0) {
        DWORD w = 0, chunk = n > 0x10000000 ? 0x10000000 : (DWORD)n;
        if (!WriteFile(hout, buf, chunk, &w, NULL) || w == 0) return;
        buf += w;
        n -= w;
    }
}

int plat_resized(void) {
    int w, h;
    if (resized_flag) {
        resized_flag = 0;
        plat_term_size(&last_w, &last_h);
        return 1;
    }
    /* not every console reports window (as opposed to buffer) resizes */
    if (plat_term_size(&w, &h) == 0 && (w != last_w || h != last_h)) {
        last_w = w;
        last_h = h;
        return 1;
    }
    return 0;
}

int plat_can_suspend(void) { return 0; }
int plat_suspend(void) { return 0; }

int plat_stdin_is_tty(void) {
    return GetFileType(GetStdHandle(STD_INPUT_HANDLE)) == FILE_TYPE_CHAR;
}

int plat_reopen_tty(void) {
    /* keyboard input is always read from CONIN$; make stdio's stdin the
       console again for line prompts (backup recovery, shell mode) */
    return freopen("CONIN$", "r", stdin) ? 0 : -1;
}

char *plat_read_stdin(long *len) {
    size_t cap = 65536, n = 0, got;
    char *buf = malloc(cap + 1), *nb;
    if (!buf) return NULL;
    _setmode(_fileno(stdin), _O_BINARY);
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

void plat_args(int *argc, char ***argv) {
    int n, i;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &n);
    char **v;
    if (!wargv) return;
    v = malloc(sizeof(char *) * (size_t)(n + 1));
    if (!v) { LocalFree(wargv); return; }
    for (i = 0; i < n; i++) v[i] = from_w(wargv[i]);
    v[n] = NULL;
    LocalFree(wargv);
    *argc = n;
    *argv = v; /* lives for the whole program */
}

long long plat_now_ms(void) { return (long long)GetTickCount64(); }

/* ---------------- files ---------------- */

FILE *plat_fopen(const char *path, const char *mode) {
    wchar_t *wp = to_w(path), *wm = to_w(mode);
    FILE *f = wp && wm ? _wfopen(wp, wm) : NULL;
    free(wp);
    free(wm);
    return f;
}

int plat_remove(const char *path) {
    wchar_t *wp = to_w(path);
    int r = wp ? _wremove(wp) : -1;
    free(wp);
    return r;
}

static DWORD attrs(const char *path) {
    wchar_t *wp = to_w(path);
    DWORD a = wp ? GetFileAttributesW(wp) : INVALID_FILE_ATTRIBUTES;
    free(wp);
    return a;
}

int plat_is_dir(const char *path) {
    DWORD a = attrs(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

int plat_exists(const char *path) { return attrs(path) != INVALID_FILE_ATTRIBUTES; }

int plat_is_writable(const char *path) {
    DWORD a = attrs(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_READONLY);
}

long long plat_mtime(const char *path) {
    struct _stat64 st;
    wchar_t *wp = to_w(path);
    long long r = 0;
    if (wp && _wstat64(wp, &st) == 0) r = (long long)st.st_mtime;
    free(wp);
    return r;
}

int plat_is_sep(int c) { return c == '/' || c == '\\'; }

int plat_is_abs(const char *p) {
    if (plat_is_sep((unsigned char)p[0])) return 1; /* \dir or \\server\share */
    return ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':' &&
           plat_is_sep((unsigned char)p[2]);
}

int plat_mkdir_p(const char *path) {
    size_t i, n = strlen(path), start = 0;
    char *tmp = malloc(n + 1);
    int ok;
    if (!tmp || n == 0) { free(tmp); return -1; }
    memcpy(tmp, path, n + 1);
    if (n >= 2 && tmp[1] == ':') start = 3;              /* C:\ */
    else if (n >= 2 && plat_is_sep(tmp[0]) && plat_is_sep(tmp[1])) start = n; /* UNC: only the leaf */
    for (i = start; i < n; i++) {
        if (plat_is_sep((unsigned char)tmp[i]) && i > 0) {
            wchar_t *w;
            char c = tmp[i];
            tmp[i] = 0;
            w = to_w(tmp);
            if (w) CreateDirectoryW(w, NULL);
            free(w);
            tmp[i] = c;
        }
    }
    {
        wchar_t *w = to_w(tmp);
        ok = w && (CreateDirectoryW(w, NULL) || GetLastError() == ERROR_ALREADY_EXISTS);
        free(w);
    }
    free(tmp);
    return ok ? 0 : -1;
}

char *plat_getcwd(void) {
    wchar_t buf[32768];
    if (!_wgetcwd(buf, 32768)) return NULL;
    return from_w(buf);
}

int plat_chdir(const char *path) {
    wchar_t *w = to_w(path);
    int r = w ? _wchdir(w) : -1;
    free(w);
    return r;
}

char *plat_realpath(const char *path) {
    wchar_t *w = to_w(path), buf[32768];
    DWORD n;
    char *r = NULL;
    if (!w) return NULL;
    n = GetFullPathNameW(w, 32768, buf, NULL);
    free(w);
    if (n == 0 || n >= 32768) return NULL;
    if (GetFileAttributesW(buf) == INVALID_FILE_ATTRIBUTES) return NULL;
    r = from_w(buf);
    return r;
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

char **plat_list_dir(const char *path) {
    WIN32_FIND_DATAW fd;
    HANDLE h;
    char *pattern;
    wchar_t *wpat;
    char **out = NULL;
    size_t n = 0, cap = 0, plen;
    const char *dir = path && *path ? path : ".";
    plen = strlen(dir);
    pattern = malloc(plen + 3);
    if (!pattern) return NULL;
    memcpy(pattern, dir, plen);
    if (plen > 0 && !plat_is_sep((unsigned char)dir[plen - 1])) pattern[plen++] = '\\';
    pattern[plen++] = '*';
    pattern[plen] = 0;
    wpat = to_w(pattern);
    free(pattern);
    if (!wpat) return NULL;
    h = FindFirstFileW(wpat, &fd);
    free(wpat);
    if (h == INVALID_HANDLE_VALUE) {
        out = malloc(sizeof *out);
        if (out) out[0] = NULL;
        return out;
    }
    do {
        char *name, *s;
        size_t len;
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        name = from_w(fd.cFileName);
        if (!name) continue;
        len = strlen(name);
        s = malloc(len + 2);
        if (!s) { free(name); break; }
        memcpy(s, name, len + 1);
        free(name);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { s[len] = '/'; s[len + 1] = 0; }
        if (n + 2 > cap) {
            char **nn;
            cap = cap ? cap * 2 : 32;
            nn = realloc(out, cap * sizeof *out);
            if (!nn) { free(s); break; }
            out = nn;
        }
        out[n++] = s;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (!out) { out = malloc(sizeof *out); if (!out) return NULL; }
    out[n] = NULL;
    qsort(out, n, sizeof *out, cmp_str);
    return out;
}

/* ---------------- environment / processes ---------------- */

char *plat_getenv(const char *name) {
    wchar_t *wn = to_w(name), *v;
    char *r;
    if (!wn) return NULL;
    v = _wgetenv(wn);
    free(wn);
    if (!v || !*v) return NULL;
    r = from_w(v);
    return r;
}

char *plat_home_dir(void) {
    char *h = plat_getenv("USERPROFILE");
    if (!h) {
        char *d = plat_getenv("HOMEDRIVE"), *p = plat_getenv("HOMEPATH");
        if (d && p) {
            h = malloc(strlen(d) + strlen(p) + 1);
            if (h) { strcpy(h, d); strcat(h, p); }
        }
        free(d);
        free(p);
    }
    if (!h) h = plat_getenv("HOME");
    return h;
}

char *plat_temp_dir(void) {
    wchar_t buf[MAX_PATH + 2];
    DWORD n = GetTempPathW(MAX_PATH + 1, buf);
    if (n == 0 || n > MAX_PATH) {
        char *r = malloc(2);
        if (r) strcpy(r, ".");
        return r;
    }
    if (n > 1 && (buf[n - 1] == L'\\' || buf[n - 1] == L'/')) buf[n - 1] = 0;
    {
        char *r = from_w(buf);
        if (r && !plat_is_dir(r)) plat_mkdir_p(r); /* missing in some profiles */
        return r;
    }
}

int plat_system(const char *cmd) {
    wchar_t *w = to_w(cmd);
    int r;
    if (!w) return -1;
    fflush(NULL);
    r = _wsystem(w);
    free(w);
    return r;
}

const char *plat_null_device(void) { return "NUL"; }

/* double quotes for cmd.exe (file names cannot contain '"') */
char *plat_shell_quote(const char *s) {
    size_t n = strlen(s);
    char *r = malloc(n + 3), *o = r;
    const char *p;
    if (!r) return NULL;
    *o++ = '"';
    for (p = s; *p; p++)
        if (*p != '"') *o++ = *p;
    *o++ = '"';
    *o = 0;
    return r;
}

/* run through a nested cmd so redirection covers compound commands */
char *plat_capture_cmd(const char *cmd, const char *in, const char *out) {
    char *qi = in ? plat_shell_quote(in) : NULL, *qo = plat_shell_quote(out), *r;
    const char *fi = qi ? qi : "NUL";
    size_t n;
    if (!qo) { free(qi); return NULL; }
    n = strlen(cmd) + strlen(fi) + strlen(qo) + 48;
    r = malloc(n);
    if (r) sprintf(r, "cmd /d /s /c \"%s\" < %s > %s 2>&1", cmd, fi, qo);
    free(qi);
    free(qo);
    return r;
}

int plat_has_sudo(void) { return 0; }

const char *plat_default_fileformat(void) { return "dos"; }

/* ---------------- clipboard ---------------- */

int plat_clipboard_set(const char *s, int len) {
    int i, wn, tries;
    char *crlf;
    size_t k = 0;
    wchar_t *w;
    HGLOBAL mem;
    /* text on the Windows clipboard uses CRLF line endings */
    crlf = malloc((size_t)len * 2 + 1);
    if (!crlf) return -1;
    for (i = 0; i < len; i++) {
        if (s[i] == '\n' && (i == 0 || s[i - 1] != '\r')) crlf[k++] = '\r';
        crlf[k++] = s[i];
    }
    crlf[k] = 0;
    wn = MultiByteToWideChar(CP_UTF8, 0, crlf, (int)k + 1, NULL, 0);
    mem = wn > 0 ? GlobalAlloc(GMEM_MOVEABLE, sizeof(wchar_t) * (size_t)wn) : NULL;
    if (!mem) { free(crlf); return -1; }
    w = GlobalLock(mem);
    MultiByteToWideChar(CP_UTF8, 0, crlf, (int)k + 1, w, wn);
    GlobalUnlock(mem);
    free(crlf);
    for (tries = 0; tries < 10 && !OpenClipboard(NULL); tries++) Sleep(10);
    if (tries == 10) { GlobalFree(mem); return -1; }
    EmptyClipboard();
    if (!SetClipboardData(CF_UNICODETEXT, mem)) {
        CloseClipboard();
        GlobalFree(mem);
        return -1;
    }
    CloseClipboard();
    return 0;
}

char *plat_clipboard_get(int *len) {
    HANDLE h;
    wchar_t *w;
    char *s = NULL;
    int tries;
    for (tries = 0; tries < 10 && !OpenClipboard(NULL); tries++) Sleep(10);
    if (tries == 10) return NULL;
    h = GetClipboardData(CF_UNICODETEXT);
    if (h && (w = GlobalLock(h)) != NULL) {
        s = from_w(w);
        GlobalUnlock(h);
    }
    CloseClipboard();
    if (s) {
        /* CRLF -> LF */
        char *r = s, *o = s;
        for (; *r; r++) {
            if (*r == '\r' && r[1] == '\n') continue;
            *o++ = *r;
        }
        *o = 0;
        *len = (int)(o - s);
    }
    return s;
}

#endif /* _WIN32 */

/* ISO C forbids an empty translation unit */
typedef int platform_win32_unused;
