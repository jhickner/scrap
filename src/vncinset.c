#include "vncinset.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "image.h"
#include "overlay.h"
#include "text.h"
#include "ui.h"

#define STUB_W 320
#define STUB_H 200

#define BOX_H  "\xe2\x94\x80"
#define BOX_V  "\xe2\x94\x82"
#define BOX_TL "\xe2\x94\x8c"
#define BOX_TR "\xe2\x94\x90"
#define BOX_BL "\xe2\x94\x94"
#define BOX_BR "\xe2\x94\x98"

#define COLS_MIN 12
#define ROWS_MIN 4

struct vncinset {
    char                    bot[128];
    int                     on;
    int                     pct;
    enum vncinset_side      side;
    struct vncinset_source *src;
    uint64_t                drawn_gen;
    int                     drawn;
    int                     test;
};

struct stub {
    struct vncinset_source base;
    uint8_t                rgb[STUB_W * STUB_H * 3];
    uint64_t               gen;
    int                    filled;
};

static void stub_fill(struct stub *s)
{
    static const uint8_t bars[8][3] = {
        {235, 235, 235}, {235, 235, 16}, {16, 235, 235}, {16, 235, 16},
        {235, 16, 235},  {235, 16, 16},  {16, 16, 235},  {16, 16, 16},
    };
    int shift = (int)(s->gen % STUB_W);
    int bx = (int)(s->gen * 7 % (STUB_W - 40));
    int by = (int)(s->gen * 5 % (STUB_H - 40));
    for (int y = 0; y < STUB_H; y++) {
        for (int x = 0; x < STUB_W; x++) {
            uint8_t *p = s->rgb + (y * STUB_W + x) * 3;
            if (y >= STUB_H * 3 / 4) {
                uint8_t g = (uint8_t)(((x + shift * 4) % STUB_W) * 255 / STUB_W);
                p[0] = p[1] = p[2] = g;
            } else {
                memcpy(p, bars[((x + shift * 4) % STUB_W) * 8 / STUB_W], 3);
            }
            if (x >= bx && x < bx + 40 && y >= by && y < by + 40)
                p[0] = p[1] = p[2] = 0;
        }
    }
    s->filled = 1;
}

static int stub_frame(struct vncinset_source *src, struct vncinset_frame *out)
{
    struct stub *s = (struct stub *)src;
    uint64_t     gen = (uint64_t)(now_seconds() * VNCINSET_STUB_FPS);
    if (!s->filled || gen != s->gen) {
        s->gen = gen;
        stub_fill(s);
    }
    out->rgb = s->rgb;
    out->w = STUB_W;
    out->h = STUB_H;
    out->gen = s->gen;
    return 1;
}

static void stub_close(struct vncinset_source *src) { free(src); }

static struct vncinset_source *vncinset_stub_open(void)
{
    struct stub *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->base.frame = stub_frame;
    s->base.close = stub_close;
    return &s->base;
}

static struct vncinset_source *stub_opener(const char *bot)
{
    (void)bot;
    return vncinset_stub_open();
}

static vncinset_open_fn opener = stub_opener;

void vncinset_set_opener(vncinset_open_fn fn) { opener = fn ? fn : stub_opener; }

void vncinset_downscale(const uint8_t *src, int sw, int sh, uint8_t *dst, int dw, int dh)
{
    if (!src || !dst || sw < 1 || sh < 1 || dw < 1 || dh < 1)
        return;
    for (int dy = 0; dy < dh; dy++) {
        int y0 = (int)((long)dy * sh / dh);
        int y1 = (int)((long)(dy + 1) * sh / dh);
        if (y1 <= y0)
            y1 = y0 + 1;
        for (int dx = 0; dx < dw; dx++) {
            int x0 = (int)((long)dx * sw / dw);
            int x1 = (int)((long)(dx + 1) * sw / dw);
            if (x1 <= x0)
                x1 = x0 + 1;
            unsigned sum[3] = {0, 0, 0};
            for (int y = y0; y < y1; y++) {
                const uint8_t *p = src + ((size_t)y * (size_t)sw + (size_t)x0) * 3;
                for (int x = x0; x < x1; x++, p += 3) {
                    sum[0] += p[0];
                    sum[1] += p[1];
                    sum[2] += p[2];
                }
            }
            unsigned n = (unsigned)((x1 - x0) * (y1 - y0));
            uint8_t *d = dst + ((size_t)dy * (size_t)dw + (size_t)dx) * 3;
            d[0] = (uint8_t)(sum[0] / n);
            d[1] = (uint8_t)(sum[1] / n);
            d[2] = (uint8_t)(sum[2] / n);
        }
    }
}

