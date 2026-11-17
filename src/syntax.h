#ifndef SYNTAX_H
#define SYNTAX_H

#include "colors.h"
#include "regex.h"

typedef struct Region Region;

typedef struct {
    int group;
    Regex *re;
} Pattern;

typedef struct {
    Pattern *pats;
    int npats;
    Region **regions;
    int nregions;
} Rules;

struct Region {
    int group, limit_group;
    Regex *start, *end, *skip;
    Region *parent;
    Rules rules;
};

typedef struct Syntax {
    char *filetype;
    char *file;       /* runtime file name */
    Regex *fname, *header, *signature;
    Rules rules;
    int loaded;       /* rules parsed */
    int failed;
} Syntax;

void        cm_syntax_init(void);                   /* index all syntax files */
Syntax     *cm_syntax_detect(const char *path, const char *firstline,
                          const char **lines, int nlines, int detectlimit);
Syntax     *cm_syntax_by_filetype(const char *ft);
int         cm_syntax_load(Syntax *s, char **err);  /* parse rules lazily */
char      **cm_syntax_filetypes(void);              /* malloc'd list */

int         cm_group_id(const char *name);
const char *cm_group_name(int id);
int         cm_group_count(void);
void        cm_group_styles_refresh(void);          /* after colorscheme change */
Style       cm_group_style(int id);

/* Highlight one line.  `state` is the region open at the start of the line
 * (NULL = none).  If `hl` is non-NULL it receives one group id per byte.
 * Returns the region open at the end of the line. */
Region *cm_syntax_highlight_line(Syntax *s, Region *state, const char *line, int len,
                              unsigned short *hl);

#endif
