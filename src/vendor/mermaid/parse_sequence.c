#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sequence.h"
#include "strutil.h"

static Sequence *sequence_new(void) {
    Sequence *s = calloc(1, sizeof(Sequence));
    return s;
}

void sequence_free(Sequence *s) {
    if (!s) return;
    for (size_t i = 0; i < s->n_labels; i++) { free(s->ids[i]); free(s->labels[i]); }
    free(s->ids); free(s->labels);
    for (size_t i = 0; i < s->n_items; i++) free(s->items[i].text);
    free(s->items);
    free(s);
}

int sequence_participant(Sequence *s, const char *id, const char *label) {
    for (size_t i = 0; i < s->n_labels; i++) {
        if (strcmp(s->ids[i], id) == 0) {
            if (label) { free(s->labels[i]); s->labels[i] = cstr_dup(label); }
            return (int)i;
        }
    }
    if (s->n_labels >= MAX_NODES) return -1;
    if (s->n_labels == s->cap_labels) {
        s->cap_labels = s->cap_labels ? s->cap_labels * 2 : 16;
        s->ids = realloc(s->ids, s->cap_labels * sizeof(char *));
        s->labels = realloc(s->labels, s->cap_labels * sizeof(char *));
    }
    s->ids[s->n_labels] = cstr_dup(id);
    s->labels[s->n_labels] = cstr_dup(label ? label : id);
    s->n_labels++;
    return (int)(s->n_labels - 1);
}

static SeqItem *push_item(Sequence *s) {
    if (s->n_items == s->cap_items) { s->cap_items = s->cap_items ? s->cap_items * 2 : 16; s->items = realloc(s->items, s->cap_items * sizeof(SeqItem)); }
    SeqItem *it = &s->items[s->n_items++];
    memset(it, 0, sizeof(SeqItem));
    return it;
}

typedef struct { const char *op; bool dashed; SeqHead head; } SeqOp;
static const SeqOp SEQ_OPS[] = {
    { "-->>", true, SEQHEAD_ARROW },
    { "->>", false, SEQHEAD_ARROW },
    { "--x", true, SEQHEAD_CROSS },
    { "-x", false, SEQHEAD_CROSS },
    { "--)", true, SEQHEAD_ARROW },
    { "-)", false, SEQHEAD_ARROW },
    { "-->", true, SEQHEAD_ARROW },
    { "->", false, SEQHEAD_ARROW },
};
#define N_SEQ_OPS (sizeof(SEQ_OPS) / sizeof(SEQ_OPS[0]))

static bool parse_seq_message(const char *st, Sequence *s, int *from, int *to, char **text, bool *dashed, SeqHead *head) {
    size_t len = strlen(st);
    long pos = -1; const SeqOp *op = NULL;
    for (size_t p = 0; p < len && !op; p++) {
        for (size_t k = 0; k < N_SEQ_OPS; k++) {
            size_t ol = strlen(SEQ_OPS[k].op);
            if (p + ol <= len && memcmp(st + p, SEQ_OPS[k].op, ol) == 0) { pos = (long)p; op = &SEQ_OPS[k]; break; }
        }
    }
    if (!op) return false;
    size_t ol = strlen(op->op);
    Slice from_id = slice_trim((Slice){ st, (size_t)pos });
    if (slice_is_empty(from_id)) return false;
    Slice rest = slice_trim_start((Slice){ st + pos + ol, len - (size_t)pos - ol });
    while (rest.len > 0 && (rest.p[0] == '+' || rest.p[0] == '-')) { rest.p++; rest.len--; }

    Slice to_id, txt;
    char *textv = NULL;
    if (slice_split_once_char(rest, ':', &to_id, &txt)) {
        to_id = slice_trim(to_id);
        Slice t = slice_trim(txt);
        char *tc = slice_dup(t);
        char *dec = decode_html_entities(tc);
        free(tc);
        if (dec[0]) textv = dec; else free(dec);
    } else {
        to_id = slice_trim(rest);
    }
    if (slice_is_empty(to_id)) { free(textv); return false; }

    char *fromc = slice_dup(from_id);
    int f = sequence_participant(s, fromc, NULL);
    free(fromc);
    if (f < 0) { free(textv); return false; }
    char *toc = slice_dup(to_id);
    int t2 = sequence_participant(s, toc, NULL);
    free(toc);
    if (t2 < 0) { free(textv); return false; }

    *from = f; *to = t2; *text = textv; *dashed = op->dashed; *head = op->head;
    return true;
}

