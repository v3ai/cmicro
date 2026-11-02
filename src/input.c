/*
 * input.c - decode terminal input bytes into key, mouse and paste events,
 * and convert between key events and micro's key names ("Ctrl-s",
 * "AltShiftUp", "MouseWheelDown", ...).
 */
#include "micro.h"
#include "platform.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static unsigned char inbuf[8192];
static int inlen;

static int fill(int timeout) {
    int n;
    if (inlen >= (int)sizeof inbuf) return 0;
    n = plat_read((char *)inbuf + inlen, (int)sizeof inbuf - inlen, timeout);
    if (n > 0) inlen += n;
    return n;
}

static void consume(int n) {
    memmove(inbuf, inbuf + n, (size_t)(inlen - n));
    inlen -= n;
}

static void set_key(InEvent *ev, int key, int mods, int nbytes) {
    ev->type = IN_KEY;
    ev->key = key;
    ev->mods = mods;
    ev->rawlen = nbytes < (int)sizeof ev->raw ? nbytes : (int)sizeof ev->raw - 1;
    memcpy(ev->raw, inbuf, (size_t)ev->rawlen);
    ev->raw[ev->rawlen] = 0;
}

static int csi_mods(int p) { return p > 1 ? ((p - 1) & 7) : 0; }

/* parse a CSI sequence starting at inbuf[0]="\x1b[" ; returns bytes consumed, 0 if incomplete */
static int parse_csi(InEvent *ev) {
    int i = 2, params[8], np = 0, cur = -1, priv = 0;
    memset(params, 0, sizeof params);
    if (i < inlen && (inbuf[i] == '<' || inbuf[i] == '?' || inbuf[i] == '>')) { priv = inbuf[i]; i++; }
    if (i < inlen && inbuf[i] == 'M' && i == 2) {
        /* legacy X10 mouse report: ESC [ M b x y (bytes offset by 32) */
        int b, btn;
        if (inlen < 6) return 0;
        b = inbuf[3] - 32;
        btn = b & 3;
        ev->type = IN_MOUSE;
        ev->mx = inbuf[4] - 33;
        ev->my = inbuf[5] - 33;
        ev->mods = ((b & 4) ? MOD_SHIFT : 0) | ((b & 8) ? MOD_ALT : 0) | ((b & 16) ? MOD_CTRL : 0);
        if (ev->mx < 0) ev->mx = 0;
        if (ev->my < 0) ev->my = 0;
        if (b & 64) ev->key = KEY_WHEEL_UP + btn;
        else if (btn == 3) ev->key = KEY_MOUSE_LEFT_RELEASE;
        else if (b & 32) ev->key = btn == 1 ? KEY_MOUSE_MIDDLE_DRAG : btn == 2 ? KEY_MOUSE_RIGHT_DRAG : KEY_MOUSE_LEFT_DRAG;
        else ev->key = btn == 1 ? KEY_MOUSE_MIDDLE : btn == 2 ? KEY_MOUSE_RIGHT : KEY_MOUSE_LEFT;
        return 6;
    }
    for (; i < inlen; i++) {
        int c = inbuf[i];
        if (isdigit(c)) {
            if (cur < 0) cur = 0;
            if (cur < 10000000) cur = cur * 10 + (c - '0'); /* cap: no overflow */
        } else if (c == ';' || c == ':') {
            if (np < 8) params[np++] = cur < 0 ? 0 : cur;
            cur = -1;
        } else if (c >= 0x40 && c <= 0x7E) {
            int key = 0, mods = 0, n = i + 1;
            if (cur >= 0 || np > 0) { if (np < 8) params[np++] = cur < 0 ? 0 : cur; }
            if (priv == '<' && (c == 'M' || c == 'm') && np >= 3) {
                int b = params[0], btn = b & 3;
                ev->type = IN_MOUSE;
                ev->mx = params[1] - 1;
                ev->my = params[2] - 1;
                ev->mods = ((b & 4) ? MOD_SHIFT : 0) | ((b & 8) ? MOD_ALT : 0) | ((b & 16) ? MOD_CTRL : 0);
                if (b & 64) {
                    ev->key = KEY_WHEEL_UP + btn;
                } else if (c == 'm') {
                    ev->key = btn == 1 ? KEY_MOUSE_MIDDLE_RELEASE : btn == 2 ? KEY_MOUSE_RIGHT_RELEASE : KEY_MOUSE_LEFT_RELEASE;
                } else if (b & 32) {
                    if (btn == 3) return n; /* plain motion: ignore */
                    ev->key = btn == 1 ? KEY_MOUSE_MIDDLE_DRAG : btn == 2 ? KEY_MOUSE_RIGHT_DRAG : KEY_MOUSE_LEFT_DRAG;
                } else {
                    ev->key = btn == 1 ? KEY_MOUSE_MIDDLE : btn == 2 ? KEY_MOUSE_RIGHT : KEY_MOUSE_LEFT;
                }
                return n;
            }
            if (priv) { ev->type = IN_NONE; return n; }
            switch (c) {
            case 'A': key = KEY_UP; break;
            case 'B': key = KEY_DOWN; break;
            case 'C': key = KEY_RIGHT; break;
            case 'D': key = KEY_LEFT; break;
            case 'H': key = KEY_HOME; break;
            case 'F': key = KEY_END; break;
            case 'Z': key = KEY_BACKTAB; break;
            case 'P': key = KEY_F1; break;
            case 'Q': key = KEY_F1 + 1; break;
            case 'R': key = KEY_F1 + 2; break;
            case 'S': key = KEY_F1 + 3; break;
            case '~':
                switch (params[0]) {
                case 1: case 7: key = KEY_HOME; break;
                case 2: key = KEY_INSERT; break;
                case 3: key = KEY_DELETE; break;
                case 4: case 8: key = KEY_END; break;
                case 5: key = KEY_PGUP; break;
                case 6: key = KEY_PGDN; break;
                case 11: case 12: case 13: case 14: case 15: key = KEY_F1 + params[0] - 11; break;
                case 17: case 18: case 19: case 20: case 21: key = KEY_F1 + 5 + params[0] - 17; break;
                case 23: case 24: key = KEY_F1 + 10 + params[0] - 23; break;
                case 200: return -n; /* bracketed paste start */
                default: break;
                }
                if (np >= 2) mods = csi_mods(params[1]);
                break;
            case 'u':
                /* CSI u / fixterms: codepoint;mods */
                key = params[0];
                if (key <= 0 || key > 0x10FFFF || (key >= 0xD800 && key <= 0xDFFF)) {
                    ev->type = IN_NONE;
                    return n;
                }
                if (key == 13) key = KEY_ENTER;
                else if (key == 9) key = KEY_TAB;
                else if (key == 27) key = KEY_ESC;
                else if (key == 127) key = KEY_BACKSPACE;
                if (np >= 2) mods = csi_mods(params[1]);
                break;
            default:
                ev->type = IN_NONE;
                return n;
            }
            if (c != '~' && c != 'u' && np >= 2) mods = csi_mods(params[1]);
            if (c == 'Z') mods &= ~MOD_SHIFT;
            if (!key) { ev->type = IN_NONE; return n; }
            set_key(ev, key, mods, n);
            return n;
        } else if (c < 0x20 || c > 0x7E) {
            /* malformed */
            ev->type = IN_NONE;
            return i;
        }
    }
    return 0;
}

