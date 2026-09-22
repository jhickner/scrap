#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "internal.h"
#include "strutil.h"

static Dir parse_dir(Slice s) {
    char *up = slice_to_lower(s);
    for (char *p = up; *p; p++) *p = (char)toupper((unsigned char)*p);
    Dir d = DIR_DOWN;
    if (strcmp(up, "LR") == 0) d = DIR_RIGHT;
    else if (strcmp(up, "RL") == 0) d = DIR_LEFT;
    else if (strcmp(up, "BT") == 0) d = DIR_UP;
    free(up);
    return d;
}

static bool state_endpoint(Graph *g, const char *id, bool is_source, int *out) {
    if (strcmp(id, "[*]") == 0) {
        *out = graph_node_index(g, is_source ? "[*]start" : "[*]end", "\xE2\x97\x8F", SHAPE_ROUND);
        return *out >= 0;
    }
    *out = graph_node_index(g, id, NULL, SHAPE_ROUND);
    return *out >= 0;
}

static bool parse_state_decl(const char *st, Graph *g) {
    Slice rest = slice_cstr(st + 5);
    rest = slice_trim(rest);
    rest = slice_trim_end_char(rest, '{');
    rest = slice_trim(rest);
    if (slice_is_empty(rest)) return true;

    bool ok;
    Slice q = slice_strip_prefix(rest, "\"", &ok);
    if (ok) {
        Slice label, after;
        if (!slice_split_once_char(q, '"', &label, &after)) return false;
        after = slice_trim(after);
        bool asok;
        Slice id = slice_strip_prefix(after, "as", &asok);
        if (asok) id = slice_trim(id); else id = label;
        char *idc = slice_dup(id);
        char *labelc = slice_dup(label);
        char *decoded = decode_html_entities(labelc);
        int r = graph_node_label(g, idc, decoded);
        free(idc); free(labelc); free(decoded);
        return r >= 0;
    }

    Shape shape = SHAPE_ROUND;
    Slice id = rest;
    bool stereotyped = false;
    long pos = slice_find_cstr(rest, "<<");
    if (pos >= 0) {
        Slice stereo = { rest.p + pos + 2, rest.len - (size_t)pos - 2 };
        bool sok = true;
        while (sok) stereo = slice_strip_suffix(stereo, ">>", &sok);
        stereo = slice_trim(stereo);
        if (slice_eq_cstr(stereo, "choice")) shape = SHAPE_DIAMOND;
        id = slice_trim((Slice){ rest.p, (size_t)pos });
        stereotyped = true;
    }
    if (slice_is_empty(id) || slice_has_whitespace(id)) return false;
    char *idc = slice_dup(id);
    int r = graph_node_index(g, idc, stereotyped ? idc : NULL, shape);
    free(idc);
    return r >= 0;
}

static bool parse_transition(const char *st, Graph *g) {
    Slice rest = slice_cstr(st);
    int prev = -1;
    bool have_prev = false;
    Slice lhs, rhs;
    while (slice_split_once_cstr(rest, "-->", &lhs, &rhs)) {
        Slice from_id = slice_trim_end(lhs);
        from_id = slice_trim_end_char(from_id, '-');
        from_id = slice_trim(from_id);
        int from;
        if (have_prev) {
            if (!slice_is_empty(from_id)) return false;
            from = prev;
        } else {
            if (slice_is_empty(from_id)) return false;
            char *idc = slice_dup(from_id);
            bool ok = state_endpoint(g, idc, true, &from);
            free(idc);
            if (!ok) return false;
        }

        Slice to_part, tail;
        Slice tp2, dummy;
        if (slice_split_once_cstr(rhs, "-->", &tp2, &dummy)) {
            to_part = tp2;
            tail = (Slice){ rhs.p + to_part.len, rhs.len - to_part.len };
        } else {
            to_part = rhs;
            tail = (Slice){ rhs.p + rhs.len, 0 };
        }

        char *label = NULL;
        Slice lsplit_l, lsplit_r;
        if (slice_split_once_char(to_part, ':', &lsplit_l, &lsplit_r)) {
            to_part = lsplit_l;
            Slice lt = slice_trim(lsplit_r);
            char *lc = slice_dup(lt);
            char *dec = decode_html_entities(lc);
            free(lc);
            if (dec[0]) label = dec; else free(dec);
        }
        Slice to_id = slice_trim_start(to_part);
        to_id = slice_trim_start_char(to_id, '>');
        to_id = slice_trim_end(to_id);
        to_id = slice_trim_end_char(to_id, '-');
        to_id = slice_trim(to_id);
        if (slice_is_empty(to_id)) { free(label); return false; }
        char *toc = slice_dup(to_id);
        int to;
        bool ok = state_endpoint(g, toc, false, &to);
        free(toc);
        if (!ok) { free(label); return false; }

        if (g->n_edges >= MAX_EDGES) { g->over_cap = true; free(label); return true; }
        graph_push_edge(g, from, to, label, HEAD_ARROW, HEAD_NONE, LINE_SOLID);
        free(label);
        prev = to;
        have_prev = true;
        rest = tail;
    }
    return true;
}

