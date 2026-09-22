#ifndef MERMAID_INTERNAL_H
#define MERMAID_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include "mermaid.h"
#include "strutil.h"

#define MAX_LABEL 28
#define PAD 1
#define GAP_X 3
#define GAP_Y 2
#define WRAP_WIDTH 24
#define MAX_LINES 4
#define MAX_NODES 128
#define MAX_EDGES 512
#define MAX_GROUPS 24
#define MAX_GROUP_DEPTH 6
#define MAX_CANVAS_CELLS (1u << 21)
#define MAX_MEMBERS 8
#define CONT 0u

extern const char LABEL_BREAK_CHARS[4];

typedef enum { SHAPE_RECT, SHAPE_ROUND, SHAPE_DIAMOND } Shape;
typedef enum { HEAD_NONE, HEAD_ARROW, HEAD_CIRCLE, HEAD_CROSS, HEAD_TRIANGLE, HEAD_DIAMOND_FILL, HEAD_DIAMOND_OPEN } Head;
typedef enum { LINE_SOLID, LINE_DOTTED, LINE_THICK } LineKind;
typedef enum { DIR_DOWN, DIR_UP, DIR_RIGHT, DIR_LEFT } Dir;
typedef enum { OVERSIZE_NONE, OVERSIZE_WIDTH, OVERSIZE_CELLS } Oversize;

typedef struct {
    char *label;
    Shape shape;
} Node;

typedef struct {
    int from, to;
    char *label;
    Head head_to, head_from;
    LineKind line;
} Edge;

typedef struct {
    char *id;
    char *label;
    int parent;
} Group;

typedef struct {
    char **ids;
    Node *nodes; size_t n_nodes, cap_nodes;
    Edge *edges; size_t n_edges, cap_edges;
    Group *groups; size_t n_groups, cap_groups;
    int *node_group;
    int cur_group;
    bool over_cap;
    Dir dir;
} Graph;

typedef struct {
    char *annotation;
    char **attrs; size_t n_attrs, cap_attrs;
    char **methods; size_t n_methods, cap_methods;
} ClassInfo;

Graph *graph_new(void);
void graph_free(Graph *g);
int graph_index_of(const Graph *g, const char *id);
int graph_node_index(Graph *g, const char *id, const char *label, Shape shape);
int graph_node_label(Graph *g, const char *id, const char *label);
void graph_push_edge(Graph *g, int from, int to, const char *label, Head head_to, Head head_from, LineKind line);
int graph_push_group(Graph *g, const char *id, const char *label, int parent);

void collect_statements(const char *src, StrVec *out);

Graph *parse_graph(const char *src);
Graph *parse_state(const char *src);
Graph *parse_class(const char *src, ClassInfo **infos_out, size_t *n_infos_out);
Graph *parse_er(const char *src, ClassInfo **infos_out, size_t *n_infos_out);
char *display_generics(const char *s);
void push_member(ClassInfo *ci, const char *raw);
void push_er_attribute(ClassInfo *ci, const char *raw);
bool parse_er_op(const char *tok, const char **card_l, const char **card_r, LineKind *line);
void class_infos_free(ClassInfo *infos, size_t n);

bool is_id_char(char c);
char *clean_label(const char *raw);
char *decode_html_entities(const char *s);
char *strip_html_tags(const char *s);
char *strip_markdown(const char *s);

char **wrap_label(const char *label, size_t width, size_t max_lines, size_t *n_out);
char *fit_label(const char *label, size_t inner);

size_t *compute_ranks(const Graph *g);
void order_ranks(size_t **by_rank, size_t *by_rank_n, size_t n_ranks, const Graph *g, const size_t *ranks);
size_t count_crossings(const Graph *g, const size_t *ranks, const size_t *pos);
size_t *assign_positions(size_t **by_rank, const size_t *by_rank_n, size_t n_ranks, const size_t *size, size_t n,
                          size_t sep, const Graph *g, const size_t *ranks);

typedef struct Sequence Sequence;
Sequence *parse_sequence(const char *src);
void sequence_free(Sequence *s);

MermaidArt *layout_flowchart(const Graph *g, int max_width, Oversize *err);
MermaidArt *render_grouped(const Graph *g, int max_width, Oversize *err);
MermaidArt *render_class(const Graph *g, const ClassInfo *infos, size_t n_infos, int max_width, Oversize *err);
MermaidArt *layout_sequence(const Sequence *seq, int max_width, Oversize *err);
MermaidArt *fallback(const char *src, int max_width, bool too_wide);

#endif