static bool parse_note_anchor(const char *rest_in, Sequence *s, char **text_out, NoteAnchorKind *kind_out, int *a_out, int *b_out) {
    Slice rest = slice_cstr(rest_in);
    char *lower = slice_to_lower(rest);
    Slice lo = slice_cstr(lower);
    Slice ids_and_text; NoteAnchorKind kind;
    bool ok;
    Slice l1 = slice_strip_prefix(lo, "over ", &ok);
    if (ok) { ids_and_text = (Slice){ rest.p + rest.len - l1.len, l1.len }; kind = ANCHOR_OVER; }
    else {
        Slice l2 = slice_strip_prefix(lo, "left of ", &ok);
        if (ok) { ids_and_text = (Slice){ rest.p + rest.len - l2.len, l2.len }; kind = ANCHOR_LEFT; }
        else {
            Slice l3 = slice_strip_prefix(lo, "right of ", &ok);
            if (ok) { ids_and_text = (Slice){ rest.p + rest.len - l3.len, l3.len }; kind = ANCHOR_RIGHT; }
            else { free(lower); return false; }
        }
    }
    free(lower);

    Slice ids, text;
    if (!slice_split_once_char(ids_and_text, ':', &ids, &text)) return false;
    Slice t = slice_trim(text);
    char *tc = slice_dup(t);
    char *dec = decode_html_entities(tc);
    free(tc);

    StrVec parts; strvec_init(&parts);
    Slice rest2 = ids;
    Slice piece;
    while (slice_split_once_char(rest2, ',', &piece, &rest2)) {
        Slice tp = slice_trim(piece);
        if (!slice_is_empty(tp)) strvec_push(&parts, slice_dup(tp));
    }
    Slice tailtrim = slice_trim(rest2);
    if (!slice_is_empty(tailtrim)) strvec_push(&parts, slice_dup(tailtrim));

    if (parts.n == 0) { free(dec); strvec_free(&parts); return false; }
    int a = sequence_participant(s, parts.data[0], NULL);
    if (a < 0) { free(dec); strvec_free(&parts); return false; }

    int b = a;
    if (kind == ANCHOR_OVER) {
        if (parts.n > 1) {
            b = sequence_participant(s, parts.data[1], NULL);
            if (b < 0) { free(dec); strvec_free(&parts); return false; }
        }
        *a_out = a < b ? a : b;
        *b_out = a < b ? b : a;
    } else if (kind == ANCHOR_LEFT) {
        *a_out = a; *b_out = a;
    } else {
        *a_out = a; *b_out = a;
    }
    strvec_free(&parts);
    *text_out = dec;
    *kind_out = kind;
    return true;
}

