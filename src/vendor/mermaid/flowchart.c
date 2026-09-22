#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "canvas.h"

MermaidArt *layout_flowchart(const Graph *g, int max_width, Oversize *err) {
    size_t n = g->n_nodes;
    NodeExtra *extras = calloc(n ? n : 1, sizeof(NodeExtra));
    for (size_t i = 0; i < n; i++) extras[i].kind = EXTRA_PLAIN;
    Canvas *canvas = layout_canvas(g, extras, max_width, err);
    free(extras);
    if (!canvas) return NULL;
    if (g->dir == DIR_UP) canvas_flip_vertical(canvas);
    else if (g->dir == DIR_LEFT) canvas_flip_horizontal(canvas);
    MermaidArt *art = mermaid_art_from_canvas(canvas);
    canvas_free(canvas);
    return art;
}

typedef struct { bool is_group; int idx; } Item;

static Item group_item(int gi) { return (Item){ true, gi }; }
static Item node_item(int ni) { return (Item){ false, ni }; }
static bool item_eq(Item a, Item b) { return a.is_group == b.is_group && a.idx == b.idx; }

static void group_chain(const Graph *g, int gopt, IntVec *out) {
    intvec_init(out);
    int cur = gopt;
    while (cur >= 0) {
        intvec_push(out, cur);
        cur = g->groups[cur].parent;
    }
    for (size_t a = 0, b = out->n - (out->n ? 1 : 0); a < b; a++, b--) {
        int t = out->data[a]; out->data[a] = out->data[b]; out->data[b] = t;
    }
}

static void endpoint(const Graph *g, const int *proxy, int n, Item *item, IntVec *chain) {
    if (proxy[n] >= 0) {
        *item = group_item(proxy[n]);
        group_chain(g, g->groups[proxy[n]].parent, chain);
    } else {
        *item = node_item(n);
        group_chain(g, g->node_group[n], chain);
    }
}

typedef struct { Item f, t; size_t ei; int scope; } ScopeEdge;

static Canvas *build_scope(const Graph *g, int scope, ScopeEdge *sedges, size_t n_sedges,
                            const int *node_group, const int *proxy, const bool *keep, int max_width, Oversize *err) {
    IntVec items_is_group, items_idx;
    intvec_init(&items_is_group);
    intvec_init(&items_idx);

    for (size_t ni = 0; ni < g->n_nodes; ni++) {
        if (proxy[ni] >= 0) continue;
        if (node_group[ni] == scope) { intvec_push(&items_is_group, 0); intvec_push(&items_idx, (int)ni); }
    }
    for (size_t gi = 0; gi < g->n_groups; gi++) {
        if (g->groups[gi].parent == scope && keep[gi]) { intvec_push(&items_is_group, 1); intvec_push(&items_idx, (int)gi); }
    }

    if (items_idx.n == 0) {
        intvec_free(&items_is_group); intvec_free(&items_idx);
        *err = OVERSIZE_NONE;
        return canvas_new(1, 1);
    }

    Graph *synth = graph_new();
    synth->dir = g->dir;
    NodeExtra *extras = calloc(items_idx.n, sizeof(NodeExtra));
    Canvas **subframes = calloc(items_idx.n, sizeof(Canvas *));

    for (size_t k = 0; k < items_idx.n; k++) {
        if (items_is_group.data[k] == 0) {
            int ni = items_idx.data[k];
            graph_node_index(synth, g->ids[ni], g->nodes[ni].label, g->nodes[ni].shape);
            extras[k].kind = EXTRA_PLAIN;
        } else {
            int gi = items_idx.data[k];
            Oversize suberr;
            Canvas *sub = build_scope(g, gi, sedges, n_sedges, node_group, proxy, keep, -1, &suberr);
            subframes[k] = sub;
            char idbuf[32]; snprintf(idbuf, sizeof(idbuf), "\x01grp%d", gi);
            graph_node_index(synth, idbuf, g->groups[gi].label, SHAPE_RECT);
            extras[k].kind = EXTRA_FRAME;
            extras[k].frame = sub;
        }
    }

    for (size_t ei = 0; ei < n_sedges; ei++) {
        if (sedges[ei].scope != scope) continue;
        long fi = -1, ti = -1;
        for (size_t k = 0; k < items_idx.n; k++) {
            Item cand = items_is_group.data[k] ? group_item(items_idx.data[k]) : node_item(items_idx.data[k]);
            if (item_eq(cand, sedges[ei].f)) fi = (long)k;
            if (item_eq(cand, sedges[ei].t)) ti = (long)k;
        }
        if (fi < 0 || ti < 0) continue;
        const Edge *e = &g->edges[sedges[ei].ei];
        graph_push_edge(synth, (int)fi, (int)ti, e->label, e->head_to, e->head_from, e->line);
    }

    intvec_free(&items_is_group); intvec_free(&items_idx);

    Canvas *canvas = layout_canvas(synth, extras, max_width, err);
    for (size_t k = 0; k < synth->n_nodes; k++) if (subframes[k]) canvas_free(subframes[k]);
    free(subframes);
    free(extras);
    graph_free(synth);
    return canvas;
}

