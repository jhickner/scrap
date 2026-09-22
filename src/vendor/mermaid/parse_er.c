#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"
#include "strutil.h"

static const char *er_card(Slice tok) {
    if (slice_eq_cstr(tok, "|o") || slice_eq_cstr(tok, "o|")) return "0..1";
    if (slice_eq_cstr(tok, "||")) return "1";
    if (slice_eq_cstr(tok, "}o") || slice_eq_cstr(tok, "o{")) return "0..*";
    if (slice_eq_cstr(tok, "}|") || slice_eq_cstr(tok, "|{")) return "1..*";
    return NULL;
}

bool parse_er_op(const char *tok, const char **card_l, const char **card_r, LineKind *line) {
    size_t len = strlen(tok);
    if (len != 6) return false;
    for (size_t i = 0; i < len; i++) if ((unsigned char)tok[i] > 127) return false;
    if (strncmp(tok + 2, "--", 2) == 0) *line = LINE_SOLID;
    else if (strncmp(tok + 2, "..", 2) == 0) *line = LINE_DOTTED;
    else return false;
    const char *l = er_card((Slice){ tok, 2 });
    const char *r = er_card((Slice){ tok + 4, 2 });
    if (!l || !r) return false;
    *card_l = l; *card_r = r;
    return true;
}

void push_er_attribute(ClassInfo *ci, const char *raw) {
    Slice s = slice_cstr(raw);
    Buf joined; buf_init(&joined);
    bool any = false;
    Slice rest = s;
    while (true) {
        rest = slice_trim_start(rest);
        if (slice_is_empty(rest)) break;
        size_t i = 0;
        while (i < rest.len && rest.p[i] != ' ' && rest.p[i] != '\t') i++;
        Slice tok = { rest.p, i };
        rest = (Slice){ rest.p + i, rest.len - i };
        if (tok.len > 0 && tok.p[0] == '"') break;
        if (any) buf_push_char(&joined, ' ');
        buf_push_slice(&joined, tok);
        any = true;
    }
    if (!any) { buf_free(&joined); return; }
    char *j = buf_take(&joined);
    char *line = decode_html_entities(j);
    free(j);
    if (ci->n_attrs < MAX_MEMBERS) {
        if (ci->n_attrs == ci->cap_attrs) { ci->cap_attrs = ci->cap_attrs ? ci->cap_attrs * 2 : 4; ci->attrs = realloc(ci->attrs, ci->cap_attrs * sizeof(char *)); }
        ci->attrs[ci->n_attrs++] = line;
    } else if (ci->n_attrs == MAX_MEMBERS) {
        if (ci->n_attrs == ci->cap_attrs) { ci->cap_attrs = ci->cap_attrs ? ci->cap_attrs * 2 : 4; ci->attrs = realloc(ci->attrs, ci->cap_attrs * sizeof(char *)); }
        ci->attrs[ci->n_attrs++] = cstr_dup("\xE2\x80\xA6");
        free(line);
    } else {
        free(line);
    }
}

static void sync_infos(const Graph *g, ClassInfo **infos, size_t *n_infos) {
    while (*n_infos < g->n_nodes) {
        *infos = realloc(*infos, (*n_infos + 1) * sizeof(ClassInfo));
        memset(&(*infos)[*n_infos], 0, sizeof(ClassInfo));
        (*n_infos)++;
    }
}

static int er_entity(Graph *g, ClassInfo **infos, size_t *n_infos, const char *token) {
    const char *open = strchr(token, '[');
    int idx;
    if (open) {
        char *id = malloc((size_t)(open - token) + 1);
        memcpy(id, token, (size_t)(open - token));
        id[open - token] = 0;
        Slice inner = slice_cstr(open + 1);
        inner = slice_trim_end_char(inner, ']');
        char *ic = slice_dup(inner);
        char *label = clean_label(ic);
        free(ic);
        Slice idtrim = slice_trim(slice_cstr(id));
        if (slice_is_empty(idtrim) || label[0] == 0) { free(id); free(label); return -1; }
        char *idc = slice_dup(idtrim);
        idx = graph_node_label(g, idc, label);
        free(id); free(idc); free(label);
    } else {
        idx = graph_node_index(g, token, NULL, SHAPE_RECT);
    }
    sync_infos(g, infos, n_infos);
    return idx;
}

