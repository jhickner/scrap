#include <stdlib.h>
#include <string.h>

#include "canvas.h"
#include "dwidth.h"
#include "strutil.h"

typedef struct {
    size_t *box_w, *box_h, *lay_w, *lay_h, *extra_h, *self_label_w;
} NodeSizes;

typedef struct {
    size_t canvas_w, canvas_h;
    size_t *band_end;
    size_t *edge_bus;
    size_t lane_base;
    size_t *edge_lane;
} RoutePlan;

typedef struct { size_t a, b, f, t, idx; } Span;

static int cmp_span(const void *pa, const void *pb) {
    const Span *a = pa, *b = pb;
    if (a->a != b->a) return a->a < b->a ? -1 : 1;
    if (a->b != b->b) return a->b < b->b ? -1 : 1;
    if (a->f != b->f) return a->f < b->f ? -1 : 1;
    if (a->t != b->t) return a->t < b->t ? -1 : 1;
    if (a->idx != b->idx) return a->idx < b->idx ? -1 : 1;
    return 0;
}

typedef struct { Span *items; size_t n; } SpanVec;

static void assign_tracks(SpanVec spans, size_t *out_slot /* len n_edges, indexed by idx */, size_t *count_out) {
    Span *sorted = malloc(sizeof(Span) * (spans.n ? spans.n : 1));
    memcpy(sorted, spans.items, sizeof(Span) * spans.n);
    qsort(sorted, spans.n, sizeof(Span), cmp_span);

    typedef struct { Span *items; size_t n, cap; } Track;
    Track *tracks = NULL;
    size_t n_tracks = 0, cap_tracks = 0;

    for (size_t i = 0; i < spans.n; i++) {
        Span s = sorted[i];
        long slot = -1;
        for (size_t ti = 0; ti < n_tracks; ti++) {
            bool compatible = true;
            for (size_t k = 0; k < tracks[ti].n; k++) {
                Span m = tracks[ti].items[k];
                bool ok = (m.b + 2 <= s.a) || (s.b + 2 <= m.a) || (m.f == s.f) || (m.t == s.t);
                if (!ok) { compatible = false; break; }
            }
            if (compatible) { slot = (long)ti; break; }
        }
        if (slot < 0) {
            if (n_tracks == cap_tracks) {
                cap_tracks = cap_tracks ? cap_tracks * 2 : 4;
                tracks = realloc(tracks, cap_tracks * sizeof(Track));
                for (size_t z = n_tracks; z < cap_tracks; z++) { tracks[z].items = NULL; tracks[z].n = 0; tracks[z].cap = 0; }
            }
            slot = (long)n_tracks++;
        }
        Track *t = &tracks[slot];
        if (t->n == t->cap) { t->cap = t->cap ? t->cap * 2 : 4; t->items = realloc(t->items, t->cap * sizeof(Span)); }
        t->items[t->n++] = s;
        out_slot[s.idx] = (size_t)slot;
    }
    *count_out = n_tracks;
    for (size_t ti = 0; ti < n_tracks; ti++) free(tracks[ti].items);
    free(tracks);
    free(sorted);
}

static SpanVec bus_spans_td(const Graph *g, const size_t *ranks, const size_t *centers, size_t r, bool exact) {
    SpanVec v = { NULL, 0 };
    size_t cap = 0;
    for (size_t i = 0; i < g->n_edges; i++) {
        const Edge *e = &g->edges[i];
        if (e->from == e->to || ranks[e->from] != r || ranks[e->to] != r + 1) continue;
        bool jogs = exact ? (centers[e->from] != centers[e->to])
                           : ((centers[e->from] > centers[e->to] ? centers[e->from] - centers[e->to] : centers[e->to] - centers[e->from]) > 1);
        if (!jogs) continue;
        if (v.n == cap) { cap = cap ? cap * 2 : 8; v.items = realloc(v.items, cap * sizeof(Span)); }
        size_t a = centers[e->from] < centers[e->to] ? centers[e->from] : centers[e->to];
        size_t b = centers[e->from] < centers[e->to] ? centers[e->to] : centers[e->from];
        v.items[v.n++] = (Span){ a, b, (size_t)e->from, (size_t)e->to, i };
    }
    return v;
}

