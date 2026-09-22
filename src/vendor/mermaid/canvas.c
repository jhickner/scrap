#include <stdlib.h>
#include <string.h>

#include "canvas.h"
#include "dwidth.h"
#include "strutil.h"

Canvas *canvas_new(size_t w, size_t h) {
    Canvas *c = malloc(sizeof(Canvas));
    c->w = w; c->h = h;
    size_t n = w * h;
    c->ch = malloc(n * sizeof(uint32_t));
    for (size_t i = 0; i < n; i++) c->ch[i] = CP_SPACE;
    c->cls = calloc(n ? n : 1, 1);
    c->mask = calloc(n ? n : 1, 1);
    c->style = calloc(n ? n : 1, 1);
    c->occupied = calloc(n ? n : 1, sizeof(bool));
    c->cur_style = STY_SOLID;
    return c;
}

void canvas_free(Canvas *c) {
    if (!c) return;
    free(c->ch); free(c->cls); free(c->mask); free(c->style); free(c->occupied);
    free(c);
}

size_t canvas_idx(const Canvas *c, size_t x, size_t y) { return y * c->w + x; }

void canvas_set(Canvas *c, size_t x, size_t y, uint32_t ch, Cls cls) {
    if (x >= c->w || y >= c->h) return;
    size_t i = canvas_idx(c, x, y);
    c->ch[i] = ch;
    c->cls[i] = (unsigned char)cls;
}

void canvas_add_bits(Canvas *c, size_t x, size_t y, unsigned bits) {
    if (x >= c->w || y >= c->h) return;
    size_t i = canvas_idx(c, x, y);
    if (c->occupied[i]) return;
    c->mask[i] |= (unsigned char)bits;
    c->style[i] |= c->cur_style;
    if (c->cls[i] != CLS_BORDER) c->cls[i] = CLS_EDGE;
}

void canvas_blit(Canvas *c, const Canvas *sub, size_t ox, size_t oy) {
    for (size_t sy = 0; sy < sub->h; sy++) {
        for (size_t sx = 0; sx < sub->w; sx++) {
            size_t x = ox + sx, y = oy + sy;
            if (x >= c->w || y >= c->h) continue;
            size_t si = canvas_idx(sub, sx, sy);
            size_t di = canvas_idx(c, x, y);
            c->ch[di] = sub->ch[si];
            c->cls[di] = sub->cls[si];
            c->style[di] = sub->style[si];
            c->occupied[di] = true;
        }
    }
}

void canvas_junction(Canvas *c, size_t x, size_t y, unsigned bits) {
    if (x >= c->w || y >= c->h) return;
    size_t i = canvas_idx(c, x, y);
    c->mask[i] |= (unsigned char)bits;
    if (c->cls[i] != CLS_BORDER) c->cls[i] = CLS_EDGE;
}

void canvas_seg_v(Canvas *c, size_t x, size_t y0, size_t y1) {
    size_t a = y0 < y1 ? y0 : y1;
    size_t b = y0 < y1 ? y1 : y0;
    for (size_t y = a; y <= b; y++) {
        unsigned bits = 0;
        if (y > a) bits |= BIT_U;
        if (y < b) bits |= BIT_D;
        canvas_add_bits(c, x, y, bits);
    }
}

void canvas_seg_h(Canvas *c, size_t y, size_t x0, size_t x1) {
    size_t a = x0 < x1 ? x0 : x1;
    size_t b = x0 < x1 ? x1 : x0;
    for (size_t x = a; x <= b; x++) {
        unsigned bits = 0;
        if (x > a) bits |= BIT_L;
        if (x < b) bits |= BIT_R;
        canvas_add_bits(c, x, y, bits);
    }
}

static uint32_t mask_char(unsigned m) {
    if (m == 0) return CP_SPACE;
    if (m == BIT_U || m == BIT_D || m == (BIT_U | BIT_D)) return CP_V;
    if (m == BIT_L || m == BIT_R || m == (BIT_L | BIT_R)) return CP_H;
    if (m == (BIT_D | BIT_R)) return CP_TL;
    if (m == (BIT_D | BIT_L)) return CP_TR;
    if (m == (BIT_U | BIT_R)) return CP_BL;
    if (m == (BIT_U | BIT_L)) return CP_BR;
    if (m == (BIT_U | BIT_D | BIT_R)) return CP_TEE_L;
    if (m == (BIT_U | BIT_D | BIT_L)) return CP_TEE_R;
    if (m == (BIT_D | BIT_L | BIT_R)) return CP_TEE_D;
    if (m == (BIT_U | BIT_L | BIT_R)) return CP_TEE_U;
    return CP_CROSS;
}

static uint32_t dotted_char(uint32_t c) {
    if (c == CP_H) return CP_H_DOT;
    if (c == CP_V) return CP_V_DOT;
    return c;
}