static int parse_plain(InEvent *ev, int off, int alt) {
    int c = inbuf[off];
    int key, mods = alt ? MOD_ALT : 0;
    if (c == 13) key = KEY_ENTER;
    else if (c == 9) key = KEY_TAB;
    else if (c == 127) key = KEY_BACKSPACE;
    else if (c == 27) key = KEY_ESC;
    else if (c == 0) { key = ' '; mods |= MOD_CTRL; }
    else if (c < 27) { key = 'a' + c - 1; mods |= MOD_CTRL; }
    else if (c < 32) {
        static const char map[] = "\\]^_";
        key = map[c - 28];
        mods |= MOD_CTRL;
    } else if (c < 128) key = c;
    else {
        int cp, n;
        n = utf8_decode((const char *)inbuf + off, inlen - off, &cp);
        if (n == 1 && (c & 0xE0) >= 0xC0) {
            /* possibly incomplete sequence */
            int need = (c & 0xF0) == 0xF0 ? 4 : (c & 0xE0) == 0xE0 ? 3 : 2;
            if (inlen - off < need) return 0;
        }
        set_key(ev, cp, mods, off + n);
        return off + n;
    }
    set_key(ev, key, mods, off + 1);
    return off + 1;
}

static int read_paste(InEvent *ev, int start) {
    Str s;
    long long deadline = plat_now_ms() + 2000;
    str_init(&s);
    str_reserve(&s, 0);
    consume(start);
    for (;;) {
        int i;
        for (i = 0; i + 5 < inlen; i++) {
            if (!memcmp(inbuf + i, "\x1b[201~", 6)) {
                str_putn(&s, (const char *)inbuf, i);
                consume(i + 6);
                goto done;
            }
        }
        /* keep the last 5 bytes in case the terminator is split */
        if (inlen > 5) {
            str_putn(&s, (const char *)inbuf, inlen - 5);
            consume(inlen - 5);
        }
        if (fill(100) <= 0 && plat_now_ms() > deadline) {
            str_putn(&s, (const char *)inbuf, inlen);
            consume(inlen);
            break;
        }
    }
done:
    /* normalize CRLF / CR to LF */
    {
        int r = 0, w = 0;
        for (r = 0; r < s.len; r++) {
            if (s.s[r] == '\r') {
                s.s[w++] = '\n';
                if (r + 1 < s.len && s.s[r + 1] == '\n') r++;
            } else s.s[w++] = s.s[r];
        }
        s.len = w;
        s.s[w] = 0;
    }
    ev->type = IN_PASTE;
    ev->pastelen = s.len;
    ev->paste = str_detach(&s);
    return 1;
}