static SpanVec lane_spans(const Graph *g, const size_t *ranks, const Placed *placed, bool vertical) {
    SpanVec v = { NULL, 0 };
    size_t cap = 0;
    for (size_t i = 0; i < g->n_edges; i++) {
        const Edge *e = &g->edges[i];
        if (e->from == e->to || ranks[e->to] == ranks[e->from] + 1) continue;
        const Placed *pf = &placed[e->from], *pt = &placed[e->to];
        size_t va = vertical ? pf->cy : pf->cx, vb = vertical ? pt->cy : pt->cx;
        size_t a = va < vb ? va : vb, b = va < vb ? vb : va;
        if (v.n == cap) { cap = cap ? cap * 2 : 8; v.items = realloc(v.items, cap * sizeof(Span)); }
        v.items[v.n++] = (Span){ a, b, (size_t)e->from, (size_t)e->to, i };
    }
    return v;
}

static RoutePlan place_td(const size_t *ranks, size_t max_rank, size_t **by_rank, const size_t *by_rank_n,
                           const NodeSizes *sizes, const Graph *g, Placed *placed) {
    size_t n = g->n_nodes;
    size_t *centers = assign_positions(by_rank, by_rank_n, max_rank + 1, sizes->lay_w, n, GAP_X, g, ranks);

    size_t *edge_bus = calloc(g->n_edges ? g->n_edges : 1, sizeof(size_t));
    size_t *bus_tracks = calloc(max_rank + 1, sizeof(size_t));
    for (size_t r = 0; r < max_rank; r++) {
        SpanVec spans = bus_spans_td(g, ranks, centers, r, false);
        if (spans.n == 0) { free(spans.items); continue; }
        size_t count;
        assign_tracks(spans, edge_bus, &count);
        bus_tracks[r] = count;
        free(spans.items);
    }

    size_t *rank_h = malloc(sizeof(size_t) * (max_rank + 1));
    for (size_t r = 0; r <= max_rank; r++) {
        size_t mx = 3;
        bool any = false;
        for (size_t k = 0; k < by_rank_n[r]; k++) {
            size_t i = by_rank[r][k];
            size_t v = sizes->box_h[i] + sizes->extra_h[i];
            if (!any || v > mx) { mx = v; any = true; }
        }
        rank_h[r] = mx;
    }
    size_t *rank_y = calloc(max_rank + 1, sizeof(size_t));
    for (size_t r = 1; r <= max_rank; r++) {
        size_t gap = GAP_Y > bus_tracks[r - 1] + 1 ? GAP_Y : bus_tracks[r - 1] + 1;
        rank_y[r] = rank_y[r - 1] + rank_h[r - 1] + gap;
    }
    size_t canvas_h = rank_y[max_rank] + rank_h[max_rank];
    size_t *band_end = malloc(sizeof(size_t) * (max_rank + 1));
    for (size_t r = 0; r <= max_rank; r++) band_end[r] = rank_y[r] + rank_h[r];

    size_t diagram_w = 1;
    for (size_t r = 0; r <= max_rank; r++) {
        for (size_t k = 0; k < by_rank_n[r]; k++) {
            size_t idx = by_rank[r][k];
            size_t w = sizes->box_w[idx], h = sizes->box_h[idx];
            size_t cx = centers[idx];
            size_t x = cx > w / 2 ? cx - w / 2 : 0;
            size_t y = rank_y[r] + (rank_h[r] - h - sizes->extra_h[idx]) / 2;
            placed[idx] = (Placed){ x, y, w, h, cx, y + h / 2, r };
            if (x + w > diagram_w) diagram_w = x + w;
            if (sizes->extra_h[idx] > 0 && sizes->self_label_w[idx] > 0) {
                size_t need = x + w + 2 + sizes->self_label_w[idx];
                if (need > diagram_w) diagram_w = need;
            }
        }
    }

    size_t content_w = diagram_w;
    for (size_t i = 0; i < g->n_edges; i++) {
        const Edge *e = &g->edges[i];
        if (e->from == e->to || !e->label) continue;
        size_t lw = dwidth_str(e->label);
        if (lw > MAX_LABEL) lw = MAX_LABEL;
        if (ranks[e->to] == ranks[e->from] + 1) {
            size_t need = placed[e->to].cx + 2 + lw;
            if (need > content_w) content_w = need;
        } else {
            size_t need = diagram_w + lw + 1;
            if (need > content_w) content_w = need;
        }
    }

    size_t *edge_lane = calloc(g->n_edges ? g->n_edges : 1, sizeof(size_t));
    SpanVec lanes = lane_spans(g, ranks, placed, true);
    size_t canvas_w, lane_base;
    if (lanes.n == 0) { canvas_w = content_w; lane_base = 0; }
    else {
        size_t count;
        assign_tracks(lanes, edge_lane, &count);
        canvas_w = content_w + 1 + count;
        lane_base = content_w + 1;
    }
    free(lanes.items);

    free(centers); free(bus_tracks); free(rank_h); free(rank_y);

    RoutePlan plan = { canvas_w, canvas_h, band_end, edge_bus, lane_base, edge_lane };
    return plan;
}