static uint32_t thick_char(uint32_t c) {
    switch (c) {
        case CP_H: return CP_H_THICK;
        case CP_V: return CP_V_THICK;
        case CP_TL: return CP_TL_THICK;
        case CP_TR: return CP_TR_THICK;
        case CP_BL: return CP_BL_THICK;
        case CP_BR: return CP_BR_THICK;
        case CP_TEE_L: return CP_TEE_L_THICK;
        case CP_TEE_R: return CP_TEE_R_THICK;
        case CP_TEE_D: return CP_TEE_D_THICK;
        case CP_TEE_U: return CP_TEE_U_THICK;
        case CP_CROSS: return CP_CROSS_THICK;
        default: return c;
    }
}

void canvas_finalize_mask(Canvas *c) {
    size_t n = c->w * c->h;
    for (size_t i = 0; i < n; i++) {
        if (c->mask[i] != 0 && c->ch[i] == CP_SPACE) {
            uint32_t g = mask_char(c->mask[i]);
            if (c->style[i] == STY_DOT) c->ch[i] = dotted_char(g);
            else if (c->style[i] == STY_THICK) c->ch[i] = thick_char(g);
            else c->ch[i] = g;
        }
    }
}

static uint32_t flip_glyph_v(uint32_t c) {
    switch (c) {
        case CP_TL: return CP_BL; case CP_BL: return CP_TL;
        case CP_TR: return CP_BR; case CP_BR: return CP_TR;
        case CP_TL_THICK: return CP_BL_THICK; case CP_BL_THICK: return CP_TL_THICK;
        case CP_TR_THICK: return CP_BR_THICK; case CP_BR_THICK: return CP_TR_THICK;
        case CP_RTL: return CP_RBL; case CP_RBL: return CP_RTL;
        case CP_RTR: return CP_RBR; case CP_RBR: return CP_RTR;
        case CP_TEE_D: return CP_TEE_U; case CP_TEE_U: return CP_TEE_D;
        case CP_TEE_D_THICK: return CP_TEE_U_THICK; case CP_TEE_U_THICK: return CP_TEE_D_THICK;
        case CP_ARR_DOWN: return CP_ARR_UP; case CP_ARR_UP: return CP_ARR_DOWN;
        case CP_ARR_DOWN_OPEN: return CP_ARR_UP_OPEN; case CP_ARR_UP_OPEN: return CP_ARR_DOWN_OPEN;
        default: return c;
    }
}

static uint32_t flip_glyph_h(uint32_t c) {
    switch (c) {
        case CP_TL: return CP_TR; case CP_TR: return CP_TL;
        case CP_BL: return CP_BR; case CP_BR: return CP_BL;
        case CP_TL_THICK: return CP_TR_THICK; case CP_TR_THICK: return CP_TL_THICK;
        case CP_BL_THICK: return CP_BR_THICK; case CP_BR_THICK: return CP_BL_THICK;
        case CP_RTL: return CP_RTR; case CP_RTR: return CP_RTL;
        case CP_RBL: return CP_RBR; case CP_RBR: return CP_RBL;
        case CP_TEE_L: return CP_TEE_R; case CP_TEE_R: return CP_TEE_L;
        case CP_TEE_L_THICK: return CP_TEE_R_THICK; case CP_TEE_R_THICK: return CP_TEE_L_THICK;
        case CP_ARR_RIGHT: return CP_ARR_LEFT; case CP_ARR_LEFT: return CP_ARR_RIGHT;
        case CP_ARR_RIGHT_OPEN: return CP_ARR_LEFT_OPEN; case CP_ARR_LEFT_OPEN: return CP_ARR_RIGHT_OPEN;
        default: return c;
    }
}

void canvas_flip_vertical(Canvas *c) {
    for (size_t y = 0; y < c->h / 2; y++) {
        size_t y2 = c->h - 1 - y;
        for (size_t x = 0; x < c->w; x++) {
            size_t i = canvas_idx(c, x, y), j = canvas_idx(c, x, y2);
            uint32_t tc = c->ch[i]; c->ch[i] = c->ch[j]; c->ch[j] = tc;
            unsigned char tcl = c->cls[i]; c->cls[i] = c->cls[j]; c->cls[j] = tcl;
        }
    }
    size_t n = c->w * c->h;
    for (size_t i = 0; i < n; i++) c->ch[i] = flip_glyph_v(c->ch[i]);
}

