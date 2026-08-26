#include "viewport.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "text.h"
#include "tty.h"
#include "ui.h"
#include "vendor/cJSON.h"

#define ITEMS_KEEP 8000

struct item {
    viewport_render_fn render;
    void  *ud;
    void (*free_ud)(void *);
    int    reflow;
    char **rows;
    int    nrows;
    int    cols;
    int    pad;
    int    hidden;
    int    nopad;
    int    borrowed;
    int    hcols;
    int    hrows;
    unsigned id;
    const char        *kind;
    viewport_encode_fn encode;
};

static struct item *items;
static int    nitems, items_cap;
static unsigned next_id = 1;

static char  *open_buf;
static size_t open_len, open_cap;
static viewport_render_fn open_render;
static void  *open_ud;
static void (*open_free)(void *);
static int    open_reflow;
static int    open_wrapped;
static int    open_pad_after;
static int    open_paid;

static int    tail_pad;

#define OPEN_MAX 4

struct open_frame {
    void  *ud;
    void (*free_ud)(void *);
};

static struct open_frame open_stack[OPEN_MAX];
static int open_depth;

static int in_render;

static char **chrome_rows;
static int    chrome_n, chrome_cap;
static int    chrome_caret_row, chrome_caret_col = -1;
static int    chrome_top = -1;

static int held;
static int active;
static int handed;
static int suspended;
static int scrolled;
static int dirty;
static int deferred;
static int painting;

static viewport_width_fn on_width;
static int painted_cols;

static unsigned anchor_id;
static int      anchor_skip;

/* every input to window_geometry bumps this; the cached geometry carries the
   epoch it was measured at */
static unsigned layout_epoch = 1;

static void layout_changed(void) { layout_epoch++; }

#define MOUSE_ON  "\x1b[?1000h\x1b[?1006h"
#define MOUSE_OFF "\x1b[?1000l\x1b[?1002l\x1b[?1003l\x1b[?1006l"

static int sync_frames(void)
{
    static int on = -1;
    if (on < 0)
        on = getenv("TMUX") == NULL;
    return on;
}

/* a frame goes out in one write: split over several, the terminal draws the
   halves as they land, and under tmux there is no synchronized update to hide it */
static char  *batch;
static size_t batch_len, batch_cap;
static int    batching;

static void direct(const char *s, size_t n)
{
    if (!batching) {
        fwrite(s, 1, n, stdout);
        return;
    }
    if (batch_len + n > batch_cap) {
        size_t want = batch_cap ? batch_cap : 8192;
        while (want < batch_len + n)
            want *= 2;
        char *grown = realloc(batch, want);
        if (!grown) {
            fwrite(batch, 1, batch_len, stdout);
            batch_len = 0;
            fwrite(s, 1, n, stdout);
            return;
        }
        batch = grown;
        batch_cap = want;
    }
    memcpy(batch + batch_len, s, n);
    batch_len += n;
}

static void batch_begin(void)
{
    batch_len = 0;
    batching = 1;
}

