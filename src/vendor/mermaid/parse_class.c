#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"
#include "strutil.h"

typedef struct { const char *op; Head hf, ht; LineKind line; } ClassOp;
static const ClassOp CLASS_OPS[] = {
    { "<|--", HEAD_TRIANGLE, HEAD_NONE, LINE_SOLID },
    { "--|>", HEAD_NONE, HEAD_TRIANGLE, LINE_SOLID },
    { "<|..", HEAD_TRIANGLE, HEAD_NONE, LINE_DOTTED },
    { "..|>", HEAD_NONE, HEAD_TRIANGLE, LINE_DOTTED },
    { "*--", HEAD_DIAMOND_FILL, HEAD_NONE, LINE_SOLID },
    { "--*", HEAD_NONE, HEAD_DIAMOND_FILL, LINE_SOLID },
    { "o--", HEAD_DIAMOND_OPEN, HEAD_NONE, LINE_SOLID },
    { "--o", HEAD_NONE, HEAD_DIAMOND_OPEN, LINE_SOLID },
    { "<--", HEAD_ARROW, HEAD_NONE, LINE_SOLID },
    { "-->", HEAD_NONE, HEAD_ARROW, LINE_SOLID },
    { "<..", HEAD_ARROW, HEAD_NONE, LINE_DOTTED },
    { "..>", HEAD_NONE, HEAD_ARROW, LINE_DOTTED },
    { "--", HEAD_NONE, HEAD_NONE, LINE_SOLID },
    { "..", HEAD_NONE, HEAD_NONE, LINE_DOTTED },
};
#define N_CLASS_OPS (sizeof(CLASS_OPS) / sizeof(CLASS_OPS[0]))

static void ci_push_attr(ClassInfo *ci, char *s) {
    if (ci->n_attrs == ci->cap_attrs) { ci->cap_attrs = ci->cap_attrs ? ci->cap_attrs * 2 : 4; ci->attrs = realloc(ci->attrs, ci->cap_attrs * sizeof(char *)); }
    ci->attrs[ci->n_attrs++] = s;
}
static void ci_push_method(ClassInfo *ci, char *s) {
    if (ci->n_methods == ci->cap_methods) { ci->cap_methods = ci->cap_methods ? ci->cap_methods * 2 : 4; ci->methods = realloc(ci->methods, ci->cap_methods * sizeof(char *)); }
    ci->methods[ci->n_methods++] = s;
}

char *display_generics(const char *s) {
    Buf out; buf_init(&out);
    bool open = false;
    for (const char *p = s; *p; p++) {
        if (*p == '~') { buf_push_char(&out, open ? '>' : '<'); open = !open; }
        else buf_push_char(&out, *p);
    }
    return buf_take(&out);
}

void push_member(ClassInfo *ci, const char *raw) {
    Slice r = slice_cstr(raw);
    bool ok;
    Slice ann = slice_strip_prefix(r, "<<", &ok);
    if (ok) {
        Slice left, right;
        if (slice_split_once_cstr(ann, ">>", &left, &right)) {
            (void)right;
            free(ci->annotation);
            ci->annotation = slice_dup(slice_trim(left));
        }
        return;
    }
    Slice trimmed = slice_trim(r);
    char *tc = slice_dup(trimmed);
    char *disp = display_generics(tc);
    char *member = decode_html_entities(disp);
    free(tc); free(disp);
    bool is_method = strchr(member, '(') != NULL;
    size_t *cnt = is_method ? &ci->n_methods : &ci->n_attrs;
    if (*cnt < MAX_MEMBERS) {
        if (is_method) ci_push_method(ci, member); else ci_push_attr(ci, member);
    } else if (*cnt == MAX_MEMBERS) {
        if (is_method) ci_push_method(ci, cstr_dup("\xE2\x80\xA6")); else ci_push_attr(ci, cstr_dup("\xE2\x80\xA6"));
        free(member);
    } else {
        free(member);
    }
}

void class_infos_free(ClassInfo *infos, size_t n) {
    for (size_t i = 0; i < n; i++) {
        free(infos[i].annotation);
        for (size_t j = 0; j < infos[i].n_attrs; j++) free(infos[i].attrs[j]);
        free(infos[i].attrs);
        for (size_t j = 0; j < infos[i].n_methods; j++) free(infos[i].methods[j]);
        free(infos[i].methods);
    }
    free(infos);
}

static void sync_infos(const Graph *g, ClassInfo **infos, size_t *n_infos) {
    while (*n_infos < g->n_nodes) {
        *infos = realloc(*infos, (*n_infos + 1) * sizeof(ClassInfo));
        memset(&(*infos)[*n_infos], 0, sizeof(ClassInfo));
        (*n_infos)++;
    }
}