static RoutePlan place_lr(const size_t *ranks, size_t max_rank, size_t **by_rank, const size_t *by_rank_n,
                           const NodeSizes *sizes, const Graph *g, Placed *placed) {
    size_t n = g->n_nodes;
    size_t *col_w = calloc(max_rank + 1, sizeof(size_t));
    for (size_t r = 0; r <= max_rank; r++) {
        size_t mx = 0;
        for (size_t k = 0; k < by_rank_n[r]; k++) {
            size_t v = sizes->box_w[by_rank[r][k]];
            if (v > mx) mx = v;
        }
        col_w[r] = mx;
    }

    size_t max_label = 0;
    for (size_t i = 0; i < g->n_edges; i++) {
        const Edge *e = &g->edges[i];
        if (!(e->from == e->to || ranks[e->to] == ranks[e->from] + 1) || !e->label) continue;
        size_t lw = dwidth_str(e->label);
        if (lw > MAX_LABEL) lw = MAX_LABEL;
        if (lw > max_label) max_label = lw;
    }
    size_t base_gap = GAP_X + 1;
    if (max_label + 3 > base_gap) base_gap = max_label + 3;

    size_t *centers = assign_positions(by_rank, by_rank_n, max_rank + 1, sizes->lay_h, n, 1, g, ranks);

    size_t *edge_bus = calloc(g->n_edges ? g->n_edges : 1, sizeof(size_t));
    size_t *bus_tracks = calloc(max_rank + 1, sizeof(size_t));
    for (size_t r = 0; r < max_rank; r++) {
        SpanVec spans = bus_spans_td(g, ranks, centers, r, true);
        if (spans.n == 0) { free(spans.items); continue; }
        size_t count;
        assign_tracks(spans, edge_bus, &count);
        bus_tracks[r] = count;
        free(spans.items);
    }

    size_t *rank_x = calloc(max_rank + 1, sizeof(size_t));
    for (size_t r = 1; r <= max_rank; r++) {
        size_t gap = base_gap > bus_tracks[r - 1] + 1 ? base_gap : bus_tracks[r - 1] + 1;
        rank_x[r] = rank_x[r - 1] + col_w[r - 1] + gap;
    }
    size_t last_extra = 0;
    for (size_t k = 0; k < by_rank_n[max_rank]; k++) {
        size_t i = by_rank[max_rank][k];
        if (sizes->extra_h[i] > 0 && sizes->self_label_w[i] > 0) {
            size_t v = 2 + sizes->self_label_w[i];
            if (v > last_extra) last_extra = v;
        }
    }
    size_t canvas_w = rank_x[max_rank] + col_w[max_rank] + last_extra;
    size_t *band_end = malloc(sizeof(size_t) * (max_rank + 1));
    for (size_t r = 0; r <= max_rank; r++) band_end[r] = rank_x[r] + col_w[r];

    size_t diagram_h = 1;
    for (size_t r = 0; r <= max_rank; r++) {
        size_t x = rank_x[r];
        for (size_t k = 0; k < by_rank_n[r]; k++) {
            size_t idx = by_rank[r][k];
            size_t w = sizes->box_w[idx], h = sizes->box_h[idx];
            size_t cy = centers[idx];
            size_t half = (h + sizes->extra_h[idx]) / 2;
            size_t y = cy > half ? cy - half : 0;
            placed[idx] = (Placed){ x, y, w, h, x + w / 2, y + h / 2, r };
            size_t v = y + h + sizes->extra_h[idx];
            if (v > diagram_h) diagram_h = v;
        }
    }

    size_t *edge_lane = calloc(g->n_edges ? g->n_edges : 1, sizeof(size_t));
    SpanVec lanes = lane_spans(g, ranks, placed, false);
    size_t canvas_h, lane_base;
    if (lanes.n == 0) { canvas_h = diagram_h; lane_base = 0; }
    else {
        size_t count;
        assign_tracks(lanes, edge_lane, &count);
        canvas_h = diagram_h + 1 + count;
        lane_base = diagram_h + 1;
    }
    free(lanes.items);

    free(centers); free(bus_tracks); free(col_w); free(rank_x);

    RoutePlan plan = { canvas_w, canvas_h, band_end, edge_bus, lane_base, edge_lane };
    return plan;
}