static void batch_end(void)
{
    batching = 0;
    fflush(stdout);
    for (size_t at = 0; at < batch_len;) {
        ssize_t put = write(STDOUT_FILENO, batch + at, batch_len - at);
        if (put <= 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        at += (size_t)put;
    }
    batch_len = 0;
}

static void direct_str(const char *s) { direct(s, strlen(s)); }

static void cup(int row, int col)
{
    char esc[32];
    snprintf(esc, sizeof esc, "\x1b[%d;%dH", row, col);
    direct_str(esc);
}

int viewport_active(void) { return active && !suspended; }

void viewport_touch(void) { dirty = 1; layout_changed(); }

int viewport_scrolled(void) { return scrolled; }

unsigned viewport_mark(void) { return next_id; }

static struct item *item_by_mark(unsigned mark)
{
    int lo = 0, hi = nitems - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (items[mid].id == mark)
            return &items[mid];
        if (items[mid].id < mark)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

static int index_of_mark(unsigned mark)
{
    int lo = 0, hi = nitems - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (items[mid].id == mark)
            return mid;
        if (items[mid].id < mark)
            lo = mid + 1;
        else
            hi = mid - 1;
    }

    return mark >= next_id ? nitems : 0;
}

void *viewport_item_data(unsigned mark)
{
    struct item *it = item_by_mark(mark);
    return it ? it->ud : NULL;
}

void viewport_item_persist(unsigned mark, const char *kind, viewport_encode_fn encode)
{
    struct item *it = item_by_mark(mark);
    if (!it)
        return;
    it->kind = kind;
    it->encode = encode;
}

void viewport_item_hide(unsigned mark, int on)
{
    struct item *it = item_by_mark(mark);
    if (!it || it->hidden == !!on)
        return;
    it->hidden = !!on;
    dirty = 1;
    layout_changed();
}

void viewport_item_pad(unsigned mark, int on)
{
    struct item *it = item_by_mark(mark);
    if (!it || it->nopad == !on)
        return;
    it->nopad = !on;
    dirty = 1;
    layout_changed();
}

void viewport_item_stale(unsigned mark)
{
    struct item *it = item_by_mark(mark);
    if (!it || !it->render)
        return;
    it->cols = -1;
    dirty = 1;
    layout_changed();
}

void viewport_on_width(viewport_width_fn fn)
{
    on_width = fn;
}

void viewport_scan(unsigned from, viewport_scan_fn fn, void *ctx)
{
    int at = from ? index_of_mark(from) : 0;
    if (at < 0)
        at = 0;
    for (int i = at; i < nitems; i++)
        if (!items[i].pad)
            fn(items[i].id, items[i].kind, items[i].ud, ctx);
}

void viewport_repad(void)
{
    int hide = 0;
    for (int i = nitems - 1; i >= 0; i--) {
        if (items[i].pad)
            items[i].hidden = hide;
        else
            hide = items[i].hidden || items[i].nopad;
    }
    dirty = 1;
    layout_changed();
}

void viewport_item_update(unsigned mark)
{
    if (in_render)
        return;
    struct item *it = item_by_mark(mark);
    if (!it || !it->render)
        return;
    it->cols = -1;
    dirty = 1;
    layout_changed();
    viewport_paint();
}

static void rows_free(struct item *it)
{
    if (!it->borrowed) {
        for (int i = 0; i < it->nrows; i++)
            free(it->rows[i]);
        free(it->rows);
    }
    it->rows = NULL;
    it->nrows = 0;
    it->borrowed = 0;
    it->hcols = 0;
    layout_changed();
}

static void item_free(struct item *it)
{
    rows_free(it);
    if (it->free_ud && it->ud)
        it->free_ud(it->ud);
    memset(it, 0, sizeof *it);
}

static void rows_set(struct item *it, const char *body, int cols)
{
    rows_free(it);
    it->cols = cols;
    if (!body || !*body)
        return;

    int cap = 8, n = 0;
    char **out = malloc((size_t)cap * sizeof *out);
    if (!out)
        return;

    const char *p = body;
    for (;;) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        while (len && p[len - 1] == '\r')
            len--;
        if (n == cap) {
            cap *= 2;
            char **grown = realloc(out, (size_t)cap * sizeof *grown);
            if (!grown)
                break;
            out = grown;
        }
        out[n] = strndup(p, len);
        if (!out[n])
            break;
        n++;
        if (!nl)
            break;
        p = nl + 1;
        if (!*p)
            break;
    }
    it->rows = out;
    it->nrows = n;
}

static struct item *items_push(void)
{
    if (in_render)
        return NULL;
    if (nitems == items_cap) {
        int cap = items_cap ? items_cap * 2 : 256;
        struct item *grown = realloc(items, (size_t)cap * sizeof *grown);
        if (!grown)
            return NULL;
        items = grown;
        items_cap = cap;
    }
    struct item *it = &items[nitems++];
    memset(it, 0, sizeof *it);
    it->id = next_id++;

    if (nitems > ITEMS_KEEP) {
        int drop = nitems - ITEMS_KEEP;
        for (int i = 0; i < drop; i++)
            item_free(&items[i]);
        memmove(items, items + drop, (size_t)(nitems - drop) * sizeof *items);
        nitems -= drop;
        it = &items[nitems - 1];
    }
    layout_changed();
    return it;
}

static void open_append(const char *s, size_t n)
{
    if (open_len + n + 1 > open_cap) {
        size_t cap = open_cap ? open_cap : 256;
        while (cap < open_len + n + 1)
            cap *= 2;
        char *grown = realloc(open_buf, cap);
        if (!grown)
            return;
        open_buf = grown;
        open_cap = cap;
    }
    memcpy(open_buf + open_len, s, n);
    open_len += n;
    open_buf[open_len] = '\0';
    layout_changed();
}

static int row_is_blank(const char *s, size_t n);

static int trailing_blanks(void)
{
    int n = 0;
    for (int i = nitems - 1; i >= 0; i--) {
        if (items[i].hidden)
            continue;
        for (int r = items[i].nrows - 1; r >= 0; r--) {
            if (!row_is_blank(items[i].rows[r], strlen(items[i].rows[r])))
                return n;
            n++;
        }
        if (items[i].nrows == 0 && !items[i].render)
            n++;
    }
    return n;
}

