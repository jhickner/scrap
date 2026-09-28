#include "chrome.h"

#include <string.h>

#include "block.h"
#include "prompt.h"
#include "sidechannel.h"
#include "status.h"
#include "tty.h"
#include "ui.h"
#include "viewport.h"

static struct prompt   *bound;
static chrome_modal_fn  modal;
static void            *modal_ud;
static int              kept;

static int budget;
static int full;
static int spin_row = -1;
static int above_rows;

void chrome_bind(struct prompt *p) { bound = p; }

int chrome_rows_left(void)
{
    int left = budget - ui_sink_rows();
    return left > 0 ? left : 0;
}

static void wipe(void)
{
    spin_row = -1;
    above_rows = 0;
    block_clear();
}

void chrome_clear(void)
{
    if (chrome_modal_active())
        return;
    wipe();
}

void chrome_keep_above(void)
{
    block_keep(above_rows);
    spin_row = -1;
    above_rows = 0;
}

static int (*modal_interrupt)(void);

void chrome_modal_interrupt(int (*fn)(void))
{
    modal_interrupt = fn;
}

int chrome_modal_interrupted(void)
{
    return tty_quit_requested() || (modal_interrupt && modal_interrupt());
}

int chrome_modal_active(void)
{
    return modal != NULL || kept;
}

void chrome_modal(chrome_modal_fn fn, void *ud)
{
    modal = fn;
    modal_ud = ud;
    kept = 0;
    block_pin(fn != NULL);
    if (!fn)
        viewport_defer();
    chrome_paint();
}

void chrome_modal_keep(void)
{
    modal = NULL;
    modal_ud = NULL;
    kept = 1;
}

struct above {
    int side;
    int sticky;
    int queued;
    int voice;
};

struct heights {
    int side;
    int sticky;
    int queued;
    int voice;
};

static const char *(*live_fn)(void);

void chrome_live_label(const char *(*fn)(void))
{
    live_fn = fn;
}

static const char *voice_row(void)
{
    return live_fn ? live_fn() : NULL;
}

static struct heights above_measure(int cols)
{
    struct heights h;
    h.side = sidechannel_rows();
    h.sticky = status_sticky_measure();
    h.queued = prompt_queued_rows(bound, cols);
    h.voice = voice_row() != NULL;
    return h;
}

static int above_height(const struct above *a, const struct heights *h)
{
    int rows = 0;
    int drawn = 0;

    if (a->side && h->side > 0)
        rows += h->side + (drawn++ ? 1 : 0);
    if (a->sticky && h->sticky > 0)
        rows += h->sticky + (drawn++ ? 1 : 0);
    if (a->queued && h->queued > 0)
        rows += h->queued + (drawn++ ? 1 : 0);
    if (a->voice && h->voice > 0)
        rows += h->voice + (drawn++ ? 1 : 0);
    return rows ? rows + 1 : 0;
}

static void fit_above(struct above *a, const struct heights *h, int room)
{
    if (above_height(a, h) <= room)
        return;
    a->queued = 0;
    if (above_height(a, h) <= room)
        return;
    a->sticky = 0;
    if (above_height(a, h) <= room)
        return;
    a->side = 0;
}

int chrome_gap(void)
{
    if (full && modal)
        return 0;
    return viewport_active() && !viewport_ends_blank();
}

void chrome_full(int on)
{
    full = on ? 1 : 0;
}

int chrome_modal_rows(void)
{
    return full ? tty_rows() : tty_rows() - 2 - chrome_gap();
}

void chrome_title_paint(const char *title)
{
    int    columns = ui_columns();
    size_t budget = columns > 3 ? (size_t)(columns - 3) : 1;
    size_t fit = ui_fit_visible(title, strlen(title), budget);

    ui_esc(ui_style(UI_CHROME));
    ui_put(UI_BAR);
    ui_esc(ui_style(UI_RESET));
    ui_put(" ");
    ui_esc(ui_style(UI_DIM));
    ui_putn(title, fit);
    if (title[fit])
        ui_put("\u2026");
    ui_esc(ui_style(UI_RESET));
    ui_put("\n");
}

static size_t ask_budget(int columns)
{
    return columns > 12 ? (size_t)(columns - 12) : 1;
}

int chrome_foot_rows(const char *ask, const char *hint, int columns)
{
    if (ask && *ask) {
        struct ui_wrap w = {0};
        w.budget = ask_budget(columns);
        w.measure = 1;
        w.paint_empty = 1;
        return 1 + ui_wrap_paint(ask, &w);
    }
    if (hint && *hint) {
        int rows = 2;
        for (const char *p = hint; (p = strchr(p, '\n')); p++)
            rows++;
        return rows;
    }
    return 0;
}

