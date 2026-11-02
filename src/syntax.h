#ifndef SYNTAX_H
#define SYNTAX_H

#include "colors.h"
#include "regex.h"

typedef struct Region Region;

/* syntax.c-private search prefilter derived from a rule's regex source
   (NULL: none).  Shared, like the Regex, between copies of included rules. */
typedef struct ReFilter ReFilter;

typedef struct {
    int group;
    Regex *re;
    ReFilter *flt;
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
    ReFilter *start_flt, *end_flt, *skip_flt;
};

typedef struct Syntax {
    char *filetype;
    char *file;       /* runtime file name */
    Regex *fname, *header, *signature;
    Rules rules;
    int loaded;       /* rules parsed */
    int failed;
} Syntax;

void        syntax_init(void);                   /* index all syntax files */
Syntax     *syntax_detect(const char *path, const char *firstline,
                          const char **lines, int nlines, int detectlimit);
Syntax     *syntax_by_filetype(const char *ft);
int         syntax_load(Syntax *s, char **err);  /* parse rules lazily */
char      **syntax_filetypes(void);              /* malloc'd list */

int         group_id(const char *name);
const char *group_name(int id);
int         group_count(void);
void        group_styles_refresh(void);          /* after colorscheme change */
Style       group_style(int id);

/* Highlight one line.  `state` is the region open at the start of the line
 * (NULL = none).  If `hl` is non-NULL it receives one group id per byte.
 * Returns the region open at the end of the line. */
Region *syntax_highlight_line(Syntax *s, Region *state, const char *line, int len,
                              unsigned short *hl);

#endif