static void blank_push(void)
{
    struct item *it = items_push();
    if (!it)
        return;
    rows_set(it, "", 0);
    it->pad = 1;
}

static void pad_seam(int before, int own)
{
    if (!nitems)
        return;
    int want = before > tail_pad ? before : tail_pad;
    if (want <= own)
        return;
    want -= trailing_blanks() + own;
    for (int i = 0; i < want; i++)
        blank_push();
}

static void loose_row(const char *body, size_t n)
{
    if (!getenv("MUX_STRICT_ENTRIES"))
        return;
    while (n && (body[n - 1] == '\n' || body[n - 1] == '\r'))
        n--;
    if (!n)
        return;

    char path[4200];
    if (!path_config_file(path, sizeof path, "loose-rows.log"))
        return;
    FILE *f = fopen(path, "a");
    if (!f)
        return;
    fprintf(f, "%.*s\n", (int)(n > 120 ? 120 : n), body);
    fclose(f);
}

/* the buffer is read back as a C string, so the length reset has to move the
   terminator with it -- an item that renders nothing would otherwise inherit
   the bytes of the one before it */
static void open_reset(void)
{
    open_len = 0;
    if (open_buf)
        open_buf[0] = '\0';
}

static void open_close(int cols)
{
    if (!open_paid) {
        loose_row(open_buf ? open_buf : "", open_len);
        const char *body = open_buf ? open_buf : "";
        pad_seam(0, row_is_blank(body, open_len) ? 1 : 0);
    }

    struct item *it = items_push();
    if (!it) {
        open_reset();
        layout_changed();
        return;
    }
    it->render = open_render;
    it->ud = open_ud;
    it->free_ud = open_free;
    it->reflow = open_reflow;
    rows_set(it, open_buf ? open_buf : "", cols);

    open_reset();
    layout_changed();
    open_render = NULL;
    open_ud = NULL;
    open_free = NULL;
    open_reflow = 0;
    tail_pad = open_pad_after;
    open_pad_after = 0;
    open_paid = 0;
}

unsigned viewport_item_begin(const struct viewport_entry *e)
{
    static const struct viewport_entry none;
    if (!e)
        e = &none;
    void *ud = e->ud;
    void (*free_ud)(void *) = e->free_ud;

    int depth = open_depth++;
    if (depth > 0 || in_render) {
        if (depth < OPEN_MAX) {
            open_stack[depth].ud = ud;
            open_stack[depth].free_ud = free_ud;
        } else if (free_ud && ud) {
            free_ud(ud);
        }
        return 0;
    }

    int diverted = ui_diverted();

    if (viewport_active() && open_len)
        open_close(0);
    if (viewport_active() && !diverted) {
        pad_seam(e->pad_before, 0);
        open_paid = 1;
    } else if (!viewport_active() && !diverted) {
        int want = e->pad_before > tail_pad ? e->pad_before : tail_pad;
        for (int i = 0; i < want; i++)
            ui_put("\n");
        tail_pad = e->pad_after;
    }
    open_render = e->render;
    open_ud = ud;
    open_free = free_ud;
    open_reflow = e->reflow;
    open_pad_after = e->pad_after;

    open_wrapped = viewport_active() && !diverted;
    return open_wrapped ? next_id : 0;
}

void viewport_item_end(void)
{
    if (open_depth == 0)
        return;
    int depth = --open_depth;
    if (depth > 0 || in_render) {
        if (depth < OPEN_MAX) {
            struct open_frame *f = &open_stack[depth];
            if (f->free_ud && f->ud)
                f->free_ud(f->ud);
            f->ud = NULL;
            f->free_ud = NULL;
        }
        return;
    }

    if (!open_wrapped) {
        if (open_free && open_ud)
            open_free(open_ud);
        open_render = NULL;
        open_ud = NULL;
        open_free = NULL;
        open_pad_after = 0;
        open_paid = 0;
        return;
    }
    open_wrapped = 0;
    open_close(tty_screen_columns());
    dirty = 1;
}

void viewport_write(const char *s, size_t n)
{
    if (in_render)
        return;
    if (open_wrapped) {
        open_append(s, n);
        return;
    }

    size_t start = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n') {
            open_append(s + start, i - start);
            open_close(0);
            start = i + 1;
        } else if (s[i] == '\r') {
            open_append(s + start, i - start);
            open_reset();
            layout_changed();
            start = i + 1;
        }
    }
    open_append(s + start, n - start);

    dirty = 1;
}

static int row_is_blank(const char *s, size_t n)
{
    for (size_t i = 0; i < n;) {
        enum ui_esc_kind kind;
        size_t end = ui_esc_span(s, n, i, &kind);
        if (kind == UI_ESC_TEXT && s[i] != ' ' && s[i] != '\t' && s[i] != '\r')
            return 0;
        i = end;
    }
    return 1;
}

