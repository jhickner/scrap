#include <stdlib.h>
#include <string.h>

#include "canvas.h"
#include "dwidth.h"
#include "sequence.h"
#include "strutil.h"

#define SEQ_GAP 5

static size_t item_text_w(const char *text) { return text ? dwidth_str(text) : 0; }

static size_t div_ceil(size_t a, size_t b) { return (a + b - 1) / b; }

typedef struct { size_t l, r, need; } Req;

static void note_geometry(const size_t *xs, const SeqItem *it, size_t text_w, size_t *x_out, size_t *w_out) {
    if (it->anchor_kind == ANCHOR_OVER) {
        size_t l = (size_t)it->anchor_a, r = (size_t)it->anchor_b;
        size_t center = (xs[l] + xs[r]) / 2;
        size_t w1 = (xs[r] - xs[l]) + 5;
        size_t w2 = text_w + 2 * PAD + 2;
        size_t w = w1 > w2 ? w1 : w2;
        *x_out = center > w / 2 ? center - w / 2 : 0;
        *w_out = w;
    } else if (it->anchor_kind == ANCHOR_LEFT) {
        size_t i = (size_t)it->anchor_a;
        size_t w = text_w + 2 * PAD + 2;
        size_t sub = 2 + w - 1;
        *x_out = xs[i] > sub ? xs[i] - sub : 0;
        *w_out = w;
    } else {
        size_t i = (size_t)it->anchor_a;
        *x_out = xs[i] + 2;
        *w_out = text_w + 2 * PAD + 2;
    }
}