static void route_self(Canvas *c, const Placed *p, const Edge *e) {
    size_t bottom = p->y + p->h - 1;
    size_t exit_x = p->cx + 1;
    size_t ret_x = p->x + p->w - 2;
    if (ret_x <= exit_x || bottom + 2 >= c->h) return;
    uint32_t v, h, bl, br;
    if (e->line == LINE_DOTTED) { v = CP_V_DOT; h = CP_H_DOT; bl = CP_RBL; br = CP_RBR; }
    else if (e->line == LINE_THICK) { v = CP_V_THICK; h = CP_H_THICK; bl = CP_BL_THICK; br = CP_BR_THICK; }
    else { v = CP_V; h = CP_H; bl = CP_RBL; br = CP_RBR; }
    canvas_junction(c, exit_x, bottom, BIT_D);
    canvas_set(c, exit_x, bottom + 1, v, CLS_EDGE);
    canvas_set(c, exit_x, bottom + 2, bl, CLS_EDGE);
    for (size_t x = exit_x + 1; x < ret_x; x++) canvas_set(c, x, bottom + 2, h, CLS_EDGE);
    canvas_set(c, ret_x, bottom + 2, br, CLS_EDGE);
    canvas_set(c, ret_x, bottom + 1, head_glyph(e->head_to, CP_ARR_UP), CLS_EDGE);
    if (e->label) place_label(c, e->label, bottom + 1, p->x + p->w + 1);
}

static void route_forward(Canvas *c, const Placed *from, const Placed *to, const Edge *e, size_t bus) {
    size_t tx = to->cx;
    size_t diff = from->cx > tx ? from->cx - tx : tx - from->cx;
    size_t bx = diff <= 1 ? tx : from->cx;
    size_t by = from->y + from->h - 1;
    size_t head_row = to->y - 1;
    canvas_junction(c, bx, by, BIT_D);
    canvas_seg_v(c, bx, by, bus);
    if (bx == tx) canvas_seg_v(c, bx, bus, head_row);
    else { canvas_seg_h(c, bus, bx, tx); canvas_seg_v(c, tx, bus, head_row); }
    if (e->head_to == HEAD_NONE) canvas_add_bits(c, tx, head_row, BIT_U);
    else canvas_set(c, tx, head_row, head_glyph(e->head_to, CP_ARR_DOWN), CLS_EDGE);
    if (e->head_from != HEAD_NONE) canvas_set(c, bx, by, head_glyph(e->head_from, CP_ARR_UP), CLS_EDGE);
    if (e->label) place_label(c, e->label, head_row, tx + 1);
}