static int text_ends_blank(const char *s, size_t n)
{
    if (!n)
        return 1;
    if (s[n - 1] == '\n')
        n--;
    size_t start = n;
    while (start && s[start - 1] != '\n')
        start--;
    return row_is_blank(s + start, n - start);
}

int viewport_ends_blank(void)
{
    if (!viewport_active())
        return 0;
    if (open_len)
        return text_ends_blank(open_buf, open_len);
    for (int i = nitems - 1; i >= 0; i--) {
        if (items[i].hidden)
            continue;
        if (items[i].nrows == 0) {
            if (!items[i].render)
                return 1;
            continue;
        }
        const char *last = items[i].rows[items[i].nrows - 1];
        return row_is_blank(last, strlen(last));
    }
    return 1;
}

struct viewport_state {
    struct item *items;
    int    nitems, items_cap;
    char  *open_buf;
    size_t open_len, open_cap;
    viewport_render_fn open_render;
    void  *open_ud;
    void (*open_free)(void *);
    int    open_wrapped;
    int    open_pad_after;
    int    open_paid;
    int    tail_pad;
    int    scrolled;
    unsigned anchor_id;
    int      anchor_skip;
};

void viewport_hold(int on)
{
    held = on ? 1 : 0;
}

struct viewport_state *viewport_state_new(void)
{
    return calloc(1, sizeof(struct viewport_state));
}

void viewport_state_free(struct viewport_state *st)
{
    if (!st)
        return;
    for (int i = 0; i < st->nitems; i++)
        item_free(&st->items[i]);
    free(st->items);
    free(st->open_buf);
    if (st->open_free && st->open_ud)
        st->open_free(st->open_ud);
    free(st);
}

void viewport_stash(struct viewport_state *st)
{
    if (!st)
        return;
    st->items = items;
    st->nitems = nitems;
    st->items_cap = items_cap;
    st->open_buf = open_buf;
    st->open_len = open_len;
    st->open_cap = open_cap;
    st->open_render = open_render;
    st->open_ud = open_ud;
    st->open_free = open_free;
    st->open_wrapped = open_wrapped;
    st->open_pad_after = open_pad_after;
    st->open_paid = open_paid;
    st->tail_pad = tail_pad;
    st->scrolled = scrolled;
    st->anchor_id = anchor_id;
    st->anchor_skip = anchor_skip;

    items = NULL;
    nitems = items_cap = 0;
    open_buf = NULL;
    open_len = open_cap = 0;
    open_render = NULL;
    open_ud = NULL;
    open_free = NULL;
    open_wrapped = 0;
    open_pad_after = 0;
    open_paid = 0;
    tail_pad = 0;
    scrolled = 0;
    anchor_id = 0;
    anchor_skip = 0;
    dirty = 1;
    layout_changed();
}

void viewport_adopt(struct viewport_state *st)
{
    if (!st)
        return;
    items = st->items;
    nitems = st->nitems;
    items_cap = st->items_cap;
    open_buf = st->open_buf;
    open_len = st->open_len;
    open_cap = st->open_cap;
    open_render = st->open_render;
    open_ud = st->open_ud;
    open_free = st->open_free;
    open_wrapped = st->open_wrapped;
    open_pad_after = st->open_pad_after;
    open_paid = st->open_paid;
    tail_pad = st->tail_pad;
    scrolled = st->scrolled;
    anchor_id = st->anchor_id;
    anchor_skip = st->anchor_skip;
    memset(st, 0, sizeof *st);
    dirty = 1;
    layout_changed();
}

void viewport_clear(void)
{
    for (int i = 0; i < nitems; i++)
        item_free(&items[i]);
    nitems = 0;
    open_reset();
    tail_pad = 0;
    scrolled = 0;
    anchor_id = 0;
    anchor_skip = 0;
    dirty = 1;
    layout_changed();
}

struct style {
    char  *buf;
    size_t len, cap;
};

static void style_add(struct style *st, const char *s, size_t n)
{
    if (st->len + n + 1 > st->cap) {
        size_t cap = st->cap ? st->cap : 512;
        while (cap < st->len + n + 1)
            cap *= 2;
        char *grown = realloc(st->buf, cap);
        if (!grown)
            return;
        st->buf = grown;
        st->cap = cap;
    }
    memcpy(st->buf + st->len, s, n);
    st->len += n;
    st->buf[st->len] = '\0';
}

static void style_reset(struct style *st)
{
    st->len = 0;
    if (st->buf)
        st->buf[0] = '\0';
}

static void style_copy(struct style *dst, const struct style *src)
{
    dst->len = 0;
    if (src->len)
        style_add(dst, src->buf, src->len);
    else
        style_reset(dst);
}