int vncinset_layout(enum vncinset_side side, int pct, int cols, int rows, int frame_w,
                    int frame_h, int cell_w, int cell_h, struct vncinset_box *out)
{
    if (cols < COLS_MIN || rows < ROWS_MIN || frame_w < 1 || frame_h < 1 || cell_w < 1 ||
        cell_h < 1)
        return 0;
    if (pct < VNCINSET_WIDTH_MIN)
        pct = VNCINSET_WIDTH_MIN;
    if (pct > VNCINSET_WIDTH_MAX)
        pct = VNCINSET_WIDTH_MAX;

    int max_cells = image_cells_max();
    int w = cols * pct / 100;
    if (w < COLS_MIN)
        w = COLS_MIN;
    if (w > cols)
        w = cols;

    long img_cols = w - 2;
    if (img_cols > max_cells)
        img_cols = max_cells;
    long img_rows = (img_cols * cell_w * frame_h + (long)frame_w * cell_h / 2) /
                    ((long)frame_w * cell_h);
    if (img_rows < 1)
        img_rows = 1;
    if (img_rows > rows - 2 || img_rows > max_cells) {
        img_rows = rows - 2 < max_cells ? rows - 2 : max_cells;
        img_cols = (img_rows * cell_h * frame_w + (long)frame_h * cell_w / 2) /
                   ((long)frame_h * cell_w);
        if (img_cols < 1)
            img_cols = 1;
    }

    out->img_cols = (int)img_cols;
    out->img_rows = (int)img_rows;
    out->w = out->img_cols + 2;
    out->h = out->img_rows + 2;
    out->row = 0;
    out->col = side == VNCINSET_LEFT ? 0 : cols - out->w;
    return 1;
}

static const struct vncinset *sent_by;
static uint64_t               sent_gen;
static int                    sent_cols, sent_rows;
static double                 sent_at;
static uint8_t               *scaled;
static size_t                 scaled_cap;

struct vncinset *vncinset_new(const char *bot)
{
    struct vncinset *v = calloc(1, sizeof *v);
    if (!v)
        return NULL;
    snprintf(v->bot, sizeof v->bot, "%s", bot ? bot : "");
    v->pct = VNCINSET_WIDTH_DEFAULT;
    v->side = VNCINSET_RIGHT;
    return v;
}

static void close_source(struct vncinset *v)
{
    if (v->src && v->src->close)
        v->src->close(v->src);
    v->src = NULL;
    v->drawn = 0;
    if (sent_by == v) {
        image_drop(image_inset_id());
        sent_by = NULL;
    }
}

void vncinset_free(struct vncinset *v)
{
    if (!v)
        return;
    close_source(v);
    free(v);
}

void vncinset_set_bot(struct vncinset *v, const char *bot)
{
    if (!v || !bot || !strcmp(v->bot, bot))
        return;
    close_source(v);
    snprintf(v->bot, sizeof v->bot, "%s", bot);
}

void vncinset_set_test(struct vncinset *v, int on)
{
    if (!v || v->test == !!on)
        return;
    close_source(v);
    v->test = !!on;
}

int vncinset_shown(const struct vncinset *v) { return v && v->on; }

void vncinset_show(struct vncinset *v, int on)
{
    if (!v)
        return;
    v->on = on ? 1 : 0;
    if (!v->on)
        close_source(v);
}

void vncinset_set_side(struct vncinset *v, enum vncinset_side side)
{
    if (v)
        v->side = side;
}

void vncinset_set_width(struct vncinset *v, int pct)
{
    if (!v)
        return;
    if (pct < VNCINSET_WIDTH_MIN)
        pct = VNCINSET_WIDTH_MIN;
    if (pct > VNCINSET_WIDTH_MAX)
        pct = VNCINSET_WIDTH_MAX;
    v->pct = pct;
}

static int latest(struct vncinset *v, struct vncinset_frame *f)
{
    memset(f, 0, sizeof *f);
    if (!v->src)
        v->src = v->test ? vncinset_stub_open() : opener(v->bot);
    return v->src && v->src->frame && v->src->frame(v->src, f) && f->rgb && f->w > 0 &&
           f->h > 0;
}

int vncinset_stale(struct vncinset *v)
{
    if (!v || !v->on)
        return 0;
    struct vncinset_frame f;
    int                   have = latest(v, &f);
    if (have != v->drawn)
        return 1;
    return have && f.gen != v->drawn_gen &&
           (sent_by != v || now_seconds() - sent_at >= VNCINSET_FRAME_INTERVAL);
}

struct paint {
    const struct vncinset     *v;
    const struct vncinset_box *b;
    const char                *title;
    const char                *status;
    int                        image;
    int                        at;
};

static void rule(int n)
{
    for (int i = 0; i < n; i++)
        ui_put(BOX_H);
}