MermaidArt *render_grouped(const Graph *g, int max_width, Oversize *err) {
    int *proxy = malloc(sizeof(int) * (g->n_nodes ? g->n_nodes : 1));
    for (size_t i = 0; i < g->n_nodes; i++) proxy[i] = -1;
    for (size_t gi = 0; gi < g->n_groups; gi++) {
        int ni = graph_index_of(g, g->groups[gi].id);
        if (ni >= 0) proxy[ni] = (int)gi;
    }

    ScopeEdge *sedges = malloc(sizeof(ScopeEdge) * (g->n_edges ? g->n_edges : 1));
    bool *referenced = calloc(g->n_groups ? g->n_groups : 1, sizeof(bool));
    for (size_t ei = 0; ei < g->n_edges; ei++) {
        const Edge *e = &g->edges[ei];
        Item item_f, item_t;
        IntVec chain_f, chain_t;
        endpoint(g, proxy, e->from, &item_f, &chain_f);
        endpoint(g, proxy, e->to, &item_t, &chain_t);
        size_t k = 0;
        while (k < chain_f.n && k < chain_t.n && chain_f.data[k] == chain_t.data[k]) k++;
        int scope = k == 0 ? -1 : chain_f.data[k - 1];
        Item f = chain_f.n > k ? group_item(chain_f.data[k]) : item_f;
        Item t = chain_t.n > k ? group_item(chain_t.data[k]) : item_t;
        if (f.is_group) referenced[f.idx] = true;
        if (t.is_group) referenced[t.idx] = true;
        sedges[ei] = (ScopeEdge){ f, t, ei, scope };
        intvec_free(&chain_f); intvec_free(&chain_t);
    }

    bool *keep = calloc(g->n_groups ? g->n_groups : 1, sizeof(bool));
    for (long gi = (long)g->n_groups - 1; gi >= 0; gi--) {
        bool has_nodes = false;
        for (size_t ni = 0; ni < g->n_nodes; ni++)
            if (proxy[ni] < 0 && g->node_group[ni] == gi) { has_nodes = true; break; }
        bool has_children = false;
        for (size_t c = 0; c < g->n_groups; c++)
            if (g->groups[c].parent == gi && keep[c]) { has_children = true; break; }
        keep[gi] = has_nodes || has_children || referenced[gi];
    }

    Canvas *canvas = build_scope(g, -1, sedges, g->n_edges, g->node_group, proxy, keep, max_width, err);
    free(proxy); free(sedges); free(referenced); free(keep);
    if (!canvas) return NULL;

    if (g->dir == DIR_UP) canvas_flip_vertical(canvas);
    else if (g->dir == DIR_LEFT) canvas_flip_horizontal(canvas);
    MermaidArt *art = mermaid_art_from_canvas(canvas);
    canvas_free(canvas);
    return art;
}