void chrome_foot_paint(const char *ask, const char *hint, int columns)
{
    if (ask && *ask) {
        const char *p = ask;
        size_t      n = strlen(p);
        size_t      wide = ask_budget(columns);
        ui_put("\n");
        while (n) {
            size_t skip = 0;
            size_t row = ui_wrap_row(p, n, wide, &skip, NULL);
            size_t used = row + skip;
            ui_put("    ");
            ui_putn(p, row);
            p += used;
            n -= used < n ? used : n;
            if (n)
                ui_put("\n");
        }
        ui_put(" ");
        ui_esc(ui_style(UI_ACCENT));
        ui_put("y/n");
        ui_esc(ui_style(UI_RESET));
        return;
    }
    if (!hint || !*hint)
        return;

    ui_put("\n");
    size_t wide = columns > 6 ? (size_t)(columns - 6) : 1;
    for (const char *p = hint; p;) {
        const char *nl = strchr(p, '\n');
        size_t      n = nl ? (size_t)(nl - p) : strlen(p);
        ui_esc(ui_style(UI_DIM));
        ui_put("    ");
        ui_putn(p, ui_fit_visible(p, n, wide));
        ui_esc(ui_style(UI_RESET));
        if (!nl)
            break;
        ui_put("\n");
        p = nl + 1;
    }
}

int chrome_read_yesno(const tty_event *ev)
{
    if (ev->key == TK_CHAR && (ev->cp == 'y' || ev->cp == 'Y'))
        return 1;
    if (ev->key == TK_CHAR &&
        (ev->cp == 'n' || ev->cp == 'N' || ev->cp == 3 || ev->cp == 4))
        return 0;
    if (ev->key == TK_ESCAPE || ev->key == TK_EOF)
        return 0;
    return -1;
}

void chrome_paint(void)
{
    if (kept)
        return;

    if (ui_too_narrow()) {
        wipe();
        return;
    }

    if (modal) {
        block_begin();
        budget = tty_rows() - 1;
        spin_row = -1;
            above_rows = 0;
        if (chrome_gap())
            ui_put("\n");
        block_fill(full);
        modal(modal_ud);
        block_end(0, -1);
        return;
    }

    if (!bound) {
        wipe();
        return;
    }

    int cols = ui_columns();
    int spinning = status_spinning();

    int input_rows = prompt_input_rows(bound, cols);
    int gap = chrome_gap();

    struct heights h = above_measure(cols);
    struct above a = {1, 1, 1, 1};
    fit_above(&a, &h, tty_rows() - 1 - input_rows - spinning - gap);

    block_begin();
    block_fill(0);
    budget = tty_rows() - 1;
    spin_row = -1;

    if (gap)
        ui_put("\n");

    int drawn = 0;
    if (a.side && h.side > 0) {
        sidechannel_paint(chrome_rows_left());
        drawn = 1;
    }
    if (a.sticky && h.sticky > 0) {
        if (drawn)
            ui_put("\n");
        status_paint_sticky();
        drawn = 1;
    }
    if (a.queued && h.queued > 0) {
        if (drawn)
            ui_put("\n");
        prompt_paint_queued(bound, chrome_rows_left());
        drawn = 1;
    }
    if (a.voice && h.voice > 0) {
        if (drawn)
            ui_put("\n");
        ui_esc(UI_ERASE_EOL);
        ui_esc(ui_style(UI_DIM));
        ui_put(voice_row());
        ui_esc(ui_style(UI_RESET));
        ui_put("\n");
    }

    if (ui_sink_rows() - gap > 0) {
        ui_esc(UI_ERASE_EOL);
        ui_put("\n");
    }

    above_rows = ui_sink_rows() - gap;

    if (spinning) {
        spin_row = ui_sink_rows();
        status_paint_spin();
        ui_put("\n");
    }

    int caret_row = 0, caret_col = -1;
    int first = ui_sink_rows();
    prompt_paint_input(bound, input_rows, &caret_row, &caret_col);

    block_end(first + caret_row, caret_col);
}

int chrome_paint_spin(void)
{
    if (spin_row < 0 || !status_spinning() || ui_columns() < 24 || !block_have())
        return 0;
    block_row_begin(spin_row);
    status_paint_spin();
    block_row_end();
    return 1;
}
