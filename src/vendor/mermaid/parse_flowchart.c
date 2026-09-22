#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "dwidth.h"
#include "internal.h"
#include "strutil.h"

static bool cp_is_id(uint32_t c) {
    return c < 128 ? (isalnum((int)c) || c == '_') : true;
}
static bool cp_is_space(uint32_t c) { return c == ' ' || c == '\t'; }
static bool cp_is_link_char(uint32_t c) { return c == '-' || c == '.' || c == '=' || c == '<' || c == '>'; }

static char *cps_dup(const uint32_t *cps, size_t start, size_t end) {
    return utf8_encode(cps + start, end - start);
}

static size_t skip_spaces(const uint32_t *cps, size_t n, size_t i) {
    while (i < n && cp_is_space(cps[i])) i++;
    return i;
}

typedef struct { Shape shape; char *label; bool has_label; size_t after; } ShapeResult;

static ShapeResult read_shape(const uint32_t *cps, size_t n, size_t start, const char *closer, Shape shape) {
    uint32_t clc[4]; size_t cl = 0;
    for (const char *p = closer; *p; p++) clc[cl++] = (uint32_t)(unsigned char)*p;
    size_t i = start;
    bool quoted;
    {
        size_t j = start;
        while (j < n && cp_is_space(cps[j])) j++;
        quoted = j < n && cps[j] == '"';
    }
    bool in_quotes = false;
    Buf text; buf_init(&text);
    while (i < n) {
        uint32_t c = cps[i];
        if (quoted && c == '"') {
            in_quotes = !in_quotes;
            char tmp[4]; size_t tl = utf8_encode_cp(c, tmp);
            for (size_t t = 0; t < tl; t++) buf_push_char(&text, tmp[t]);
            i++;
            continue;
        }
        if (!in_quotes && i + cl <= n) {
            bool match = true;
            for (size_t k = 0; k < cl; k++) if (cps[i + k] != clc[k]) { match = false; break; }
            if (match) {
                char *raw = buf_take(&text);
                char *label = clean_label(raw);
                free(raw);
                ShapeResult r = { shape, label, true, i + cl };
                return r;
            }
        }
        char tmp[4]; size_t tl = utf8_encode_cp(c, tmp);
        for (size_t t = 0; t < tl; t++) buf_push_char(&text, tmp[t]);
        i++;
    }
    char *raw = buf_take(&text);
    char *label = clean_label(raw);
    free(raw);
    ShapeResult r = { shape, label, true, n };
    return r;
}

