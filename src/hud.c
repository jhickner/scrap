#include "hud.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api.h"
#include "app.h"
#include "gitinfo.h"
#include "models.h"
#include "scrollback.h"
#include "session.h"
#include "tg.h"
#include "relay.h"
#include "voice.h"
#include "ui.h"
#include "viewport.h"
#include "text.h"
#include "workspace.h"
#include "vendor/cJSON.h"

#define SEP    " \xe2\x80\xba "
#define BRANCH "\xee\x82\xa0"

#define SEG_MAX 8

struct seg {
    char        *text;
    enum ui_role role;
};

struct row {
    struct seg seg[SEG_MAX];
    int        n;
};

struct hud {
    struct row row[2];
    int        restarts;

    int        restored;
};

static void row_add(struct row *r, enum ui_role role, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void row_add(struct row *r, enum ui_role role, const char *fmt, ...)
{
    char    text[1024];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);

    if (!text[0] || r->n >= SEG_MAX)
        return;
    char *copy = strdup(text);
    if (!copy)
        return;
    r->seg[r->n].text = copy;
    r->seg[r->n].role = role;
    r->n++;
}

static void row_free(struct row *r)
{
    for (int i = 0; i < r->n; i++)
        free(r->seg[i].text);
    r->n = 0;
}

static void row_paint(const struct row *r, int cols)
{
    size_t budget = (size_t)(cols > 1 ? cols - 1 : 1);

    ui_esc("\x1b[K");
    for (int i = 0; i < r->n && budget > 0; i++) {
        size_t bytes = ui_fit_bytes(r->seg[i].text, budget);
        if (bytes == 0)
            break;
        ui_esc(ui_style(r->seg[i].role));
        ui_putn(r->seg[i].text, bytes);
        budget -= ui_cells_n(r->seg[i].text, bytes);
    }
    ui_esc(ui_style(UI_RESET));
    ui_put("\n");
}

static void row_identity(const struct session *s, struct row *r)
{
    const char *backend = session_backend(s);
    const char *model = models_short_name(backend, session_model_label(s));
    const char *effort = session_effort_label(s);
    const char *chat = tg_label();
    int         count = workspace_count();

    row_add(r, UI_BRAND, UI_BAR " ");
    row_add(r, UI_BOLD, APP_NAME);
    row_add(r, UI_DIM, SEP "%s" SEP "%s%s%s", backend, model,
            effort ? SEP : "", effort ? effort : "");
    if (count > 1)
        row_add(r, UI_ACCENT, SEP "session %d/%d", workspace_index() + 1, count);
    if (chat)
        row_add(r, UI_OK, SEP "%s", chat);
    if (relay_label())
        row_add(r, UI_OK, SEP "%s", relay_label());
    if (voice_label())
        row_add(r, UI_OK, SEP "%s", voice_label());
    if (api_active())
        row_add(r, UI_OK, SEP "api");
}

static void row_location(const struct session *s, struct row *r)
{
    const char *dir = session_workdir(s);

    char path[1024];
    path_home_relative(dir, path, sizeof path);

    const struct gitinfo *g = gitinfo_get(dir);
    int percent = session_context_percent(s);

    row_add(r, UI_BRAND, UI_BAR " ");
    row_add(r, UI_CHROME, "%s", path);
    if (g->repo)
        row_add(r, UI_DIM, " on " BRANCH " %s%s%s%s",
                g->branch[0] ? g->branch : "detached",
                g->sha[0] ? " (" : "", g->sha, g->sha[0] ? ")" : "");
    if (g->added)
        row_add(r, UI_OK, " +%ld", g->added);
    if (g->removed)
        row_add(r, UI_ERROR, " -%ld", g->removed);
    if (g->dirty || g->untracked)
        row_add(r, UI_ERROR, " [%s%s]", g->dirty ? "!" : "",
                g->untracked ? "?" : "");
    if (percent >= 0)
        row_add(r, UI_DIM, " \xc2\xb7 %d%%", percent);

    long in = session_tokens_in(s), out = session_tokens_out(s);
    long cached = session_tokens_cached(s);
    if (in > 0 || out > 0) {
        char got[32], sent[32], hit[32];
        text_humanize(in, got, sizeof got);
        text_humanize(out, sent, sizeof sent);
        text_humanize(cached, hit, sizeof hit);
        if (cached > 0)
            row_add(r, UI_DIM, " \xc2\xb7 %s in (%s cached) / %s out",
                    got, hit, sent);
        else
            row_add(r, UI_DIM, " \xc2\xb7 %s in / %s out", got, sent);
    }
}