static bool parse_state_desc(const char *st, Graph *g) {
    Slice s = slice_cstr(st);
    Slice id, desc;
    if (slice_split_once_char(s, ':', &id, &desc)) {
        id = slice_trim(id);
        desc = slice_trim(desc);
        if (slice_is_empty(id) || slice_has_whitespace(id) || slice_is_empty(desc)) return false;
        char *idc = slice_dup(id);
        char *descc = slice_dup(desc);
        char *dec = decode_html_entities(descc);
        int r = graph_node_label(g, idc, dec);
        free(idc); free(descc); free(dec);
        return r >= 0;
    }
    if (!slice_has_whitespace(s)) {
        char *idc = slice_dup(s);
        int r = graph_node_index(g, idc, NULL, SHAPE_ROUND);
        free(idc);
        return r >= 0;
    }
    return false;
}

Graph *parse_state(const char *src) {
    StrVec statements;
    collect_statements(src, &statements);
    if (statements.n == 0) { strvec_free(&statements); return NULL; }

    Slice header = slice_cstr(statements.data[0]);
    Slice hw = slice_first_word(header, NULL);
    char *hwl = slice_to_lower(hw);
    bool is_state = strncmp(hwl, "statediagram", 12) == 0;
    free(hwl);
    if (!is_state) { strvec_free(&statements); return NULL; }

    Graph *g = graph_new();
    bool in_note = false;

    for (size_t si = 1; si < statements.n; si++) {
        const char *st = statements.data[si];
        if (in_note) {
            if (strcasecmp(st, "end note") == 0) in_note = false;
            continue;
        }
        Slice sl = slice_cstr(st);
        Slice first = slice_first_word(sl, NULL);
        char *fw = slice_to_lower(first);

        bool fail = false;
        if (strcmp(fw, "direction") == 0) {
            Slice rest;
            slice_first_word(sl, &rest);
            Slice dw = slice_first_word(rest, NULL);
            g->dir = parse_dir(dw);
        } else if (strcmp(fw, "note") == 0) {
            if (!slice_contains_char(sl, ':')) in_note = true;
        } else if (strcmp(fw, "state") == 0) {
            fail = !parse_state_decl(st, g);
        } else if (strcmp(fw, "classdef") == 0 || strcmp(fw, "class") == 0 || strcmp(fw, "hide") == 0 ||
                   strcmp(fw, "scale") == 0 || strcmp(fw, "}") == 0 || strcmp(fw, "--") == 0) {
            /* no-op */
        } else if (slice_contains_cstr(sl, "-->")) {
            fail = !parse_transition(st, g);
        } else {
            fail = !parse_state_desc(st, g);
        }
        free(fw);
        if (fail || g->over_cap) { strvec_free(&statements); graph_free(g); return NULL; }
    }
    strvec_free(&statements);
    if (g->n_nodes == 0) { graph_free(g); return NULL; }
    return g;
}
