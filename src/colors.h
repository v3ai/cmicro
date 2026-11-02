#ifndef COLORS_H
#define COLORS_H

#define COLOR_DEFAULT (-1)
#define COLOR_RGB(r, g, b) ((1 << 24) | ((r) << 16) | ((g) << 8) | (b))
#define COLOR_IS_RGB(c) ((c) >= (1 << 24))

enum {
    ATTR_BOLD = 1,
    ATTR_ITALIC = 2,
    ATTR_UNDERLINE = 4,
    ATTR_REVERSE = 8,
    ATTR_DIM = 16,
    ATTR_BLINK = 32,
    ATTR_STRIKE = 64
};

typedef struct {
    int fg, bg;
    unsigned attr;
} Style;

extern Style def_style;

int   style_eq(Style a, Style b);
Style style_reverse(Style s);

/* colorscheme */
int   colorscheme_load(const char *name, char **err); /* 0 on success */
Style color_get(const char *group);         /* with "a.b.c" -> "a.b" -> "a" fallback */
int   color_has(const char *group);          /* exact link exists */
char **colorscheme_list(void);              /* malloc'd, NULL-terminated */

#endif
