/*
 * tabs.c - tabs, each holding a binary tree of split panes.
 */
#include "micro.h"

#include <stdlib.h>

static Node *node_leaf(Pane *p) {
    Node *n = xcalloc(1, sizeof *n);
    n->kind = NODE_LEAF;
    n->pane = p;
    n->ratio = 0.5;
    return n;
}

Tab *tab_new(Pane *p) {
    Tab *t = xcalloc(1, sizeof *t);
    t->root = node_leaf(p);
    t->active = p;
    p->tab = t;
    return t;
}

static void node_free(Node *n) {
    if (!n) return;
    node_free(n->a);
    node_free(n->b);
    if (n->kind == NODE_LEAF) pane_free(n->pane);
    free(n);
}

void tab_free(Tab *t) {
    node_free(t->root);
    free(t);
}

static void node_layout(Node *n, int x, int y, int w, int h) {
    n->x = x;
    n->y = y;
    n->w = w;
    n->h = h;
    if (n->kind == NODE_LEAF) {
        n->pane->x = x;
        n->pane->y = y;
        n->pane->w = w;
        n->pane->h = h;
        return;
    }
    if (n->kind == NODE_VSPLIT) {
        /* side by side with a 1-column divider */
        int aw = (int)((w - 1) * n->ratio + 0.5);
        if (aw < 1) aw = 1;
        if (aw > w - 2) aw = w - 2;
        if (aw < 1) aw = 1;
        node_layout(n->a, x, y, aw, h);
        node_layout(n->b, x + aw + 1, y, w - aw - 1 > 0 ? w - aw - 1 : 1, h);
    } else {
        int ah = (int)(h * n->ratio + 0.5);
        if (ah < 1) ah = 1;
        if (ah > h - 1) ah = h - 1;
        if (ah < 1) ah = 1;
        node_layout(n->a, x, y, w, ah);
        node_layout(n->b, x, y + ah, w, h - ah > 0 ? h - ah : 1);
    }
}

void tab_layout(Tab *t, int x, int y, int w, int h) { node_layout(t->root, x, y, w, h); }

Node *tab_find_node(Node *n, Pane *p) {
    Node *r;
    if (!n) return NULL;
    if (n->kind == NODE_LEAF) return n->pane == p ? n : NULL;
    if ((r = tab_find_node(n->a, p))) return r;
    return tab_find_node(n->b, p);
}

Pane *tab_split(Tab *t, Pane *p, Buffer *b, int vertical) {
    Node *n = tab_find_node(t->root, p);
    Pane *np;
    Node *old, *nw;
    int after = vertical ? BOPT(p->buf, O_SPLITRIGHT) : BOPT(p->buf, O_SPLITBOTTOM);
    if (!n) return NULL;
    np = pane_new(b);
    np->tab = t;
    old = node_leaf(p);
    nw = node_leaf(np);
    n->kind = vertical ? NODE_VSPLIT : NODE_HSPLIT;
    n->pane = NULL;
    n->ratio = 0.5;
    n->a = after ? old : nw;
    n->b = after ? nw : old;
    old->parent = n;
    nw->parent = n;
    t->active = np;
    return np;
}

void tab_close_pane(Tab *t, Pane *p) {
    Node *n = tab_find_node(t->root, p), *parent, *sib;
    if (!n) return;
    parent = n->parent;
    if (!parent) {
        /* last pane in tab */
        node_free(n);
        t->root = NULL;
        t->active = NULL;
        return;
    }
    sib = parent->a == n ? parent->b : parent->a;
    /* replace parent with sibling */
    parent->kind = sib->kind;
    parent->pane = sib->pane;
    parent->a = sib->a;
    parent->b = sib->b;
    parent->ratio = sib->ratio;
    if (parent->a) parent->a->parent = parent;
    if (parent->b) parent->b->parent = parent;
    free(sib);
    node_free(n);
    if (t->active == p) {
        Node *f = parent;
        while (f->kind != NODE_LEAF) f = f->a;
        t->active = f->pane;
    }
}

static void collect(Node *n, Pane ***arr, int *cnt, int *cap) {
    if (!n) return;
    if (n->kind == NODE_LEAF) {
        if (*cnt == *cap) {
            *cap = *cap ? *cap * 2 : 8;
            *arr = xrealloc(*arr, sizeof **arr * (size_t)*cap);
        }
        (*arr)[(*cnt)++] = n->pane;
        return;
    }
    collect(n->a, arr, cnt, cap);
    collect(n->b, arr, cnt, cap);
}

Pane **tab_panes(Tab *t, int *n) {
    Pane **arr = NULL;
    int cnt = 0, cap = 0;
    collect(t->root, &arr, &cnt, &cap);
    *n = cnt;
    return arr;
}

int tab_npanes(Tab *t) {
    int n;
    Pane **a = tab_panes(t, &n);
    free(a);
    return n;
}

Pane *tab_pane_at(Tab *t, int x, int y) {
    int n, i;
    Pane **a = tab_panes(t, &n), *r = NULL;
    for (i = 0; i < n; i++) {
        Pane *p = a[i];
        if (x >= p->x && x < p->x + p->w && y >= p->y && y < p->y + p->h) { r = p; break; }
    }
    free(a);
    return r;
}