static bool parse_node(const uint32_t *cps, size_t n, size_t start, Graph *g, int *idx_out, size_t *after_out) {
    size_t i = skip_spaces(cps, n, start);
    size_t id_start = i;
    while (i < n && cp_is_id(cps[i])) i++;
    if (i == id_start) return false;
    char *id = cps_dup(cps, id_start, i);

    Shape shape = SHAPE_RECT;
    char *label = NULL;
    bool has_label = false;
    size_t after = i;

    if (i < n) {
        uint32_t c = cps[i];
        if (c == '[') {
            if (i + 1 < n && cps[i + 1] == '[') {
                ShapeResult r = read_shape(cps, n, i + 2, "]]", SHAPE_RECT);
                shape = r.shape; label = r.label; has_label = r.has_label; after = r.after;
            } else if (i + 1 < n && cps[i + 1] == '(') {
                ShapeResult r = read_shape(cps, n, i + 2, ")]", SHAPE_ROUND);
                shape = r.shape; label = r.label; has_label = r.has_label; after = r.after;
            } else {
                ShapeResult r = read_shape(cps, n, i + 1, "]", SHAPE_RECT);
                shape = r.shape; label = r.label; has_label = r.has_label; after = r.after;
            }
        } else if (c == '(') {
            if (i + 1 < n && cps[i + 1] == '(') {
                ShapeResult r = read_shape(cps, n, i + 2, "))", SHAPE_ROUND);
                shape = r.shape; label = r.label; has_label = r.has_label; after = r.after;
            } else if (i + 1 < n && cps[i + 1] == '[') {
                ShapeResult r = read_shape(cps, n, i + 2, "])", SHAPE_ROUND);
                shape = r.shape; label = r.label; has_label = r.has_label; after = r.after;
            } else {
                ShapeResult r = read_shape(cps, n, i + 1, ")", SHAPE_ROUND);
                shape = r.shape; label = r.label; has_label = r.has_label; after = r.after;
            }
        } else if (c == '{') {
            if (i + 1 < n && cps[i + 1] == '{') {
                ShapeResult r = read_shape(cps, n, i + 2, "}}", SHAPE_DIAMOND);
                shape = r.shape; label = r.label; has_label = r.has_label; after = r.after;
            } else {
                ShapeResult r = read_shape(cps, n, i + 1, "}", SHAPE_DIAMOND);
                shape = r.shape; label = r.label; has_label = r.has_label; after = r.after;
            }
        } else if (c == '>') {
            ShapeResult r = read_shape(cps, n, i + 1, "]", SHAPE_RECT);
            shape = r.shape; label = r.label; has_label = r.has_label; after = r.after;
        }
    }

    int idx = graph_node_index(g, id, has_label ? label : NULL, shape);
    free(id);
    free(label);
    if (idx < 0) return false;
    *idx_out = idx;
    *after_out = after;
    return true;
}

static bool parse_node_group(const uint32_t *cps, size_t n, size_t start, Graph *g, IntVec *group, size_t *after_out) {
    intvec_init(group);
    int first;
    size_t i;
    if (!parse_node(cps, n, start, g, &first, &i)) return false;
    intvec_push(group, first);
    while (true) {
        size_t j = skip_spaces(cps, n, i);
        if (!(j < n && cps[j] == '&')) break;
        int next; size_t k;
        if (!parse_node(cps, n, j + 1, g, &next, &k)) break;
        intvec_push(group, next);
        i = k;
    }
    *after_out = i;
    return true;
}

typedef struct {
    Head left, right;
    LineKind line;
    char *label;
    size_t after;
    bool ok;
} LinkResult;

static LineKind line_kind(const char *op) {
    if (strchr(op, '=')) return LINE_THICK;
    if (strchr(op, '.')) return LINE_DOTTED;
    return LINE_SOLID;
}

static bool trailing_head(const uint32_t *cps, size_t n, size_t i, Head *head, size_t *after) {
    if (i >= n) return false;
    if (cps[i] == 'o') *head = HEAD_CIRCLE;
    else if (cps[i] == 'x') *head = HEAD_CROSS;
    else return false;
    size_t j = i + 1;
    if (j >= n || cps[j] == ' ' || cps[j] == '\t' || cps[j] == '|' || cps[j] == '&' || cps[j] == ';') {
        *after = j;
        return true;
    }
    return false;
}