static int sgr_is_reset(const char *s, size_t n)
{
    size_t i = 2;
    if (i >= n)
        return 1;
    if (s[i] == 'm')
        return 1;
    for (; i < n && s[i] != 'm'; i++)
        if (s[i] != '0')
            return 0;
    return 1;
}

static size_t step(const char *s, size_t n, size_t i, size_t *cells, struct style *st)
{
    enum ui_esc_kind kind;
    size_t end = ui_esc_span(s, n, i, &kind);

    switch (kind) {
    case UI_ESC_TEXT:
        *cells += ui_cells_n(s + i, end - i);
        break;
    case UI_ESC_SGR:
        if (st) {
            if (sgr_is_reset(s + i, end - i)) {
                style_reset(st);
            } else {
                style_add(st, s + i, end - i);
            }
        }
        break;
    case UI_ESC_OSC8:
        if (st)
            style_add(st, s + i, end - i);
        break;
    case UI_ESC_OTHER:
        break;
    }
    return end;
}

static int wrap_count(const char *s, int W)
{
    size_t n = strlen(s);
    if (n == 0 || W < 1)
        return 1;
    int    used = 1;
    size_t cells = 0, i = 0;
    while (i < n) {
        size_t was = cells;
        i = step(s, n, i, &cells, NULL);
        if (cells > (size_t)W && was > 0) {
            used++;
            cells -= was;
        }
    }
    return used;
}

static void item_rows(struct item *it, int W)
{
    if (!it->render || !it->reflow || it->cols == W)
        return;
    ui_capture_begin(W);
    in_render++;
    it->render(it->ud, W);
    in_render--;
    char *painted = ui_capture_end();
    rows_set(it, painted ? painted : "", W);
    free(painted);
}

static struct item *item_at(int r, struct item *pending)
{
    return r == nitems ? pending : &items[r];
}

static int item_height(int r, struct item *pending, int W)
{
    if (item_at(r, pending)->hidden)
        return 0;
    item_rows(item_at(r, pending), W);
    struct item *it = item_at(r, pending);
    if (it->hcols == W)
        return it->hrows;

    int used;
    if (it->nrows == 0) {
        used = it->render ? 0 : 1;
    } else {
        used = 0;
        for (int i = 0; i < it->nrows; i++)
            used += wrap_count(it->rows[i], W);
    }
    if (W > 0) {
        it->hcols = W;
        it->hrows = used;
    }
    return used;
}

struct frame {
    char              **row;
    unsigned long long *hash;
    int                 n, cap;
};

static struct frame shown;
static struct frame built;
static struct frame all;
static int shown_rows, shown_cols;

static unsigned long long row_hash(const char *s)
{
    unsigned long long h = 1469598103934665603ULL;
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 1099511628211ULL;
    }
    return h;
}

static void frame_reset(struct frame *f)
{
    for (int i = 0; i < f->n; i++)
        free(f->row[i]);
    f->n = 0;
}

static void frame_add(struct frame *f, char *s, unsigned long long h)
{
    if (f->n == f->cap) {
        int cap = f->cap ? f->cap * 2 : 64;
        char **r = realloc(f->row, (size_t)cap * sizeof *r);
        unsigned long long *h = realloc(f->hash, (size_t)cap * sizeof *h);
        if (r)
            f->row = r;
        if (h)
            f->hash = h;
        if (!r || !h) {
            free(s);
            return;
        }
        f->cap = cap;
    }
    f->hash[f->n] = h;
    f->row[f->n++] = s;
}

static void frame_push(struct frame *f, char *s)
{
    if (!s)
        return;
    frame_add(f, s, row_hash(s));
}

static void frame_swap(struct frame *a, struct frame *b)
{
    struct frame t = *a;
    *a = *b;
    *b = t;
}

static void row_into_frame(struct frame *f, const char *s, int W)
{
    static struct style st, at_start;
    size_t n = strlen(s);
    size_t i = 0, start = 0;

    style_reset(&st);
    for (;;) {
        size_t cells = 0;
        size_t line_start = start;
        style_copy(&at_start, &st);
        while (i < n) {
            size_t was = cells;
            size_t next = step(s, n, i, &cells, &st);
            if (cells > (size_t)W && was > 0)
                break;
            i = next;
        }

        size_t body = i - line_start;
        char  *out = malloc(at_start.len + body + 1);
        if (!out)
            return;
        if (at_start.len)
            memcpy(out, at_start.buf, at_start.len);
        memcpy(out + at_start.len, s + line_start, body);
        out[at_start.len + body] = '\0';
        frame_push(f, out);

        start = i;
        if (i >= n)
            return;
    }
}

