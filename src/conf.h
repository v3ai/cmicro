/*
 * conf.h - tiny tree parsers for the config formats micro uses:
 *   - the YAML subset used by syntax definition files
 *   - JSON (lenient, json5-ish: comments + trailing commas) for
 *     settings.json and bindings.json
 */
#ifndef CONF_H
#define CONF_H

enum { CN_NULL, CN_STR, CN_NUM, CN_BOOL, CN_LIST, CN_MAP };

typedef struct CNode {
    int type;
    char *str;           /* CN_STR (and the raw text for NUM/BOOL) */
    double num;          /* CN_NUM */
    int b;               /* CN_BOOL */
    char **keys;         /* CN_MAP */
    struct CNode **vals; /* CN_MAP values / CN_LIST items */
    int n, cap;
} CNode;

CNode *cm_yaml_parse(const char *text, char **err);
CNode *cm_json_parse(const char *text, char **err);
void   cm_cnode_free(CNode *n);
CNode *cm_cnode_get(const CNode *map, const char *key);
const char *cm_cnode_str(const CNode *map, const char *key); /* string value or NULL */

CNode *cm_cnode_new(int type);
void   cm_cnode_set(CNode *map, const char *key, CNode *val); /* takes ownership of val */
void   cm_cnode_del(CNode *map, const char *key);
int    cm_json_write_file(const char *path, const CNode *n);

#endif