static LinkResult parse_link(const uint32_t *cps, size_t n, size_t start) {
    LinkResult res; res.ok = false; res.label = NULL;
    size_t i = skip_spaces(cps, n, start);
    Head left = HEAD_NONE;
    if (i < n && (cps[i] == 'o' || cps[i] == 'x') && i + 1 < n &&
        (cps[i + 1] == '-' || cps[i + 1] == '.' || cps[i + 1] == '=')) {
        left = cps[i] == 'o' ? HEAD_CIRCLE : HEAD_CROSS;
        i++;
    }
    size_t op_start = i;
    while (i < n && cp_is_link_char(cps[i])) i++;
    if (i == op_start) return res;
    char *op1 = cps_dup(cps, op_start, i);
    if (left == HEAD_NONE && op1[0] == '<') left = HEAD_ARROW;
    LineKind line = line_kind(op1);
    Head right = strchr(op1, '>') ? HEAD_ARROW : HEAD_NONE;
    free(op1);

    if (right == HEAD_NONE) {
        Head h; size_t after;
        if (trailing_head(cps, n, i, &h, &after)) { right = h; i = after; }
    }

    if (i < n && cps[i] == '|') {
        i++;
        size_t l_start = i;
        while (i < n && cps[i] != '|') i++;
        char *raw = cps_dup(cps, l_start, i);
        char *label = clean_label(raw);
        free(raw);
        if (i < n && cps[i] == '|') i++;
        res.left = left; res.right = right; res.line = line;
        res.label = (label[0] == 0) ? (free(label), NULL) : label;
        res.after = i; res.ok = true;
        return res;
    }

    if (right == HEAD_NONE) {
        size_t text_start = skip_spaces(cps, n, i);
        size_t j = text_start;
        while (j < n && !cp_is_link_char(cps[j])) j++;
        if (j < n && j > text_start && (cps[j] == '-' || cps[j] == '.' || cps[j] == '=' || cps[j] == '>')) {
            char *text = cps_dup(cps, text_start, j);
            size_t op2_start = j;
            while (j < n && cp_is_link_char(cps[j])) j++;
            char *op2 = cps_dup(cps, op2_start, j);
            if (strchr(op2, '>')) right = HEAD_ARROW;
            else {
                Head h; size_t after;
                if (trailing_head(cps, n, j, &h, &after)) { right = h; j = after; }
                else right = HEAD_NONE;
            }
            if (line == LINE_SOLID) line = line_kind(op2);
            free(op2);
            char *label = clean_label(text);
            free(text);
            res.left = left; res.right = right; res.line = line;
            res.label = (label[0] == 0) ? (free(label), NULL) : label;
            res.after = j; res.ok = true;
            return res;
        }
    }

    res.left = left; res.right = right; res.line = line; res.label = NULL;
    res.after = i; res.ok = true;
    return res;
}

static void parse_statement(const char *st, Graph *g) {
    uint32_t *cps; size_t n = utf8_decode(st, &cps);
    size_t i = 0;
    IntVec prev;
    if (!parse_node_group(cps, n, i, g, &prev, &i)) { free(cps); return; }

    while (true) {
        i = skip_spaces(cps, n, i);
        if (i >= n) break;
        LinkResult link = parse_link(cps, n, i);
        if (!link.ok) break;
        i = skip_spaces(cps, n, link.after);
        IntVec next;
        if (!parse_node_group(cps, n, i, g, &next, &i)) { free(link.label); intvec_free(&prev); free(cps); return; }
        for (size_t a = 0; a < prev.n; a++) {
            for (size_t b = 0; b < next.n; b++) {
                if (g->n_edges >= MAX_EDGES) { g->over_cap = true; free(link.label); intvec_free(&prev); intvec_free(&next); free(cps); return; }
                int f = prev.data[a], t = next.data[b];
                if (link.left == HEAD_ARROW && link.right != HEAD_ARROW) {
                    graph_push_edge(g, t, f, link.label, HEAD_ARROW, link.right, link.line);
                } else {
                    graph_push_edge(g, f, t, link.label, link.right, link.left, link.line);
                }
            }
        }
        free(link.label);
        intvec_free(&prev);
        prev = next;
    }
    intvec_free(&prev);
    free(cps);
}