void canvas_flip_horizontal(Canvas *c) {
    for (size_t y = 0; y < c->h; y++) {
        for (size_t x = 0; x < c->w / 2; x++) {
            size_t x2 = c->w - 1 - x;
            size_t i = canvas_idx(c, x, y), j = canvas_idx(c, x2, y);
            uint32_t tc = c->ch[i]; c->ch[i] = c->ch[j]; c->ch[j] = tc;
            unsigned char tcl = c->cls[i]; c->cls[i] = c->cls[j]; c->cls[j] = tcl;
        }
    }
    size_t n = c->w * c->h;
    for (size_t i = 0; i < n; i++) c->ch[i] = flip_glyph_h(c->ch[i]);

    for (size_t y = 0; y < c->h; y++) {
        size_t x = 0;
        while (x < c->w) {
            unsigned char cls = c->cls[canvas_idx(c, x, y)];
            if (cls == CLS_TEXT || cls == CLS_EDGE_LABEL) {
                size_t start = x;
                while (x < c->w && c->cls[canvas_idx(c, x, y)] == cls) x++;
                size_t end = x;
                size_t lo = start, hi = end - 1;
                while (lo < hi) {
                    size_t li = canvas_idx(c, lo, y), hidx = canvas_idx(c, hi, y);
                    uint32_t tc = c->ch[li]; c->ch[li] = c->ch[hidx]; c->ch[hidx] = tc;
                    lo++; hi--;
                }
            } else {
                x++;
            }
        }
    }
}

char **canvas_to_lines(const Canvas *c, size_t *n_out) {
    char **out = malloc(sizeof(char *) * (c->h ? c->h : 1));
    for (size_t y = 0; y < c->h; y++) {
        size_t last = c->w;
        for (long x = (long)c->w - 1; x >= 0; x--) {
            uint32_t ch = c->ch[canvas_idx(c, (size_t)x, y)];
            if (ch != CP_SPACE && ch != CONT) { last = (size_t)x + 1; break; }
            if (x == 0) last = 0;
        }
        uint32_t *row = malloc(sizeof(uint32_t) * (last ? last : 1));
        size_t rn = 0;
        for (size_t x = 0; x < last; x++) {
            uint32_t ch = c->ch[canvas_idx(c, x, y)];
            if (ch == CONT) continue;
            row[rn++] = ch;
        }
        char *line = utf8_encode(row, rn);
        free(row);
        size_t ll = strlen(line);
        while (ll > 0 && (line[ll - 1] == ' ')) ll--;
        line[ll] = 0;
        out[y] = line;
    }
    *n_out = c->h;
    return out;
}

void draw_seq_text(Canvas *c, const char *text, size_t x, size_t y, Cls cls) {
    uint32_t *cps; size_t n = utf8_decode(text, &cps);
    size_t cur = x;
    for (size_t i = 0; i < n; i++) {
        int cw = dwidth_char(cps[i]);
        if (cw < 1) cw = 1;
        for (int k = 0; k < cw; k++) {
            if (cur + (size_t)k < c->w && y < c->h) {
                size_t idx = canvas_idx(c, cur + (size_t)k, y);
                c->mask[idx] = 0;
            }
            canvas_set(c, cur + (size_t)k, y, k == 0 ? cps[i] : CONT, cls);
        }
        cur += (size_t)cw;
    }
    free(cps);
}

void draw_box(Canvas *c, const Placed *p, char **lines, size_t n_lines, Shape shape) {
    size_t x = p->x, y = p->y, w = p->w, h = p->h;
    size_t right = x + w - 1, bottom = y + h - 1;
    uint32_t tl, tr, bl, br;
    if (shape == SHAPE_ROUND || shape == SHAPE_DIAMOND) { tl = CP_RTL; tr = CP_RTR; bl = CP_RBL; br = CP_RBR; }
    else { tl = CP_TL; tr = CP_TR; bl = CP_BL; br = CP_BR; }
    canvas_set(c, x, y, tl, CLS_BORDER);
    canvas_set(c, right, y, tr, CLS_BORDER);
    canvas_set(c, x, bottom, bl, CLS_BORDER);
    canvas_set(c, right, bottom, br, CLS_BORDER);
    for (size_t cx = x + 1; cx < right; cx++) {
        canvas_add_bits(c, cx, y, BIT_L | BIT_R);
        canvas_add_bits(c, cx, bottom, BIT_L | BIT_R);
    }
    for (size_t cy = y + 1; cy < bottom; cy++) {
        canvas_add_bits(c, x, cy, BIT_U | BIT_D);
        canvas_add_bits(c, right, cy, BIT_U | BIT_D);
    }
    for (size_t cy = y; cy <= bottom; cy++)
        for (size_t cx = x; cx <= right; cx++)
            c->occupied[canvas_idx(c, cx, cy)] = true;

    size_t inner = w >= 2 * PAD + 2 ? w - 2 * PAD - 2 : 0;
    if (inner < 1) inner = 1;
    for (size_t li = 0; li < n_lines; li++) {
        size_t row = y + 1 + li;
        char *text = fit_label(lines[li], inner);
        size_t tw = dwidth_str(text);
        size_t text_x = x + 1 + PAD + (inner > tw ? (inner - tw) / 2 : 0);
        uint32_t *cps; size_t cn = utf8_decode(text, &cps);
        size_t cur = text_x;
        for (size_t k = 0; k < cn; k++) {
            int cw = dwidth_char(cps[k]);
            if (cw < 1) cw = 1;
            canvas_set(c, cur, row, cps[k], CLS_TEXT);
            for (int j = 1; j < cw; j++) canvas_set(c, cur + (size_t)j, row, CONT, CLS_TEXT);
            cur += (size_t)cw;
        }
        free(cps);
        free(text);
    }
}