static char *blank_row(void) { return strdup(""); }

static int window_pending(struct item *pending)
{
    if (open_len && !open_wrapped) {
        pending->rows = &open_buf;
        pending->nrows = 1;
        pending->borrowed = 1;
        return 1;
    }
    return 0;
}

static int window_first(int W, int body, int scroll, struct item *pending, int total,
                        int *have_out)
{
    int want = body + scroll;
    int first = total;
    int have = 0;
    while (first > 0 && have < want) {
        have += item_height(first - 1, pending, W);
        first--;
    }
    *have_out = have;
    return first;
}

struct window {
    int first;
    int skip;
    int body;
    int chrome_shown;
    int total;
    int scrolled;
};

static struct window geom_cache;
static unsigned      geom_epoch;
static int           geom_W, geom_H, geom_valid;

static struct window window_geometry(int W, int H, struct item *pending)
{
    if (geom_valid && geom_epoch == layout_epoch && geom_W == W && geom_H == H) {
        window_pending(pending);
        return geom_cache;
    }

    struct window g = {0};

    int ch = chrome_n;
    if (ch > H)
        ch = H;
    if (ch < 0)
        ch = 0;

    g.total = nitems + window_pending(pending);
    g.scrolled = scrolled;

    if (anchor_id) {
        int tail = ch;
        for (int r = index_of_mark(anchor_id); r < g.total; r++)
            tail += item_height(r, pending, W);
        int want = tail - anchor_skip - H;
        g.scrolled = want > 0 ? want : 0;
    }

    int chrome_shown = ch - g.scrolled;
    if (chrome_shown < 0)
        chrome_shown = 0;
    int body = H - chrome_shown;
    int scroll = g.scrolled > ch ? g.scrolled - ch : 0;

    int have = 0;
    int first = window_first(W, body, scroll, pending, g.total, &have);
    if (first == 0 && have < body + scroll) {
        int most = have + ch - H;
        g.scrolled = most > 0 ? most : 0;
        chrome_shown = ch - g.scrolled;
        if (chrome_shown < 0)
            chrome_shown = 0;
        body = H - chrome_shown;
        scroll = g.scrolled > ch ? g.scrolled - ch : 0;
    }

    g.first = first;
    g.body = body;
    g.chrome_shown = chrome_shown;
    g.skip = have - body - scroll;
    if (g.skip < 0)
        g.skip = 0;

    geom_cache = g;
    geom_W = W;
    geom_H = H;
    geom_epoch = layout_epoch;
    geom_valid = 1;
    return g;
}

int viewport_visible(unsigned mark)
{
    int W = tty_screen_columns(), H = tty_rows();
    if (W < 1 || H < 1)
        return 1;

    struct item pending = {0};
    struct window g = window_geometry(W, H, &pending);
    return g.first >= nitems ? mark >= next_id : mark >= items[g.first].id;
}

static int shift_score(int body, int k)
{
    int score = 0;
    for (int i = 0; i < body; i++) {
        int j = i + k;
        if (j < 0 || j >= body)
            continue;
        if (built.hash[i] == shown.hash[j] && built.row[i][0])
            score++;
    }
    return score;
}

/* prefix counts of the built rows a shift can score on: no shift k can beat a
   score the rows in its overlap cannot reach */
static int *shift_reach(int body)
{
    static int *reach;
    static int  reach_cap;

    if (body + 1 > reach_cap) {
        int *grown = realloc(reach, (size_t)(body + 1) * sizeof *grown);
        if (!grown)
            return NULL;
        reach = grown;
        reach_cap = body + 1;
    }
    reach[0] = 0;
    for (int i = 0; i < body; i++)
        reach[i + 1] = reach[i] + (built.row[i][0] ? 1 : 0);
    return reach;
}

static int frame_shift(int body, int *score_out)
{
    *score_out = 0;
    if (shown.n < body)
        return 0;

    int  best = shift_score(body, 0);
    int  best_k = 0;
    int *reach = shift_reach(body);
    for (int k = -(body - 1); k < body; k++) {
        if (k == 0)
            continue;
        if (reach) {
            int lo = k > 0 ? 0 : -k;
            int hi = k > 0 ? body - k : body;
            if (reach[hi] - reach[lo] <= best)
                continue;
        }
        int score = shift_score(body, k);
        if (score > best) {
            best = score;
            best_k = k;
        }
    }
    *score_out = best_k ? best : 0;
    return best_k;
}

void viewport_forget(void)
{
    frame_reset(&shown);
}

void viewport_defer(void)
{
    deferred = 1;
}

void viewport_flush(void)
{
    if (!deferred)
        return;
    deferred = 0;
    if (dirty)
        viewport_paint();
}