MermaidArt *layout_sequence(const Sequence *seq, int max_width, Oversize *err) {
    *err = OVERSIZE_NONE;
    size_t n = seq->n_labels;
    char **labels = malloc(sizeof(char *) * n);
    size_t *box_w = malloc(sizeof(size_t) * n);
    for (size_t i = 0; i < n; i++) {
        labels[i] = fit_label(seq->labels[i], WRAP_WIDTH);
        size_t w = dwidth_str(labels[i]);
        if (w < 1) w = 1;
        box_w[i] = w + 2 * PAD + 2;
    }
    size_t box_h = 3;

    size_t *gaps = malloc(sizeof(size_t) * (n > 1 ? n - 1 : 1));
    for (size_t i = 0; i + 1 < n; i++) {
        size_t v = div_ceil(box_w[i], 2) + div_ceil(box_w[i + 1], 2) + 1;
        gaps[i] = SEQ_GAP > v ? SEQ_GAP : v;
    }

    Req *reqs = malloc(sizeof(Req) * (seq->n_items * 2 + 1));
    size_t n_reqs = 0;
    for (size_t ii = 0; ii < seq->n_items; ii++) {
        const SeqItem *it = &seq->items[ii];
        if (it->kind == SEQ_MESSAGE) {
            size_t tw = item_text_w(it->text);
            if ((size_t)it->from != (size_t)it->to) {
                size_t l = (size_t)it->from < (size_t)it->to ? (size_t)it->from : (size_t)it->to;
                size_t r = (size_t)it->from < (size_t)it->to ? (size_t)it->to : (size_t)it->from;
                size_t need = tw + 2 > 4 ? tw + 2 : 4;
                reqs[n_reqs++] = (Req){ l, r, need };
            } else if ((size_t)it->from + 1 < n) {
                reqs[n_reqs++] = (Req){ (size_t)it->from, (size_t)it->from + 1, 5 + tw + 2 };
            }
        } else if (it->kind == SEQ_NOTE) {
            size_t tw = dwidth_str(it->text);
            if (it->anchor_kind == ANCHOR_OVER && it->anchor_a != it->anchor_b) {
                reqs[n_reqs++] = (Req){ (size_t)it->anchor_a, (size_t)it->anchor_b, tw > 0 ? tw - 1 : 0 };
            } else if (it->anchor_kind == ANCHOR_OVER) {
                size_t i = (size_t)it->anchor_a;
                size_t half = div_ceil(tw + 4, 2) + 2;
                if (i > 0) reqs[n_reqs++] = (Req){ i - 1, i, half };
                if (i + 1 < n) reqs[n_reqs++] = (Req){ i, i + 1, half };
            } else if (it->anchor_kind == ANCHOR_LEFT) {
                size_t i = (size_t)it->anchor_a;
                if (i > 0) reqs[n_reqs++] = (Req){ i - 1, i, tw + 7 };
            } else {
                size_t i = (size_t)it->anchor_a;
                if (i + 1 < n) reqs[n_reqs++] = (Req){ i, i + 1, tw + 7 };
            }
        }
    }
    for (size_t i = 1; i < n_reqs; i++) {
        Req key = reqs[i];
        size_t kw = key.r - key.l;
        long j = (long)i - 1;
        while (j >= 0 && (reqs[j].r - reqs[j].l) > kw) { reqs[j + 1] = reqs[j]; j--; }
        reqs[j + 1] = key;
    }
    for (size_t i = 0; i < n_reqs; i++) {
        size_t cur = 0;
        for (size_t k = reqs[i].l; k < reqs[i].r; k++) cur += gaps[k];
        if (cur < reqs[i].need) gaps[reqs[i].r - 1] += reqs[i].need - cur;
    }
    free(reqs);

    size_t *xs = malloc(sizeof(size_t) * (n ? n : 1));
    if (n > 0) {
        xs[0] = box_w[0] / 2;
        for (size_t i = 1; i < n; i++) xs[i] = xs[i - 1] + gaps[i - 1];
    }

    size_t canvas_w = n > 0 ? xs[n - 1] + div_ceil(box_w[n - 1], 2) + 1 : 1;
    for (size_t ii = 0; ii < seq->n_items; ii++) {
        const SeqItem *it = &seq->items[ii];
        if (it->kind == SEQ_MESSAGE && it->from == it->to) {
            size_t need = xs[it->from] + 5 + item_text_w(it->text) + 1;
            if (need > canvas_w) canvas_w = need;
        } else if (it->kind == SEQ_NOTE) {
            size_t x, w;
            note_geometry(xs, it, dwidth_str(it->text), &x, &w);
            if (x + w + 1 > canvas_w) canvas_w = x + w + 1;
        } else if (it->kind == SEQ_DIVIDER) {
            size_t need = dwidth_str(it->text) + 4;
            if (need > canvas_w) canvas_w = need;
        }
    }

    size_t *rows = malloc(sizeof(size_t) * (seq->n_items ? seq->n_items : 1));
    size_t y = box_h + 1;
    for (size_t ii = 0; ii < seq->n_items; ii++) {
        rows[ii] = y;
        const SeqItem *it = &seq->items[ii];
        if (it->kind == SEQ_MESSAGE) y += it->from == it->to ? 4 : (it->text ? 3 : 2);
        else if (it->kind == SEQ_NOTE) y += 4;
        else y += 2;
    }
    size_t bottom_top = y;
    size_t canvas_h = bottom_top + box_h;

    if (max_width > 0 && (size_t)max_width < canvas_w) {
        *err = OVERSIZE_WIDTH;
        free(labels); free(box_w); free(gaps); free(xs); free(rows);
        return NULL;
    }
    if ((unsigned long long)canvas_w * (unsigned long long)canvas_h > MAX_CANVAS_CELLS) {
        *err = OVERSIZE_CELLS;
        free(labels); free(box_w); free(gaps); free(xs); free(rows);
        return NULL;
    }

    Canvas *canvas = canvas_new(canvas_w, canvas_h);
    for (size_t i = 0; i < n; i++) {
        size_t bys[2] = { 0, bottom_top };
        for (int k = 0; k < 2; k++) {
            Placed p = { xs[i] > box_w[i] / 2 ? xs[i] - box_w[i] / 2 : 0, bys[k], box_w[i], box_h, xs[i], bys[k] + 1, 0 };
            draw_box(canvas, &p, &labels[i], 1, SHAPE_RECT);
        }
    }
    for (size_t ii = 0; ii < seq->n_items; ii++) {
        const SeqItem *it = &seq->items[ii];
        if (it->kind != SEQ_NOTE) continue;
        size_t x, w;
        note_geometry(xs, it, dwidth_str(it->text), &x, &w);
        Placed p = { x, rows[ii], w, 3, x + w / 2, rows[ii] + 1, 0 };
        draw_box(canvas, &p, (char **)&it->text, 1, SHAPE_RECT);
    }
    for (size_t i = 0; i < n; i++) {
        canvas_junction(canvas, xs[i], box_h - 1, BIT_D);
        canvas_seg_v(canvas, xs[i], box_h, bottom_top - 1);
        canvas_junction(canvas, xs[i], bottom_top, BIT_U);
    }

    for (size_t ii = 0; ii < seq->n_items; ii++) {
        const SeqItem *it = &seq->items[ii];
        size_t r = rows[ii];
        if (it->kind == SEQ_MESSAGE) {
            uint32_t line_ch = it->dashed ? CP_H_DOT : CP_H;
            if (it->from == it->to) {
                size_t x = xs[it->from];
                canvas_junction(canvas, x, r, BIT_R);
                canvas_set(canvas, x + 1, r, line_ch, CLS_EDGE);
                canvas_set(canvas, x + 2, r, line_ch, CLS_EDGE);
                canvas_set(canvas, x + 3, r, CP_RTR, CLS_EDGE);
                canvas_set(canvas, x + 3, r + 1, CP_V, CLS_EDGE);
                canvas_set(canvas, x + 1, r + 2, it->head == SEQHEAD_CROSS ? CP_CROSSX : CP_ARR_LEFT, CLS_EDGE);
                canvas_set(canvas, x + 2, r + 2, line_ch, CLS_EDGE);
                canvas_set(canvas, x + 3, r + 2, CP_RBR, CLS_EDGE);
                if (it->text) draw_seq_text(canvas, it->text, x + 5, r + 1, CLS_TEXT);
            } else {
                size_t x0 = xs[it->from], x1 = xs[it->to];
                bool rightward = x1 > x0;
                size_t arrow_row = it->text ? r + 1 : r;
                size_t lo = x0 < x1 ? x0 : x1, hi = x0 < x1 ? x1 : x0;
                canvas_junction(canvas, x0, arrow_row, rightward ? BIT_R : BIT_L);
                for (size_t x = lo + 1; x < hi; x++) canvas_set(canvas, x, arrow_row, line_ch, CLS_EDGE);
                uint32_t head_ch = it->head == SEQHEAD_CROSS ? CP_CROSSX : (rightward ? CP_ARR_RIGHT : CP_ARR_LEFT);
                size_t head_x = rightward ? x1 - 1 : x1 + 1;
                canvas_set(canvas, head_x, arrow_row, head_ch, CLS_EDGE);
                if (it->text) {
                    size_t span = hi - lo - 1;
                    char *t = fit_label(it->text, span > 0 ? span : 1);
                    size_t tw = dwidth_str(t);
                    size_t tx = lo + 1 + (span > tw ? (span - tw) / 2 : 0);
                    draw_seq_text(canvas, t, tx, r, CLS_TEXT);
                    free(t);
                }
            }
        } else if (it->kind == SEQ_DIVIDER) {
            for (size_t x = 0; x < canvas_w; x++) canvas_set(canvas, x, r, CP_H, CLS_EDGE);
            size_t avail = canvas_w > 4 ? canvas_w - 4 : 0;
            char *t = fit_label(it->text, avail);
            Buf b; buf_init(&b);
            buf_push_char(&b, ' '); buf_push_cstr(&b, t); buf_push_char(&b, ' ');
            free(t);
            char *padded = buf_take(&b);
            draw_seq_text(canvas, padded, 2, r, CLS_EDGE_LABEL);
            free(padded);
        }
    }

    canvas_finalize_mask(canvas);
    MermaidArt *art = mermaid_art_from_canvas(canvas);
    canvas_free(canvas);

    for (size_t i = 0; i < n; i++) free(labels[i]);
    free(labels); free(box_w); free(gaps); free(xs); free(rows);
    return art;
}