uint32_t head_glyph(Head head, uint32_t arrow) {
    switch (head) {
        case HEAD_CIRCLE: return (uint32_t)'o';
        case HEAD_CROSS: return CP_CROSSX;
        case HEAD_DIAMOND_FILL: return CP_DIAMOND_FILL;
        case HEAD_DIAMOND_OPEN: return CP_DIAMOND_OPEN;
        case HEAD_TRIANGLE:
            if (arrow == CP_ARR_DOWN) return CP_ARR_DOWN_OPEN;
            if (arrow == CP_ARR_UP) return CP_ARR_UP_OPEN;
            if (arrow == CP_ARR_LEFT) return CP_ARR_LEFT_OPEN;
            if (arrow == CP_ARR_RIGHT) return CP_ARR_RIGHT_OPEN;
            return arrow;
        default: return arrow;
    }
}

void place_label(Canvas *c, const char *label, size_t row, size_t start_x) {
    if (row >= c->h) return;
    char *text = fit_label(label, MAX_LABEL);
    uint32_t *cps; size_t n = utf8_decode(text, &cps);
    size_t x = start_x;
    for (size_t i = 0; i < n; i++) {
        int cw = dwidth_char(cps[i]);
        if (cw < 1) cw = 1;
        if (x + (size_t)cw > c->w) break;
        bool blocked = false;
        for (int k = 0; k < cw; k++) {
            size_t idx = canvas_idx(c, x + (size_t)k, row);
            if (c->ch[idx] != CP_SPACE || c->mask[idx] != 0 || c->occupied[idx]) { blocked = true; break; }
        }
        if (blocked) break;
        canvas_set(c, x, row, cps[i], CLS_EDGE_LABEL);
        for (int k = 1; k < cw; k++) canvas_set(c, x + (size_t)k, row, CONT, CLS_EDGE_LABEL);
        x += (size_t)cw;
    }
    free(cps);
    free(text);
}

void draw_class_box(Canvas *c, const Placed *p, char ***sections, size_t *section_n, size_t n_sections) {
    draw_box(c, p, NULL, 0, SHAPE_RECT);
    size_t inner = p->w >= 2 * PAD + 2 ? p->w - 2 * PAD - 2 : 0;
    if (inner < 1) inner = 1;
    size_t row = p->y + 1;
    bool first = true;
    for (size_t si = 0; si < n_sections; si++) {
        if (section_n[si] == 0) continue;
        if (!first) {
            canvas_set(c, p->x, row, CP_TEE_L, CLS_BORDER);
            for (size_t x = p->x + 1; x < p->x + p->w - 1; x++) canvas_set(c, x, row, CP_H, CLS_BORDER);
            canvas_set(c, p->x + p->w - 1, row, CP_TEE_R, CLS_BORDER);
            row++;
        }
        first = false;
        for (size_t li = 0; li < section_n[si]; li++) {
            char *text = fit_label(sections[si][li], inner);
            size_t tw = dwidth_str(text);
            size_t tx = si == 0 ? p->x + 1 + PAD + (inner > tw ? (inner - tw) / 2 : 0) : p->x + 1 + PAD;
            draw_seq_text(c, text, tx, row, CLS_TEXT);
            free(text);
            row++;
        }
    }
}

MermaidArt *mermaid_art_from_canvas(Canvas *c) {
    MermaidArt *art = malloc(sizeof(MermaidArt));
    art->lines = canvas_to_lines(c, &art->n);
    return art;
}

void draw_frame(Canvas *c, const Placed *p, const char *title, const Canvas *sub) {
    draw_box(c, p, NULL, 0, SHAPE_RECT);
    size_t avail = p->w >= 4 ? p->w - 4 : 0;
    char *t = fit_label(title, avail);
    Buf b; buf_init(&b);
    buf_push_char(&b, ' ');
    buf_push_cstr(&b, t);
    buf_push_char(&b, ' ');
    free(t);
    char *padded = buf_take(&b);
    draw_seq_text(c, padded, p->x + 1, p->y, CLS_TEXT);
    free(padded);
    size_t ox = p->x + 1 + (p->w - 2 - sub->w) / 2;
    size_t oy = p->y + 1 + (p->h - 2 - sub->h) / 2;
    canvas_blit(c, sub, ox, oy);
}