static void route_back(Canvas *c, const Placed *from, const Placed *to, const Edge *e, size_t lane_x) {
    size_t sx = from->x + from->w - 1, sy = from->cy;
    size_t tx = to->x + to->w - 1, tyc = to->cy;
    canvas_junction(c, sx, sy, BIT_R);
    canvas_seg_h(c, sy, sx, lane_x);
    canvas_seg_v(c, lane_x, sy, tyc);
    canvas_seg_h(c, tyc, tx + 1, lane_x);
    if (e->head_to == HEAD_NONE) canvas_add_bits(c, tx + 1, tyc, BIT_R);
    else canvas_set(c, tx + 1, tyc, head_glyph(e->head_to, CP_ARR_LEFT), CLS_EDGE);
    if (e->head_from != HEAD_NONE) canvas_set(c, sx, sy, head_glyph(e->head_from, CP_ARR_LEFT), CLS_EDGE);
    if (e->label) {
        size_t row = tyc > 0 ? tyc - 1 : 0;
        size_t lw = dwidth_str(e->label);
        size_t start = lane_x > lw + 1 ? lane_x - lw - 1 : 0;
        place_label(c, e->label, row, start);
    }
}

static void route_forward_lr(Canvas *c, const Placed *from, const Placed *to, const Edge *e, size_t bus) {
    size_t rx = from->x + from->w - 1, ry = from->cy;
    size_t ly = to->cy;
    size_t head_col = to->x - 1;
    canvas_junction(c, rx, ry, BIT_R);
    canvas_seg_h(c, ry, rx, bus);
    if (ry == ly) canvas_seg_h(c, ry, bus, head_col);
    else { canvas_seg_v(c, bus, ry, ly); canvas_seg_h(c, ly, bus, head_col); }
    if (e->head_to == HEAD_NONE) canvas_add_bits(c, head_col, ly, BIT_R);
    else canvas_set(c, head_col, ly, head_glyph(e->head_to, CP_ARR_RIGHT), CLS_EDGE);
    if (e->head_from != HEAD_NONE) canvas_set(c, rx, ry, head_glyph(e->head_from, CP_ARR_LEFT), CLS_EDGE);
    if (e->label) {
        size_t row = ly > 0 ? ly - 1 : 0;
        place_label(c, e->label, row, bus + 1);
    }
}

static void route_back_lr(Canvas *c, const Placed *from, const Placed *to, const Edge *e, size_t lane_y) {
    size_t sx = from->cx, sy = from->y + from->h - 1;
    size_t tx = to->cx, ty = to->y + to->h - 1;
    canvas_junction(c, sx, sy, BIT_D);
    canvas_seg_v(c, sx, sy, lane_y);
    canvas_seg_h(c, lane_y, sx, tx);
    canvas_seg_v(c, tx, lane_y, ty + 1);
    if (e->head_to == HEAD_NONE) canvas_add_bits(c, tx, ty + 1, BIT_D);
    else canvas_set(c, tx, ty + 1, head_glyph(e->head_to, CP_ARR_UP), CLS_EDGE);
    if (e->head_from != HEAD_NONE) canvas_set(c, sx, sy, head_glyph(e->head_from, CP_ARR_UP), CLS_EDGE);
    if (e->label) {
        size_t row = lane_y > 0 ? lane_y - 1 : 0;
        place_label(c, e->label, row, (sx + tx) / 2);
    }
}

