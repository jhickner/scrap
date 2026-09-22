#include <stdlib.h>
#include <string.h>

#include "internal.h"
#include "strutil.h"

Graph *graph_new(void) {
    Graph *g = calloc(1, sizeof(Graph));
    g->cur_group = -1;
    g->dir = DIR_DOWN;
    return g;
}

void graph_free(Graph *g) {
    if (!g) return;
    for (size_t i = 0; i < g->n_nodes; i++) { free(g->ids[i]); free(g->nodes[i].label); }
    free(g->ids); free(g->nodes);
    for (size_t i = 0; i < g->n_edges; i++) free(g->edges[i].label);
    free(g->edges);
    for (size_t i = 0; i < g->n_groups; i++) { free(g->groups[i].id); free(g->groups[i].label); }
    free(g->groups);
    free(g->node_group);
    free(g);
}

int graph_index_of(const Graph *g, const char *id) {
    for (size_t i = 0; i < g->n_nodes; i++)
        if (strcmp(g->ids[i], id) == 0) return (int)i;
    return -1;
}

static void nodes_grow(Graph *g) {
    if (g->n_nodes == g->cap_nodes) {
        g->cap_nodes = g->cap_nodes ? g->cap_nodes * 2 : 16;
        g->ids = realloc(g->ids, g->cap_nodes * sizeof(char *));
        g->nodes = realloc(g->nodes, g->cap_nodes * sizeof(Node));
        g->node_group = realloc(g->node_group, g->cap_nodes * sizeof(int));
    }
}

int graph_node_index(Graph *g, const char *id, const char *label, Shape shape) {
    int i = graph_index_of(g, id);
    if (i >= 0) {
        if (label) {
            free(g->nodes[i].label);
            g->nodes[i].label = cstr_dup(label);
            g->nodes[i].shape = shape;
        }
        return i;
    }
    if (g->n_nodes >= MAX_NODES) { g->over_cap = true; return -1; }
    nodes_grow(g);
    g->ids[g->n_nodes] = cstr_dup(id);
    g->nodes[g->n_nodes].label = cstr_dup(label ? label : id);
    g->nodes[g->n_nodes].shape = shape;
    g->node_group[g->n_nodes] = g->cur_group;
    g->n_nodes++;
    return (int)(g->n_nodes - 1);
}

int graph_node_label(Graph *g, const char *id, const char *label) {
    int i = graph_index_of(g, id);
    if (i >= 0) {
        free(g->nodes[i].label);
        g->nodes[i].label = cstr_dup(label);
        return i;
    }
    return graph_node_index(g, id, label, SHAPE_ROUND);
}

void graph_push_edge(Graph *g, int from, int to, const char *label, Head head_to, Head head_from, LineKind line) {
    if (g->n_edges == g->cap_edges) {
        g->cap_edges = g->cap_edges ? g->cap_edges * 2 : 16;
        g->edges = realloc(g->edges, g->cap_edges * sizeof(Edge));
    }
    g->edges[g->n_edges].from = from;
    g->edges[g->n_edges].to = to;
    g->edges[g->n_edges].label = label ? cstr_dup(label) : NULL;
    g->edges[g->n_edges].head_to = head_to;
    g->edges[g->n_edges].head_from = head_from;
    g->edges[g->n_edges].line = line;
    g->n_edges++;
}

int graph_push_group(Graph *g, const char *id, const char *label, int parent) {
    if (g->n_groups == g->cap_groups) {
        g->cap_groups = g->cap_groups ? g->cap_groups * 2 : 8;
        g->groups = realloc(g->groups, g->cap_groups * sizeof(Group));
    }
    g->groups[g->n_groups].id = cstr_dup(id);
    g->groups[g->n_groups].label = cstr_dup(label);
    g->groups[g->n_groups].parent = parent;
    g->n_groups++;
    return (int)(g->n_groups - 1);
}
