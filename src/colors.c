#include "colors.h"
#include "runtime.h"
#include "util.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

Style def_style = {COLOR_DEFAULT, COLOR_DEFAULT, 0};

typedef struct {
    char *name;
    Style st;
} Link;

static Link *links;
static int nlinks, clinks;

int style_eq(Style a, Style b) { return a.fg == b.fg && a.bg == b.bg && a.attr == b.attr; }

Style style_reverse(Style s) {
    s.attr ^= ATTR_REVERSE;
    return s;
}

static int named_color(const char *s, int *out) {
    static const char *names[] = {"black", "red", "green", "yellow", "blue", "magenta", "cyan", "white"};
    int i;
    const char *base = s;
    int bright = 0;
    if (starts_with(s, "bright")) { bright = 1; base = s + 6; }
    else if (starts_with(s, "light")) { bright = 1; base = s + 5; }
    for (i = 0; i < 8; i++) {
        if (!strcmp(base, names[i])) { *out = i + (bright ? 8 : 0); return 1; }
    }
    return 0;
}

static int string_to_color(const char *s, int *out) {
    int n;
    if (!*s || !strcmp(s, "default")) { *out = COLOR_DEFAULT; return 1; }
    if (named_color(s, out)) return 1;
    if (isdigit((unsigned char)s[0])) {
        char *end;
        n = (int)strtol(s, &end, 10);
        if (*end == 0) {
            *out = n == 0 ? COLOR_DEFAULT : (n & 255);
            return 1;
        }
    }
    if (s[0] == '#' && strlen(s) == 7) {
        long v = strtol(s + 1, NULL, 16);
        *out = (int)((1L << 24) | v);
        return 1;
    }
    return 0;
}

static Style string_to_style(const char *str) {
    const char *last = strrchr(str, ' ');
    char fg[64] = "", bg[64] = "";
    const char *spec = last ? last + 1 : str;
    const char *comma = strchr(spec, ',');
    Style st = def_style;
    int c;
    if (comma) {
        size_t l = (size_t)(comma - spec);
        if (l >= sizeof fg) l = sizeof fg - 1;
        memcpy(fg, spec, l);
        fg[l] = 0;
        strncpy(bg, comma + 1, sizeof bg - 1);
    } else {
        strncpy(fg, spec, sizeof fg - 1);
    }
    /* trim */
    {
        char *p;
        for (p = fg + strlen(fg); p > fg && isspace((unsigned char)p[-1]); ) *--p = 0;
        for (p = bg + strlen(bg); p > bg && isspace((unsigned char)p[-1]); ) *--p = 0;
    }
    if (fg[0] && strcmp(fg, "default") && string_to_color(fg, &c)) st.fg = c;
    if (bg[0] && strcmp(bg, "default") && string_to_color(bg, &c)) st.bg = c;
    st.attr = 0;
    if (strstr(str, "bold")) st.attr |= ATTR_BOLD;
    if (strstr(str, "blink")) st.attr |= ATTR_BLINK;
    if (strstr(str, "dim")) st.attr |= ATTR_DIM;
    if (strstr(str, "italic")) st.attr |= ATTR_ITALIC;
    if (strstr(str, "reverse")) st.attr |= ATTR_REVERSE;
    if (strstr(str, "strikethrough")) st.attr |= ATTR_STRIKE;
    if (strstr(str, "underline")) st.attr |= ATTR_UNDERLINE;
    return st;
}

static void set_link(const char *name, Style st) {
    int i;
    for (i = 0; i < nlinks; i++)
        if (!strcmp(links[i].name, name)) { links[i].st = st; return; }
    if (nlinks == clinks) {
        clinks = clinks ? clinks * 2 : 64;
        links = xrealloc(links, sizeof *links * (size_t)clinks);
    }
    links[nlinks].name = xstrdup(name);
    links[nlinks].st = st;
    nlinks++;
}

static int parse_scheme(const char *name, char **seen, int nseen, char **err) {
    char *text = runtime_read(RT_COLORSCHEME, name);
    char *line, *save;
    int i;
    if (!text) {
        if (err && !*err) *err = xsprintf("%s is not a valid colorscheme", name);
        return -1;
    }
    for (i = 0; i < nseen; i++)
        if (!strcmp(seen[i], name)) { free(text); return 0; } /* include cycle */
    seen[nseen++] = (char *)name;
    for (line = text; line && *line; line = save) {
        char *e = strchr(line, '\n');
        char *p = line;
        save = e ? e + 1 : NULL;
        if (e) *e = 0;
        while (isspace((unsigned char)*p)) p++;
        if (!*p || *p == '#') continue;
        if (starts_with(p, "include")) {
            char *q1 = strchr(p, '"'), *q2;
            if (q1 && (q2 = strchr(q1 + 1, '"')) && nseen < 16) {
                *q2 = 0;
                parse_scheme(q1 + 1, seen, nseen, err);
            }
            continue;
        }
        if (starts_with(p, "color-link")) {
            char *nm, *q1, *q2;
            p += 10;
            while (isspace((unsigned char)*p)) p++;
            nm = p;
            while (*p && !isspace((unsigned char)*p)) p++;
            if (!*p) continue;
            *p++ = 0;
            q1 = strchr(p, '"');
            q2 = q1 ? strrchr(q1 + 1, '"') : NULL;
            if (!q1 || !q2) continue;
            *q2 = 0;
            {
                Style st = string_to_style(q1 + 1);
                set_link(nm, st);
                if (!strcmp(nm, "default")) def_style = st;
            }
        }
    }
    free(text);
    return 0;
}

int colorscheme_load(const char *name, char **err) {
    char *seen[16];
    int i;
    char *probe = runtime_read(RT_COLORSCHEME, name);
    if (err) *err = NULL;
    if (!probe) {
        if (err) *err = xsprintf("%s is not a valid colorscheme", name);
        return -1;
    }
    free(probe);
    for (i = 0; i < nlinks; i++) free(links[i].name);
    nlinks = 0;
    def_style.fg = def_style.bg = COLOR_DEFAULT;
    def_style.attr = 0;
    return parse_scheme(name, seen, 0, err);
}

int color_has(const char *group) {
    int i;
    for (i = 0; i < nlinks; i++)
        if (!strcmp(links[i].name, group)) return 1;
    return 0;
}

Style color_get(const char *group) {
    char buf[128];
    int i;
    strncpy(buf, group, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (;;) {
        char *dot;
        for (i = 0; i < nlinks; i++)
            if (!strcmp(links[i].name, buf)) return links[i].st;
        dot = strrchr(buf, '.');
        if (!dot) break;
        *dot = 0;
    }
    return def_style;
}

char **colorscheme_list(void) { return runtime_list(RT_COLORSCHEME); }