void viewport_paint(void)
{
    if (!active || suspended || held || in_render || deferred || painting)
        return;

    int H = tty_rows(), W = tty_screen_columns();
    if (H < 1 || W < 1)
        return;

    painting = 1;
    if (W != painted_cols) {
        painted_cols = W;
        if (on_width)
            on_width();
    }

    struct item pending = {0};
    struct window g = window_geometry(W, H, &pending);
    unsigned want_anchor = 0;
    int      want_skip = 0;
    if (g.scrolled > 0) {
        want_anchor = g.first < nitems ? items[g.first].id : next_id;
        want_skip = g.skip;
    }
    if (scrolled != g.scrolled || anchor_id != want_anchor || anchor_skip != want_skip) {
        scrolled = g.scrolled;
        anchor_id = want_anchor;
        anchor_skip = want_skip;
        layout_changed();
    }

    int chrome_shown = g.chrome_shown;
    int body = g.body;
    int skip = g.skip;

    chrome_top = chrome_shown > 0 ? body : -1;

    frame_reset(&all);
    for (int r = g.first; r < g.total && r <= nitems; r++) {
        if (item_at(r, &pending)->hidden)
            continue;
        item_rows(item_at(r, &pending), W);
        struct item *it = item_at(r, &pending);
        if (it->nrows == 0 && !it->render)
            frame_push(&all, blank_row());
        for (int i = 0; i < it->nrows; i++)
            row_into_frame(&all, it->rows[i], W);
        if (all.n >= skip + body)
            break;
    }

    frame_reset(&built);

    int content = all.n - skip;
    if (content < 0)
        content = 0;
    if (content > body)
        content = body;
    for (int i = content; i < body; i++)
        frame_push(&built, blank_row());
    for (int i = 0; i < content; i++) {
        frame_add(&built, all.row[skip + i], all.hash[skip + i]);
        all.row[skip + i] = NULL;
    }
    frame_reset(&all);

    for (int i = 0; i < chrome_shown; i++)
        frame_push(&built, strdup(chrome_rows[i]));

    if (shown_rows != H || shown_cols != W) {
        frame_reset(&shown);
        shown_rows = H;
        shown_cols = W;
    }

    batch_begin();
    if (sync_frames())
        direct_str("\x1b[?2026h");
    direct_str("\x1b[?25l");
    direct_str("\x1b[?7l");

    int span = built.n;
    int score = 0;
    int k = shown.n == built.n ? frame_shift(span, &score) : 0;
    if (k != 0 && score >= span / 3) {
        char esc[32];
        snprintf(esc, sizeof esc, "\x1b[1;%dr", span);
        direct_str(esc);
        snprintf(esc, sizeof esc, "\x1b[%d%c", k > 0 ? k : -k, k > 0 ? 'S' : 'T');
        direct_str(esc);
        direct_str("\x1b[r");

        for (int i = 0; i < span; i++) {
            int j = k > 0 ? i : span - 1 - i;
            int from = j + k;
            free(shown.row[j]);
            if (from >= 0 && from < span) {
                shown.row[j] = strdup(shown.row[from]);
                shown.hash[j] = shown.hash[from];
            } else {
                shown.row[j] = blank_row();
                shown.hash[j] = row_hash("");
            }
        }
    }

    for (int i = 0; i < built.n; i++) {
        if (i < shown.n && shown.hash[i] == built.hash[i] &&
            strcmp(shown.row[i], built.row[i]) == 0)
            continue;
        cup(i + 1, 1);
        direct_str("\x1b[0m\x1b[K");
        direct_str(built.row[i]);
    }

    direct_str("\x1b[?7h");
    direct_str("\x1b[0m");

    if (chrome_caret_col >= 0 && chrome_caret_row >= 0 && chrome_caret_row < chrome_shown) {
        int col = chrome_caret_col + 1;
        if (col > W)
            col = W;
        cup(body + 1 + chrome_caret_row, col);
        direct_str("\x1b[?25h");
    }

    if (sync_frames())
        direct_str("\x1b[?2026l");
    batch_end();

    frame_swap(&shown, &built);
    dirty = 0;
    painting = 0;
}

void viewport_chrome(char **rows_in, int n, int caret_row, int caret_col)
{
    int was = chrome_n;
    for (int i = 0; i < chrome_n; i++)
        free(chrome_rows[i]);
    chrome_n = 0;

    if (n > chrome_cap) {
        char **grown = realloc(chrome_rows, (size_t)n * sizeof *grown);
        if (!grown)
            return;
        chrome_rows = grown;
        chrome_cap = n;
    }
    for (int i = 0; i < n; i++) {
        chrome_rows[i] = strdup(rows_in[i] ? rows_in[i] : "");
        if (!chrome_rows[i])
            break;
        chrome_n++;
    }
    chrome_caret_row = caret_row;
    chrome_caret_col = caret_col;
    if (chrome_n != was)
        layout_changed();
    dirty = 1;
}