Canvas *layout_canvas(const Graph *g, NodeExtra *extras, int max_width, Oversize *err) {
    *err = OVERSIZE_NONE;
    size_t n = g->n_nodes;
    if (n == 0) { *err = OVERSIZE_CELLS; return NULL; }

    size_t *ranks = compute_ranks(g);
    size_t max_rank = 0;
    for (size_t i = 0; i < n; i++) if (ranks[i] > max_rank) max_rank = ranks[i];

    size_t **by_rank = malloc(sizeof(size_t *) * (max_rank + 1));
    size_t *by_rank_n = calloc(max_rank + 1, sizeof(size_t));
    size_t *by_rank_cap = calloc(max_rank + 1, sizeof(size_t));
    for (size_t r = 0; r <= max_rank; r++) by_rank[r] = NULL;
    for (size_t i = 0; i < n; i++) {
        size_t r = ranks[i];
        if (by_rank_n[r] == by_rank_cap[r]) {
            by_rank_cap[r] = by_rank_cap[r] ? by_rank_cap[r] * 2 : 4;
            by_rank[r] = realloc(by_rank[r], by_rank_cap[r] * sizeof(size_t));
        }
        by_rank[r][by_rank_n[r]++] = i;
    }
    order_ranks(by_rank, by_rank_n, max_rank + 1, g, ranks);

    char ***wrapped = malloc(sizeof(char **) * n);
    size_t *wrapped_n = malloc(sizeof(size_t) * n);
    for (size_t i = 0; i < n; i++) {
        if (extras[i].kind == EXTRA_PLAIN)
            wrapped[i] = wrap_label(g->nodes[i].label, WRAP_WIDTH, MAX_LINES, &wrapped_n[i]);
        else { wrapped[i] = NULL; wrapped_n[i] = 0; }
    }

    size_t *box_w = malloc(sizeof(size_t) * n);
    size_t *box_h = malloc(sizeof(size_t) * n);
    for (size_t i = 0; i < n; i++) {
        NodeExtra *ex = &extras[i];
        if (ex->kind == EXTRA_FRAME) {
            char *t = fit_label(g->nodes[i].label, WRAP_WIDTH);
            size_t title_w = dwidth_str(t);
            free(t);
            size_t a = ex->frame->w + 2;
            size_t b = title_w + 4;
            box_w[i] = a > b ? a : b;
            box_h[i] = ex->frame->h + 2;
        } else if (ex->kind == EXTRA_COMPARTMENTS) {
            size_t mx = 1;
            size_t filled = 0, total = 0;
            for (size_t s = 0; s < ex->n_sections; s++) {
                if (ex->section_n[s] > 0) filled++;
                total += ex->section_n[s];
                for (size_t li = 0; li < ex->section_n[s]; li++) {
                    size_t w = dwidth_str(ex->sections[s][li]);
                    if (w > mx) mx = w;
                }
            }
            box_w[i] = mx + 2 * PAD + 2;
            box_h[i] = total + (filled > 0 ? filled - 1 : 0) + 2;
        } else {
            size_t mx = 1;
            for (size_t li = 0; li < wrapped_n[i]; li++) {
                size_t w = dwidth_str(wrapped[i][li]);
                if (w > mx) mx = w;
            }
            box_w[i] = mx + 2 * PAD + 2;
            box_h[i] = wrapped_n[i] + 2;
        }
    }

    size_t *extra_h = calloc(n ? n : 1, sizeof(size_t));
    size_t *self_label_w = calloc(n ? n : 1, sizeof(size_t));
    for (size_t i = 0; i < g->n_edges; i++) {
        const Edge *e = &g->edges[i];
        if (e->from != e->to) continue;
        extra_h[e->from] = 2;
        if (e->label) {
            size_t lw = dwidth_str(e->label);
            if (lw > MAX_LABEL) lw = MAX_LABEL;
            if (lw > self_label_w[e->from]) self_label_w[e->from] = lw;
        }
    }
    for (size_t i = 0; i < n; i++) if (extra_h[i] > 0 && box_w[i] < 7) box_w[i] = 7;

    size_t *lay_w = malloc(sizeof(size_t) * n);
    size_t *lay_h = malloc(sizeof(size_t) * n);
    for (size_t i = 0; i < n; i++) {
        lay_w[i] = box_w[i] + (self_label_w[i] > 0 ? 2 * (self_label_w[i] + 3) : 0);
        lay_h[i] = box_h[i] + extra_h[i];
    }
    NodeSizes sizes = { box_w, box_h, lay_w, lay_h, extra_h, self_label_w };

    Placed *placed = calloc(n ? n : 1, sizeof(Placed));
    bool vertical = g->dir == DIR_DOWN || g->dir == DIR_UP;
    RoutePlan plan = vertical
        ? place_td(ranks, max_rank, by_rank, by_rank_n, &sizes, g, placed)
        : place_lr(ranks, max_rank, by_rank, by_rank_n, &sizes, g, placed);

    if (max_width > 0 && (size_t)max_width < plan.canvas_w) {
        *err = OVERSIZE_WIDTH;
        free(ranks);
        for (size_t r = 0; r <= max_rank; r++) free(by_rank[r]);
        free(by_rank); free(by_rank_n); free(by_rank_cap);
        for (size_t i = 0; i < n; i++) { for (size_t li = 0; li < wrapped_n[i]; li++) free(wrapped[i][li]); free(wrapped[i]); }
        free(wrapped); free(wrapped_n);
        free(box_w); free(box_h); free(lay_w); free(lay_h); free(extra_h); free(self_label_w);
        free(placed); free(plan.band_end); free(plan.edge_bus); free(plan.edge_lane);
        return NULL;
    }
    if ((unsigned long long)plan.canvas_w * (unsigned long long)plan.canvas_h > MAX_CANVAS_CELLS) {
        *err = OVERSIZE_CELLS;
        free(ranks);
        for (size_t r = 0; r <= max_rank; r++) free(by_rank[r]);
        free(by_rank); free(by_rank_n); free(by_rank_cap);
        for (size_t i = 0; i < n; i++) { for (size_t li = 0; li < wrapped_n[i]; li++) free(wrapped[i][li]); free(wrapped[i]); }
        free(wrapped); free(wrapped_n);
        free(box_w); free(box_h); free(lay_w); free(lay_h); free(extra_h); free(self_label_w);
        free(placed); free(plan.band_end); free(plan.edge_bus); free(plan.edge_lane);
        return NULL;
    }

    Canvas *canvas = canvas_new(plan.canvas_w, plan.canvas_h);
    for (size_t i = 0; i < n; i++) {
        NodeExtra *ex = &extras[i];
        if (ex->kind == EXTRA_FRAME) draw_frame(canvas, &placed[i], g->nodes[i].label, ex->frame);
        else if (ex->kind == EXTRA_COMPARTMENTS) draw_class_box(canvas, &placed[i], ex->sections, ex->section_n, ex->n_sections);
        else draw_box(canvas, &placed[i], wrapped[i], wrapped_n[i], g->nodes[i].shape);
    }
    for (size_t i = 0; i < g->n_edges; i++) {
        const Edge *e = &g->edges[i];
        canvas->cur_style = e->line == LINE_SOLID ? STY_SOLID : e->line == LINE_DOTTED ? STY_DOT : STY_THICK;
        if (e->from == e->to) { route_self(canvas, &placed[e->from], e); continue; }
        const Placed *from = &placed[e->from], *to = &placed[e->to];
        bool adjacent = to->rank == from->rank + 1;
        size_t bus = plan.band_end[from->rank] + plan.edge_bus[i];
        size_t lane = plan.lane_base + plan.edge_lane[i];
        if (vertical && adjacent) route_forward(canvas, from, to, e, bus);
        else if (vertical && !adjacent) route_back(canvas, from, to, e, lane);
        else if (!vertical && adjacent) route_forward_lr(canvas, from, to, e, bus);
        else route_back_lr(canvas, from, to, e, lane);
    }
    canvas_finalize_mask(canvas);

    free(ranks);
    for (size_t r = 0; r <= max_rank; r++) free(by_rank[r]);
    free(by_rank); free(by_rank_n); free(by_rank_cap);
    for (size_t i = 0; i < n; i++) { for (size_t li = 0; li < wrapped_n[i]; li++) free(wrapped[i][li]); free(wrapped[i]); }
    free(wrapped); free(wrapped_n);
    free(box_w); free(box_h); free(lay_w); free(lay_h); free(extra_h); free(self_label_w);
    free(placed); free(plan.band_end); free(plan.edge_bus); free(plan.edge_lane);
    return canvas;
}