int input_poll(InEvent *ev, int timeout_ms) {
    int n;
    memset(ev, 0, sizeof *ev);
    if (plat_resized()) {
        ev->type = IN_RESIZE;
        return 1;
    }
    if (inlen == 0) {
        if (fill(timeout_ms) <= 0) {
            if (plat_resized()) { ev->type = IN_RESIZE; return 1; }
            return 0;
        }
    }
    for (;;) {
        if (inbuf[0] == 27) {
            if (inlen == 1) {
                if (fill(25) <= 0) {
                    set_key(ev, KEY_ESC, 0, 1);
                    consume(1);
                    return 1;
                }
                continue;
            }
            if (inbuf[1] == '[') {
                n = parse_csi(ev);
                if (n == 0) {
                    if (fill(50) <= 0) { set_key(ev, '[', MOD_ALT, 2); consume(2); return 1; }
                    continue;
                }
                if (n < 0) return read_paste(ev, -n);
                consume(n);
                return ev->type != IN_NONE ? 1 : input_poll(ev, 0);
            }
            if (inbuf[1] == 'O') {
                int key = 0;
                if (inlen < 3) {
                    if (fill(25) <= 0) { set_key(ev, 'O', MOD_ALT, 2); consume(2); return 1; }
                    continue;
                }
                switch (inbuf[2]) {
                case 'A': key = KEY_UP; break;
                case 'B': key = KEY_DOWN; break;
                case 'C': key = KEY_RIGHT; break;
                case 'D': key = KEY_LEFT; break;
                case 'H': key = KEY_HOME; break;
                case 'F': key = KEY_END; break;
                case 'P': key = KEY_F1; break;
                case 'Q': key = KEY_F1 + 1; break;
                case 'R': key = KEY_F1 + 2; break;
                case 'S': key = KEY_F1 + 3; break;
                case 'M': key = KEY_ENTER; break;
                default: break;
                }
                if (key) { set_key(ev, key, 0, 3); consume(3); return 1; }
                set_key(ev, 'O', MOD_ALT, 2);
                consume(2);
                return 1;
            }
            if (inbuf[1] == 27) {
                /* ESC ESC: report Esc */
                set_key(ev, KEY_ESC, 0, 1);
                consume(1);
                return 1;
            }
            n = parse_plain(ev, 1, 1);
            if (n == 0) { if (fill(25) <= 0) { consume(1); continue; } continue; }
            consume(n);
            return 1;
        }
        n = parse_plain(ev, 0, 0);
        if (n == 0) {
            if (fill(25) <= 0) { set_key(ev, 0xFFFD, 0, 1); consume(1); return 1; }
            continue;
        }
        consume(n);
        return 1;
    }
}

void input_free(InEvent *ev) {
    free(ev->paste);
    ev->paste = NULL;
}

/* ---------------- key names ---------------- */

static const struct {
    const char *name;
    int key;
    int mods;
} keynames[] = {
    {"Up", KEY_UP, 0}, {"Down", KEY_DOWN, 0}, {"Right", KEY_RIGHT, 0}, {"Left", KEY_LEFT, 0},
    {"Home", KEY_HOME, 0}, {"End", KEY_END, 0}, {"PageUp", KEY_PGUP, 0}, {"PgUp", KEY_PGUP, 0},
    {"PageDown", KEY_PGDN, 0}, {"PgDn", KEY_PGDN, 0}, {"Insert", KEY_INSERT, 0},
    {"Delete", KEY_DELETE, 0}, {"Backspace", KEY_BACKSPACE, 0}, {"Backspace2", KEY_BACKSPACE, 0},
    {"OldBackspace", 'h', MOD_CTRL}, {"Tab", KEY_TAB, 0}, {"Backtab", KEY_BACKTAB, 0},
    {"Enter", KEY_ENTER, 0}, {"Esc", KEY_ESC, 0}, {"Escape", KEY_ESC, 0},
    {"Space", ' ', 0},
    {"CtrlSpace", ' ', MOD_CTRL}, {"LeftSq", '[', 0}, {"Backslash", '\\', 0},
    {"RightSq", ']', 0}, {"Carat", '^', 0}, {"Underscore", '_', 0},
    {"MouseLeft", KEY_MOUSE_LEFT, 0}, {"MouseMiddle", KEY_MOUSE_MIDDLE, 0},
    {"MouseRight", KEY_MOUSE_RIGHT, 0}, {"MouseLeftDrag", KEY_MOUSE_LEFT_DRAG, 0},
    {"MouseMiddleDrag", KEY_MOUSE_MIDDLE_DRAG, 0}, {"MouseRightDrag", KEY_MOUSE_RIGHT_DRAG, 0},
    {"MouseLeftRelease", KEY_MOUSE_LEFT_RELEASE, 0},
    {"MouseMiddleRelease", KEY_MOUSE_MIDDLE_RELEASE, 0},
    {"MouseRightRelease", KEY_MOUSE_RIGHT_RELEASE, 0}, {"MouseRelease", KEY_MOUSE_LEFT_RELEASE, 0},
    {"MouseWheelUp", KEY_WHEEL_UP, 0}, {"MouseWheelDown", KEY_WHEEL_DOWN, 0},
    {"MouseWheelLeft", KEY_WHEEL_LEFT, 0}, {"MouseWheelRight", KEY_WHEEL_RIGHT, 0},
};