int viewport_chrome_top(void)
{
    return chrome_top;
}

void viewport_chrome_row(int at, const char *s)
{
    if (at < 0 || at >= chrome_n || !s)
        return;
    char *copy = strdup(s);
    if (!copy)
        return;
    free(chrome_rows[at]);
    chrome_rows[at] = copy;
    dirty = 1;
}

void viewport_chrome_keep(int keep)
{
    if (keep > chrome_n)
        keep = chrome_n;
    for (int i = 0; i < keep; i++) {
        struct item *it = items_push();
        if (it)
            rows_set(it, chrome_rows[i], 0);
        free(chrome_rows[i]);
        chrome_rows[i] = NULL;
    }
    for (int i = keep; i < chrome_n; i++)
        free(chrome_rows[i]);
    chrome_n = 0;
    chrome_caret_col = -1;
    dirty = 1;
    layout_changed();
}

void viewport_chrome_clear(void)
{
    chrome_top = -1;
    for (int i = 0; i < chrome_n; i++)
        free(chrome_rows[i]);
    chrome_n = 0;
    chrome_caret_col = -1;
    dirty = 1;
    layout_changed();
}

void viewport_scroll(int delta)
{
    anchor_id = 0;
    scrolled += delta;
    if (scrolled < 0)
        scrolled = 0;
    dirty = 1;
    layout_changed();
    viewport_paint();
}

void viewport_scroll_end(void)
{
    anchor_id = 0;
    scrolled = 0;
    dirty = 1;
    layout_changed();
    viewport_paint();
}

void viewport_begin(void)
{
    if (active)
        return;
    active = 1;
    direct_str("\x1b[?1049h");
    direct_str(MOUSE_ON);
    fflush(stdout);
    viewport_paint();
}

void viewport_end(void)
{
    if (!active && !handed)
        return;
    active = 0;
    deferred = 0;
    handed = 0;
    suspended = 0;
    direct_str(MOUSE_OFF);
    direct_str("\x1b[?25h");
    direct_str("\x1b[?1049l");
    fflush(stdout);
}

void viewport_handoff(void)
{
    if (!active)
        return;
    active = 0;
    deferred = 0;
    handed = 1;
    suspended = 0;
    direct_str(MOUSE_OFF);
    direct_str("\x1b[?25h");
    fflush(stdout);
}

void viewport_inherit(void)
{
    if (active)
        return;
    active = 1;
    direct_str(MOUSE_ON);
    fflush(stdout);
    viewport_forget();
}

static int dump_item(FILE *f, const struct item *it)
{
    cJSON *line = cJSON_CreateObject();
    if (!line)
        return 0;

    char *state = it->encode && it->kind ? it->encode(it->ud) : NULL;
    if (state) {
        cJSON_AddStringToObject(line, "kind", it->kind);
        cJSON_AddItemToObject(line, "state", cJSON_CreateRaw(state));
        free(state);
    } else {
        cJSON *rows = it->nrows > 0
                          ? cJSON_CreateStringArray((const char *const *)it->rows, it->nrows)
                          : cJSON_CreateArray();
        cJSON_AddStringToObject(line, "kind", "rows");
        cJSON_AddItemToObject(line, "rows", rows);
    }

    char *text = cJSON_PrintUnformatted(line);
    cJSON_Delete(line);
    if (!text)
        return 0;
    int ok = fprintf(f, "%s\n", text) > 0;
    free(text);
    return ok;
}

int viewport_dump(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return 0;

    int ok = 1;
    for (int i = 0; i < nitems; i++)
        ok = dump_item(f, &items[i]) && ok;
    if (open_len) {
        struct item tail = {0};
        rows_set(&tail, open_buf, 0);
        ok = dump_item(f, &tail) && ok;
        rows_free(&tail);
    }
    ok = ferror(f) == 0 && ok;
    return fclose(f) == 0 && ok;
}

void viewport_suspend(void)
{
    if (!active || suspended)
        return;
    suspended = 1;
    deferred = 0;
    direct_str(MOUSE_OFF);
    direct_str("\x1b[?25h");
    direct_str("\x1b[?1049l");
    fflush(stdout);
}

void viewport_resume(void)
{
    if (!active || !suspended)
        return;
    suspended = 0;
    direct_str("\x1b[?1049h");
    direct_str(MOUSE_ON);
    fflush(stdout);
    viewport_forget();
    viewport_paint();
}