static Slice strip_cardinality_suffix(Slice s, char **card_out) {
    Slice t = slice_trim_end(s);
    bool ok;
    Slice rest = slice_strip_suffix(t, "\"", &ok);
    if (ok) {
        long q = slice_rfind_char(rest, '"');
        if (q >= 0) {
            Slice card = { rest.p + q + 1, rest.len - (size_t)q - 1 };
            *card_out = slice_dup(card);
            return slice_trim_end((Slice){ rest.p, (size_t)q });
        }
    }
    *card_out = cstr_dup("");
    return t;
}
static Slice strip_cardinality_prefix(Slice s, char **card_out) {
    Slice t = slice_trim_start(s);
    bool ok;
    Slice rest = slice_strip_prefix(t, "\"", &ok);
    if (ok) {
        long q = slice_find_char(rest, '"');
        if (q >= 0) {
            Slice card = { rest.p, (size_t)q };
            *card_out = slice_dup(card);
            return slice_trim_start((Slice){ rest.p + q + 1, rest.len - (size_t)q - 1 });
        }
    }
    *card_out = cstr_dup("");
    return t;
}

static bool parse_class_relation(const char *st, char **from_out, char **to_out, Head *hf, Head *ht, LineKind *line, char **label_out) {
    size_t len = strlen(st);
    long found_pos = -1; const ClassOp *found_op = NULL;
    for (size_t pos = 0; pos < len && !found_op; pos++) {
        for (size_t k = 0; k < N_CLASS_OPS; k++) {
            const char *op = CLASS_OPS[k].op;
            size_t ol = strlen(op);
            if (pos + ol > len || memcmp(st + pos, op, ol) != 0) continue;
            if (op[0] == 'o' && pos > 0 && is_id_char(st[pos - 1])) continue;
            if (op[ol - 1] == 'o' && pos + ol < len && is_id_char(st[pos + ol])) continue;
            found_pos = (long)pos; found_op = &CLASS_OPS[k];
            break;
        }
    }
    if (!found_op) return false;
    size_t ol = strlen(found_op->op);
    Slice lhs = slice_trim((Slice){ st, (size_t)found_pos });
    Slice rhs = slice_trim((Slice){ st + found_pos + ol, len - (size_t)found_pos - ol });

    char *card_from; Slice lhs2 = strip_cardinality_suffix(lhs, &card_from);
    char *card_to; Slice rhs2 = strip_cardinality_prefix(rhs, &card_to);

    Slice to_id, rel_label_s;
    char *rel_label = NULL;
    if (slice_split_once_char(rhs2, ':', &to_id, &rel_label_s)) {
        to_id = slice_trim(to_id);
        Slice t = slice_trim(rel_label_s);
        char *tc = slice_dup(t);
        char *dec = decode_html_entities(tc);
        free(tc);
        if (dec[0]) rel_label = dec; else free(dec);
    } else {
        to_id = slice_trim(rhs2);
    }
    if (slice_is_empty(lhs2) || slice_is_empty(to_id) || slice_has_whitespace(lhs2) || slice_has_whitespace(to_id)) {
        free(card_from); free(card_to); free(rel_label);
        return false;
    }

    Buf label; buf_init(&label);
    bool any = false;
    if (card_from[0]) { buf_push_cstr(&label, card_from); any = true; }
    if (rel_label && rel_label[0]) { if (any) buf_push_char(&label, ' '); buf_push_cstr(&label, rel_label); any = true; }
    if (card_to[0]) { if (any) buf_push_char(&label, ' '); buf_push_cstr(&label, card_to); any = true; }
    free(card_from); free(card_to); free(rel_label);

    *from_out = slice_dup(lhs2);
    *to_out = slice_dup(to_id);
    *hf = found_op->hf; *ht = found_op->ht; *line = found_op->line;
    char *l = buf_take(&label);
    *label_out = (any && l[0]) ? l : (free(l), NULL);
    return true;
}