static void hud_fill(struct hud *h, const struct session *s)
{
    row_free(&h->row[0]);
    row_free(&h->row[1]);
    row_identity(s, &h->row[0]);
    row_location(s, &h->row[1]);
}

static void hud_render(void *ud, int cols)
{
    const struct hud *h = ud;

    row_paint(&h->row[0], cols);
    row_paint(&h->row[1], cols);
    if (h->restarts <= 0)
        return;
    ui_esc("\x1b[K");
    ui_esc(ui_style(UI_DIM));
    ui_put("  ");
    ui_printf("restarted %dx", h->restarts);
    ui_esc(ui_style(UI_RESET));
    ui_put("\n");
}

static void hud_free(void *ud)
{
    struct hud *h = ud;
    row_free(&h->row[0]);
    row_free(&h->row[1]);
    free(h);
}

static char *hud_encode(void *ud)
{
    const struct hud *h = ud;

    cJSON *st = cJSON_CreateObject();
    if (!st)
        return NULL;
    cJSON_AddNumberToObject(st, "restarts", h->restarts);

    cJSON *rows = cJSON_AddArrayToObject(st, "rows");
    for (int i = 0; rows && i < 2; i++) {
        cJSON *segs = cJSON_CreateArray();
        if (!segs)
            break;
        cJSON_AddItemToArray(rows, segs);
        for (int j = 0; j < h->row[i].n; j++) {
            cJSON *seg = cJSON_CreateObject();
            if (!seg)
                break;
            cJSON_AddStringToObject(seg, "t", h->row[i].seg[j].text);
            cJSON_AddNumberToObject(seg, "r", h->row[i].seg[j].role);
            cJSON_AddItemToArray(segs, seg);
        }
    }

    char *text = cJSON_PrintUnformatted(st);
    cJSON_Delete(st);
    return text;
}

static void hud_place(struct hud *h)
{
    unsigned mark = viewport_item_begin(&(struct viewport_entry){
        .render = hud_render, .ud = h, .free_ud = hud_free, .reflow = 1,
        .pad_before = 1, .pad_after = 1});
    hud_render(h, ui_columns());
    viewport_item_end();
    viewport_item_persist(mark, HUD_KIND, hud_encode);
    ui_flush();
}

void hud_load(const cJSON *st)
{
    struct hud *h = calloc(1, sizeof *h);
    if (!h)
        return;
    h->restarts = scrollback_int(st, "restarts");

    const cJSON *rows = cJSON_GetObjectItem(st, "rows");
    for (int i = 0; i < 2; i++) {
        const cJSON *seg;
        cJSON_ArrayForEach(seg, cJSON_GetArrayItem(rows, i))
            row_add(&h->row[i], (enum ui_role)scrollback_int(seg, "r"), "%s",
                    scrollback_str(seg, "t"));
    }

    h->restored = 1;
    hud_place(h);
}

void hud_print(const struct session *s)
{
    if (!s)
        return;

    unsigned    mark = viewport_item_find(HUD_KIND);
    struct hud *back = mark ? viewport_item_data(mark) : NULL;
    int         restarts = 0;
    if (back && back->restored) {
        back->restored = 0;
        if (viewport_item_last(mark)) {
            hud_fill(back, s);
            viewport_item_update(mark);
            ui_flush();
            return;
        }

        restarts = back->restarts;
        back->restarts = 0;
        viewport_item_update(mark);
    }

    struct hud *h = calloc(1, sizeof *h);
    if (!h)
        return;
    h->restarts = restarts;
    hud_fill(h, s);
    hud_place(h);
}

int hud_restarted(void)
{
    unsigned    mark = viewport_item_find(HUD_KIND);
    struct hud *h = mark ? viewport_item_data(mark) : NULL;
    if (!h)
        return 0;
    h->restarts++;
    viewport_item_update(mark);
    ui_flush();
    return 1;
}