static bool split_er_relationship(const char *st, Slice *rel, Slice *label) {
    Slice s = slice_cstr(st);
    Slice l, r;
    if (slice_split_once_char(s, ':', &l, &r)) { *rel = l; *label = slice_trim(r); }
    else { *rel = s; label->p = NULL; label->len = 0; }
    Slice rest = *rel;
    while (true) {
        rest = slice_trim_start(rest);
        if (slice_is_empty(rest)) break;
        size_t i = 0;
        while (i < rest.len && rest.p[i] != ' ' && rest.p[i] != '\t') i++;
        char *tok = slice_dup((Slice){ rest.p, i });
        const char *cl, *cr; LineKind ln;
        bool has = parse_er_op(tok, &cl, &cr, &ln);
        free(tok);
        if (has) return true;
        rest = (Slice){ rest.p + i, rest.len - i };
    }
    return false;
}

Graph *parse_er(const char *src, ClassInfo **infos_out, size_t *n_infos_out) {
    StrVec statements;
    collect_statements(src, &statements);
    if (statements.n == 0) { strvec_free(&statements); return NULL; }
    Slice header = slice_cstr(statements.data[0]);
    Slice hw = slice_first_word(header, NULL);
    bool ok_header = slice_eq_ignore_case_cstr(hw, "erdiagram");
    if (!ok_header) { strvec_free(&statements); return NULL; }

    Graph *g = graph_new();
    ClassInfo *infos = NULL; size_t n_infos = 0;
    int cur_entity = -1;

    for (size_t si = 1; si < statements.n; si++) {
        const char *st = statements.data[si];
        if (cur_entity >= 0) {
            if (strcmp(st, "}") == 0) cur_entity = -1;
            else push_er_attribute(&infos[cur_entity], st);
            continue;
        }
        Slice rel, label;
        if (split_er_relationship(st, &rel, &label)) {
            char *relc = slice_dup(rel);
            StrVec toks; strvec_init(&toks);
            Slice rest = slice_cstr(relc);
            while (true) {
                rest = slice_trim_start(rest);
                if (slice_is_empty(rest)) break;
                size_t i = 0;
                while (i < rest.len && rest.p[i] != ' ' && rest.p[i] != '\t') i++;
                strvec_push(&toks, slice_dup((Slice){ rest.p, i }));
                rest = (Slice){ rest.p + i, rest.len - i };
            }
            free(relc);
            if (toks.n != 3) { strvec_free(&toks); goto fail; }
            const char *cl, *cr; LineKind ln;
            if (!parse_er_op(toks.data[1], &cl, &cr, &ln)) { strvec_free(&toks); goto fail; }
            int f = er_entity(g, &infos, &n_infos, toks.data[0]);
            int t = er_entity(g, &infos, &n_infos, toks.data[2]);
            if (f < 0 || t < 0) { strvec_free(&toks); goto fail; }
            if (g->n_edges >= MAX_EDGES) { strvec_free(&toks); goto fail; }
            char *rel_label;
            if (label.p) {
                char *tmp = slice_dup(label);
                rel_label = clean_label(tmp);
                free(tmp);
            } else {
                rel_label = cstr_dup("");
            }
            Buf lb; buf_init(&lb);
            bool any = false;
            if (cl[0]) { buf_push_cstr(&lb, cl); any = true; }
            if (rel_label[0]) { if (any) buf_push_char(&lb, ' '); buf_push_cstr(&lb, rel_label); any = true; }
            if (cr[0]) { if (any) buf_push_char(&lb, ' '); buf_push_cstr(&lb, cr); any = true; }
            free(rel_label);
            char *lbs = buf_take(&lb);
            graph_push_edge(g, f, t, (any && lbs[0]) ? lbs : NULL, HEAD_NONE, HEAD_NONE, ln);
            free(lbs);
            strvec_free(&toks);
            continue;
        }

        Slice sl = slice_cstr(st);
        bool open;
        Slice decl = slice_strip_suffix(sl, "{", &open);
        decl = slice_trim(decl);
        size_t wc = 0;
        {
            Slice r2 = decl;
            while (true) {
                r2 = slice_trim_start(r2);
                if (slice_is_empty(r2)) break;
                size_t i = 0;
                while (i < r2.len && r2.p[i] != ' ' && r2.p[i] != '\t') i++;
                wc++;
                r2 = (Slice){ r2.p + i, r2.len - i };
            }
        }
        if (slice_is_empty(decl) || wc != 1) goto fail;
        char *declc = slice_dup(decl);
        int idx = er_entity(g, &infos, &n_infos, declc);
        free(declc);
        if (idx < 0) goto fail;
        if (open) cur_entity = idx;
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