Graph *parse_class(const char *src, ClassInfo **infos_out, size_t *n_infos_out) {
    StrVec statements;
    collect_statements(src, &statements);
    if (statements.n == 0) { strvec_free(&statements); return NULL; }
    Slice header = slice_cstr(statements.data[0]);
    Slice hw = slice_first_word(header, NULL);
    char *hwl = slice_to_lower(hw);
    bool ok_header = strncmp(hwl, "classdiagram", 12) == 0;
    free(hwl);
    if (!ok_header) { strvec_free(&statements); return NULL; }

    Graph *g = graph_new();
    ClassInfo *infos = NULL; size_t n_infos = 0;
    int cur_class = -1;

    for (size_t si = 1; si < statements.n; si++) {
        const char *st = statements.data[si];
        if (cur_class >= 0) {
            if (strcmp(st, "}") == 0) { cur_class = -1; }
            else push_member(&infos[cur_class], st);
            continue;
        }
        Slice sl = slice_cstr(st);
        Slice first = slice_first_word(sl, NULL);
        char *fw = slice_to_lower(first);
        bool handled = true;
        if (strcmp(fw, "direction") == 0) {
            Slice rest; slice_first_word(sl, &rest);
            Slice dw = slice_first_word(rest, NULL);
            char *up = slice_to_lower(dw);
            for (char *p = up; *p; p++) *p = (char)toupper((unsigned char)*p);
            if (strcmp(up, "LR") == 0) g->dir = DIR_RIGHT;
            else if (strcmp(up, "RL") == 0) g->dir = DIR_LEFT;
            else if (strcmp(up, "BT") == 0) g->dir = DIR_UP;
            else g->dir = DIR_DOWN;
            free(up);
        } else if (strcmp(fw, "note") == 0 || strcmp(fw, "callback") == 0 || strcmp(fw, "click") == 0 ||
                   strcmp(fw, "link") == 0 || strcmp(fw, "style") == 0 || strcmp(fw, "cssclass") == 0 ||
                   strcmp(fw, "classdef") == 0 || strcmp(fw, "namespace") == 0 || strcmp(fw, "}") == 0) {
            /* skip */
        } else if (strcmp(fw, "class") == 0) {
            Slice rest = slice_trim((Slice){ sl.p + 5, sl.len - 5 });
            bool open;
            Slice name = slice_strip_suffix(rest, "{", &open);
            name = slice_trim(name);
            if (slice_is_empty(name) || slice_has_whitespace(name)) { free(fw); strvec_free(&statements); graph_free(g); class_infos_free(infos, n_infos); return NULL; }
            char *namec = slice_dup(name);
            int idx = graph_node_index(g, namec, NULL, SHAPE_RECT);
            free(namec);
            sync_infos(g, &infos, &n_infos);
            if (open) cur_class = idx;
        } else {
            handled = false;
        }
        free(fw);
        if (handled) continue;

        bool ann_ok;
        Slice ann = slice_strip_prefix(sl, "<<", &ann_ok);
        if (ann_ok) {
            Slice annName, rest;
            if (!slice_split_once_cstr(ann, ">>", &annName, &rest)) goto fail;
            Slice name = slice_trim(rest);
            if (slice_is_empty(name) || slice_has_whitespace(name)) goto fail;
            char *namec = slice_dup(name);
            int idx = graph_node_index(g, namec, NULL, SHAPE_RECT);
            free(namec);
            sync_infos(g, &infos, &n_infos);
            free(infos[idx].annotation);
            infos[idx].annotation = slice_dup(slice_trim(annName));
            continue;
        }

        char *fromid, *toid, *label; Head hf, ht; LineKind line;
        if (parse_class_relation(st, &fromid, &toid, &hf, &ht, &line, &label)) {
            int f = graph_node_index(g, fromid, NULL, SHAPE_RECT);
            sync_infos(g, &infos, &n_infos);
            int t = graph_node_index(g, toid, NULL, SHAPE_RECT);
            sync_infos(g, &infos, &n_infos);
            free(fromid); free(toid);
            if (g->n_edges >= MAX_EDGES) { free(label); goto fail; }
            graph_push_edge(g, f, t, label, ht, hf, line);
            free(label);
            continue;
        }

        Slice id, member;
        if (slice_split_once_char(sl, ':', &id, &member)) {
            id = slice_trim(id);
            member = slice_trim(member);
            if (slice_is_empty(id) || slice_has_whitespace(id) || slice_is_empty(member)) goto fail;
            char *idc = slice_dup(id);
            int idx = graph_node_index(g, idc, NULL, SHAPE_RECT);
            free(idc);
            sync_infos(g, &infos, &n_infos);
            char *mc = slice_dup(member);
            push_member(&infos[idx], mc);
            free(mc);
            continue;
        }
        goto fail;
    }
    strvec_free(&statements);
    if (g->n_nodes == 0) goto fail2;
    sync_infos(g, &infos, &n_infos);
    *infos_out = infos; *n_infos_out = n_infos;
    return g;

fail:
    strvec_free(&statements);
fail2:
    graph_free(g);
    class_infos_free(infos, n_infos);
    return NULL;
}