static void paint_row(void *ud, int unused, int width)
{
    (void)unused;
    const struct paint        *p = ud;
    const struct vncinset_box *b = p->b;
    int                        inner = width - 2;

    ui_esc(ui_style(UI_DIM));
    if (p->at == 0) {
        ui_put(BOX_TL);
        size_t budget = inner > 2 ? (size_t)(inner - 2) : 0;
        size_t fit = budget ? ui_fit_visible(p->title, strlen(p->title), budget) : 0;
        int    used = 0;
        if (fit) {
            ui_put(" ");
            ui_putn(p->title, fit);
            ui_put(" ");
            used = (int)ui_cells_n(p->title, fit) + 2;
        }
        rule(inner - used);
        ui_put(BOX_TR);
    } else if (p->at == b->h - 1) {
        ui_put(BOX_BL);
        rule(inner);
        ui_put(BOX_BR);
    } else {
        ui_put(BOX_V);
        ui_esc(ui_style(UI_RESET));
        if (p->image) {
            image_place_row(image_inset_id(), p->at - 1, inner);
        } else if (p->at == (b->h - 1) / 2 && inner >= 8) {
            const char *wait = p->status && *p->status ? p->status : "no frame";
            size_t      fit = ui_fit_visible(wait, strlen(wait), (size_t)inner);
            int         cells = (int)ui_cells_n(wait, fit);
            int         pad = (inner - cells) / 2;
            ui_pad(pad);
            ui_esc(ui_style(UI_DIM));
            ui_putn(wait, fit);
            ui_pad(inner - cells - pad);
        } else {
            ui_pad(inner);
        }
        ui_esc(ui_style(UI_DIM));
        ui_put(BOX_V);
    }
    ui_esc(ui_style(UI_RESET));
}

void vncinset_cover(struct vncinset *v, char **rows, int n, int cols)
{
    if (!v || !v->on)
        return;

    struct vncinset_frame f;
    int                   have = latest(v, &f);
    int                   cw, ch;
    image_cell_size(&cw, &ch);

    struct vncinset_box b;
    if (!vncinset_layout(v->side, v->pct, cols, n, have ? f.w : 16, have ? f.h : 10, cw,
                         ch, &b))
        return;

    int    image = have && image_available();
    double now = now_seconds();
    int    moved = sent_by != v || sent_cols != b.img_cols || sent_rows != b.img_rows;
    if (image && (moved || (sent_gen != f.gen && now - sent_at >= VNCINSET_FRAME_INTERVAL))) {
        const uint8_t *rgb = f.rgb;
        int            w = f.w, h = f.h;
        long           px_w = (long)b.img_cols * cw, px_h = (long)b.img_rows * ch;
        if (px_w > VNCINSET_TRANSMIT_W_MAX) {
            px_h = px_h * VNCINSET_TRANSMIT_W_MAX / px_w;
            px_w = VNCINSET_TRANSMIT_W_MAX;
        }
        if (w > px_w || h > px_h) {
            long dw = px_w, dh = (long)h * px_w / w;
            if (dh > px_h) {
                dh = px_h;
                dw = (long)w * px_h / h;
            }
            if (dw < 1)
                dw = 1;
            if (dh < 1)
                dh = 1;
            size_t need = (size_t)dw * (size_t)dh * 3;
            if (need > scaled_cap) {
                uint8_t *grown = realloc(scaled, need);
                if (grown) {
                    scaled = grown;
                    scaled_cap = need;
                }
            }
            if (need <= scaled_cap) {
                vncinset_downscale(f.rgb, f.w, f.h, scaled, (int)dw, (int)dh);
                rgb = scaled;
                w = (int)dw;
                h = (int)dh;
            }
        }
        image_frame(image_inset_id(), rgb, w, h, b.img_cols, b.img_rows);
        sent_by = v;
        sent_gen = f.gen;
        sent_cols = b.img_cols;
        sent_rows = b.img_rows;
        sent_at = now;
    }
    v->drawn = have;
    if (!have || !image || sent_by == v)
        v->drawn_gen = have ? (image ? sent_gen : f.gen) : 0;

    const char *status = v->src && v->src->status ? v->src->status(v->src) : NULL;
    char        title[256];
    if (status && *status)
        snprintf(title, sizeof title, "%s: %s", v->bot, status);
    else
        snprintf(title, sizeof title, "%s", v->bot);

    struct paint   p = {v, &b, title, status, image, 0};
    struct overlay o = {.row = 0, .col = b.col, .w = b.w, .rows = 1, .paint_row = paint_row,
                        .ud = &p};
    for (int r = b.row; r < b.row + b.h && r < n; r++) {
        p.at = r - b.row;
        ui_sink_begin();
        overlay_put(rows[r] ? rows[r] : "", &o);
        char *s = ui_sink_end();
        if (!s)
            continue;
        size_t len = strlen(s);
        while (len && (s[len - 1] == '\n' || s[len - 1] == '\r'))
            s[--len] = '\0';
        free(rows[r]);
        rows[r] = s;
    }
}