int key_parse(const char *name, int *key, int *mods) {
    const char *k = name;
    int m = 0;
    size_t i;
    if (!*k) return 0;
    for (;;) {
        if (*k == '-' && k[1]) { k++; continue; }
        if (starts_with(k, "Ctrl") && strcmp(k, "CtrlH") != 0 && k[4]) { k += 4; m |= MOD_CTRL; continue; }
        if (starts_with(k, "Alt") && k[3]) { k += 3; m |= MOD_ALT; continue; }
        if (starts_with(k, "Shift") && k[5]) { k += 5; m |= MOD_SHIFT; continue; }
        break;
    }
    if (!strcmp(k, "CtrlH")) { *key = 'h'; *mods = m | MOD_CTRL; return 1; }
    for (i = 0; i < sizeof keynames / sizeof keynames[0]; i++) {
        if (!strcmp(k, keynames[i].name)) {
            *key = keynames[i].key;
            *mods = m | keynames[i].mods;
            if (*key == KEY_TAB && (*mods & MOD_SHIFT)) { *key = KEY_BACKTAB; *mods &= ~MOD_SHIFT; }
            return 1;
        }
    }
    if (k[0] == 'F' && isdigit((unsigned char)k[1])) {
        int n = atoi(k + 1);
        if (n >= 1 && n <= 64) { *key = KEY_F1 + n - 1; *mods = m; return 1; }
    }
    {
        int cp, n = utf8_decode(k, (int)strlen(k), &cp);
        if (n > 0 && k[n] == 0) {
            if (m & MOD_CTRL) {
                if (cp < 128) cp = tolower(cp);
                if (cp == '/') cp = '_'; /* terminals send Ctrl-/ as Ctrl-_ (0x1f) */
            }
            m &= ~MOD_SHIFT; /* shifted runes arrive as the shifted character */
            *key = cp;
            *mods = m;
            return 1;
        }
    }
    return 0;
}

char *key_name(int key, int mods) {
    Str s;
    size_t i;
    str_init(&s);
    str_reserve(&s, 0);
    if (key < KEY_SPECIAL) {
        char buf[8];
        if (key == ' ' && (mods & MOD_CTRL) && !(mods & ~MOD_CTRL)) return xstrdup("CtrlSpace");
        if (mods & MOD_CTRL) str_puts(&s, "Ctrl");
        if (mods & MOD_ALT) str_puts(&s, "Alt");
        if (mods & MOD_SHIFT) str_puts(&s, "Shift");
        if (mods) str_putc(&s, '-');
        buf[utf8_encode(key, buf)] = 0;
        str_puts(&s, buf);
        return str_detach(&s);
    }
    if (mods & MOD_CTRL) str_puts(&s, "Ctrl");
    if (mods & MOD_ALT) str_puts(&s, "Alt");
    if (mods & MOD_SHIFT) str_puts(&s, "Shift");
    if (key >= KEY_F1 && key < KEY_F1 + 64) {
        str_printf(&s, "F%d", key - KEY_F1 + 1);
        return str_detach(&s);
    }
    for (i = 0; i < sizeof keynames / sizeof keynames[0]; i++) {
        if (keynames[i].key == key && keynames[i].mods == 0) {
            if ((key >= KEY_MOUSE_FIRST) && mods) {
                /* micro writes mouse modifiers with a dash: Ctrl-MouseLeft */
                str_putc(&s, '-');
            }
            str_puts(&s, keynames[i].name);
            return str_detach(&s);
        }
    }
    str_puts(&s, "Unknown");
    return str_detach(&s);
}