Sequence *parse_sequence(const char *src) {
    StrVec statements;
    collect_statements(src, &statements);
    if (statements.n == 0) { strvec_free(&statements); return NULL; }
    Slice header = slice_cstr(statements.data[0]);
    Slice hw = slice_first_word(header, NULL);
    if (!slice_eq_ignore_case_cstr(hw, "sequencediagram")) { strvec_free(&statements); return NULL; }

    Sequence *s = sequence_new();
    bool autonumber = false;
    size_t msg_count = 0;
    IntVec blocks;
    intvec_init(&blocks);

    for (size_t si = 1; si < statements.n; si++) {
        const char *st = statements.data[si];
        Slice sl = slice_cstr(st);
        Slice first = slice_first_word(sl, NULL);
        char *fw = slice_to_lower(first);
        bool fail = false;

        if (strcmp(fw, "participant") == 0 || strcmp(fw, "actor") == 0) {
            Slice rest = slice_trim((Slice){ sl.p + first.len, sl.len - first.len });
            if (slice_is_empty(rest)) fail = true;
            else {
                Slice id, labelraw;
                char *label = NULL;
                if (slice_split_once_cstr(rest, " as ", &id, &labelraw)) {
                    id = slice_trim(id);
                    char *lc = slice_dup(labelraw);
                    label = clean_label(lc);
                    free(lc);
                } else {
                    id = rest;
                }
                char *idc = slice_dup(id);
                int r = sequence_participant(s, idc, label);
                free(idc); free(label);
                fail = r < 0;
            }
        } else if (strcmp(fw, "autonumber") == 0) {
            autonumber = true;
        } else if (strcmp(fw, "activate") == 0 || strcmp(fw, "deactivate") == 0 || strcmp(fw, "create") == 0 ||
                   strcmp(fw, "destroy") == 0 || strcmp(fw, "title") == 0 || strcmp(fw, "acctitle") == 0 ||
                   strcmp(fw, "accdescr") == 0 || strcmp(fw, "links") == 0 || strcmp(fw, "link") == 0 ||
                   strcmp(fw, "properties") == 0) {
            /* no-op */
        } else if (strcmp(fw, "note") == 0) {
            Slice rest = slice_trim((Slice){ sl.p + first.len, sl.len - first.len });
            char *restc = slice_dup(rest);
            char *text; NoteAnchorKind kind; int a, b;
            bool ok = parse_note_anchor(restc, s, &text, &kind, &a, &b);
            free(restc);
            if (!ok) fail = true;
            else if (s->n_items >= MAX_EDGES) fail = true;
            else {
                SeqItem *it = push_item(s);
                it->kind = SEQ_NOTE; it->text = text; it->anchor_kind = kind; it->anchor_a = a; it->anchor_b = b;
            }
        } else if (strcmp(fw, "loop") == 0 || strcmp(fw, "alt") == 0 || strcmp(fw, "opt") == 0 ||
                   strcmp(fw, "par") == 0 || strcmp(fw, "critical") == 0 || strcmp(fw, "break") == 0 ||
                   strcmp(fw, "else") == 0 || strcmp(fw, "and") == 0 || strcmp(fw, "option") == 0) {
            bool is_cont = strcmp(fw, "else") == 0 || strcmp(fw, "and") == 0 || strcmp(fw, "option") == 0;
            bool skip = false;
            if (is_cont) {
                if (!(blocks.n > 0 && blocks.data[blocks.n - 1] == 1)) skip = true;
            } else {
                intvec_push(&blocks, 1);
            }
            if (!skip) {
                if (s->n_items >= MAX_EDGES) fail = true;
                else {
                    SeqItem *it = push_item(s);
                    it->kind = SEQ_DIVIDER;
                    it->text = decode_html_entities(st);
                }
            }
        } else if (strcmp(fw, "rect") == 0 || strcmp(fw, "box") == 0) {
            intvec_push(&blocks, 0);
        } else if (strcmp(fw, "end") == 0) {
            if (blocks.n > 0) {
                int top = blocks.data[--blocks.n];
                if (top == 1) {
                    if (s->n_items >= MAX_EDGES) fail = true;
                    else {
                        SeqItem *it = push_item(s);
                        it->kind = SEQ_DIVIDER;
                        it->text = cstr_dup("end");
                    }
                }
            }
        } else {
            int from, to; char *text; bool dashed; SeqHead head;
            if (!parse_seq_message(st, s, &from, &to, &text, &dashed, &head)) fail = true;
            else {
                if (autonumber) {
                    msg_count++;
                    Buf b; buf_init(&b);
                    char num[32]; snprintf(num, sizeof(num), "%zu.", msg_count);
                    buf_push_cstr(&b, num);
                    if (text) { buf_push_char(&b, ' '); buf_push_cstr(&b, text); free(text); }
                    text = buf_take(&b);
                }
                if (s->n_items >= MAX_EDGES) { free(text); fail = true; }
                else {
                    SeqItem *it = push_item(s);
                    it->kind = SEQ_MESSAGE; it->from = from; it->to = to; it->text = text;
                    it->dashed = dashed; it->head = head;
                }
            }
        }
        free(fw);
        if (fail) { intvec_free(&blocks); strvec_free(&statements); sequence_free(s); return NULL; }
    }

    intvec_free(&blocks);
    strvec_free(&statements);
    if (s->n_labels == 0) { sequence_free(s); return NULL; }
    return s;
}
