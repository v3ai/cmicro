#ifndef COLORS_H
#define COLORS_H

#define COLOR_DEFAULT (-1)
/* 24-bit colors are stored as 0x1RRGGBB; r, g and b must be 0..255 */
#define COLOR_RGB(r, g, b) \
    ((int)(0x1000000UL | ((unsigned long)(r) << 16) | ((unsigned long)(g) << 8) | (unsigned long)(b)))
#define COLOR_IS_RGB(c) ((c) >= 0x1000000L)

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

extern Style cm_def_style;

int   cm_style_eq(Style a, Style b);
Style cm_style_reverse(Style s);

/* colorscheme */
int   cm_colorscheme_load(const char *name, char **err); /* 0 on success */
Style cm_color_get(const char *group);         /* with "a.b.c" -> "a.b" -> "a" fallback */
int   cm_color_has(const char *group);          /* exact link exists */
char **cm_colorscheme_list(void);              /* malloc'd, NULL-terminated */

#endif