static void parse_subgraph_decl(const char *rest_in, char **id_out, char **label_out) {
    Slice rest = slice_trim(slice_cstr(rest_in));
    bool ok;
    Slice q = slice_strip_prefix(rest, "\"", &ok);
    if (ok) {
        Slice left, right;
        if (slice_split_once_char(q, '"', &left, &right)) {
            *id_out = slice_dup(left);
            char *l = slice_dup(left);
            *label_out = decode_html_entities(l);
            free(l);
            return;
        }
    }
    long open = slice_find_char(rest, '[');
    if (open >= 0) {
        Slice id = slice_trim((Slice){ rest.p, (size_t)open });
        Slice labelraw = (Slice){ rest.p + open + 1, rest.len - (size_t)open - 1 };
        labelraw = slice_trim(labelraw);
        while (labelraw.len > 0 && labelraw.p[labelraw.len - 1] == ']') labelraw.len--;
        labelraw = slice_trim(labelraw);
        if (!slice_is_empty(id) && !slice_is_empty(labelraw)) {
            char *lraw = slice_dup(labelraw);
            char *label = clean_label(lraw);
            free(lraw);
            if (label[0] != 0) {
                *id_out = slice_dup(id);
                *label_out = label;
                return;
            }
            free(label);
        }
    }
    *id_out = slice_dup(rest);
    *label_out = slice_dup(rest);
}

Graph *parse_graph(const char *src) {
    StrVec statements;
    collect_statements(src, &statements);
    if (statements.n == 0) { strvec_free(&statements); return NULL; }

    Slice header = slice_cstr(statements.data[0]);
    Slice rest;
    Slice kindw = slice_first_word(header, &rest);
    char *kind = slice_to_lower(kindw);
    bool is_flow = strcmp(kind, "graph") == 0 || strcmp(kind, "flowchart") == 0;
    free(kind);
    if (!is_flow) { strvec_free(&statements); return NULL; }

    Slice dirw = slice_first_word(rest, NULL);
    char *dirs = slice_to_lower(dirw);
    for (char *p = dirs; *p; p++) *p = (char)toupper((unsigned char)*p);
    Dir dir = DIR_DOWN;
    if (strcmp(dirs, "LR") == 0) dir = DIR_RIGHT;
    else if (strcmp(dirs, "RL") == 0) dir = DIR_LEFT;
    else if (strcmp(dirs, "BT") == 0) dir = DIR_UP;
    free(dirs);

    Graph *g = graph_new();
    g->dir = dir;

    IntVec stack;
    intvec_init(&stack);

    for (size_t si = 1; si < statements.n; si++) {
        const char *st = statements.data[si];
        Slice sl = slice_cstr(st);
        Slice first = slice_first_word(sl, NULL);
        char *fw = slice_to_lower(first);

        if (strcmp(fw, "subgraph") == 0) {
            free(fw);
            if (g->n_groups >= MAX_GROUPS || stack.n >= MAX_GROUP_DEPTH) {
                intvec_free(&stack); strvec_free(&statements); graph_free(g); return NULL;
            }
            Slice rest2 = { sl.p + 8, sl.len - 8 };
            char *rest2c = slice_dup(slice_trim(rest2));
            char *id, *label;
            parse_subgraph_decl(rest2c, &id, &label);
            free(rest2c);
            int parent = stack.n > 0 ? stack.data[stack.n - 1] : -1;
            int gi = graph_push_group(g, id, label, parent);
            free(id); free(label);
            intvec_push(&stack, gi);
            g->cur_group = gi;
            continue;
        }
        if (strcmp(fw, "end") == 0) {
            free(fw);
            if (stack.n > 0) stack.n--;
            g->cur_group = stack.n > 0 ? stack.data[stack.n - 1] : -1;
            continue;
        }
        if (strcmp(fw, "classdef") == 0 || strcmp(fw, "class") == 0 || strcmp(fw, "style") == 0 ||
            strcmp(fw, "linkstyle") == 0 || strcmp(fw, "click") == 0 || strcmp(fw, "direction") == 0) {
            free(fw);
            continue;
        }
        free(fw);
        parse_statement(st, g);
        if (g->over_cap) { intvec_free(&stack); strvec_free(&statements); graph_free(g); return NULL; }
    }
    intvec_free(&stack);
    strvec_free(&statements);

    if (g->n_nodes == 0) { graph_free(g); return NULL; }
    return g;
}
