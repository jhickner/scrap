#include "confirm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chrome.h"
#include "frontend.h"
#include "highlight.h"
#include "session.h"
#include "toolstyle.h"
#include "tty.h"
#include "ui.h"

#define DETAIL_ROWS 12

struct ask {
    const char              *from;
    const struct permission *p;
};

static void bar(void)
{
    ui_esc(ui_style(UI_CHROME));
    ui_put(UI_BAR);
    ui_esc(ui_style(UI_RESET));
    ui_put(" ");
}

static void paint_question(void *ud)
{
    const char *p = ud;
    size_t      n = strlen(p);
    int         columns = ui_columns();
    size_t      budget = columns > 8 ? (size_t)(columns - 8) : 1;

    bar();
    while (n) {
        size_t skip = 0;
        size_t row = ui_wrap_row(p, n, budget, &skip, NULL);
        size_t used = row + skip;
        ui_putn(p, row);
        p += used;
        n -= used < n ? used : n;
        if (n)
            ui_put("\n  ");
    }
    ui_put(" ");
    ui_esc(ui_style(UI_ACCENT));
    ui_put("y/n");
    ui_esc(ui_style(UI_RESET));
}

static void paint_detail(const char *tool, const char *text)
{
    size_t         n = strlen(text);
    int            columns = ui_columns();
    size_t         budget = columns > 8 ? (size_t)(columns - 6) : 1;
    unsigned char *spans = NULL;

    if (toolstyle_is_shell(tool) && (spans = malloc(n)))
        highlight_shell(text, n, spans);

    const char *p = text;
    for (int rows = 0; n; rows++) {
        size_t skip = 0;
        size_t row = ui_wrap_row(p, n, budget, &skip, NULL);
        bar();
        ui_put("  ");
        if (rows == DETAIL_ROWS - 1 && row + skip < n) {
            ui_esc(ui_style(UI_DIM));
            ui_put("\xe2\x80\xa6");
            ui_esc(ui_style(UI_RESET));
            ui_put("\n");
            break;
        }
        if (spans)
            ui_put_spans(p, row, spans + (p - text), UI_RESET);
        else
            ui_putn(p, row);
        ui_esc(ui_style(UI_RESET));
        ui_put("\n");
        p += row + skip;
        n -= row + skip < n ? row + skip : n;
    }
    free(spans);
}

static void key(const char *k, const char *word)
{
    ui_esc(ui_style(UI_ACCENT));
    ui_put(k);
    ui_esc(ui_style(UI_DIM));
    ui_put(" ");
    ui_put(word);
    ui_esc(ui_style(UI_RESET));
}

static void paint_permission(void *ud)
{
    const struct ask *a = ud;
    char head[512];
    size_t budget = ui_columns() > 8 ? (size_t)(ui_columns() - 4) : 1;

    bar();
    if (a->from) {
        ui_esc(ui_style(UI_DIM));
        ui_put(a->from);
        ui_put(" \xc2\xb7 ");
    }
    ui_esc(ui_style(UI_TOOL));
    ui_put(a->p->tool);
    ui_esc(ui_style(UI_RESET));
    if (a->p->about) {
        snprintf(head, sizeof head, " \xc2\xb7 %s", a->p->about);
        size_t used = ui_cells(a->p->tool) + (a->from ? ui_cells(a->from) + 3 : 0);
        size_t room = budget > used ? budget - used : 0;
        size_t fit = ui_fit_visible(head, strlen(head), room);
        ui_esc(ui_style(UI_DIM));
        ui_putn(head, fit);
        if (fit < strlen(head))
            ui_put("\xe2\x80\xa6");
        ui_esc(ui_style(UI_RESET));
    }
    ui_put("\n");
    if (a->p->detail)
        paint_detail(a->p->tool, a->p->detail);
    bar();
    key("y", "allow");
    ui_esc(ui_style(UI_DIM));
    ui_put("  ");
    ui_esc(ui_style(UI_RESET));
    key("n", "deny");
}

static int modal_yesno(chrome_modal_fn paint, void *ud)
{
    if (!frontend_has_keyboard() || !tty_is_raw())
        return 0;

    /* A prompt can open over another one (a second tab asking while the
     * first waits), and closing it must leave the first one drawn. */
    void *under_ud;
    chrome_modal_fn under = chrome_modal_current(&under_ud);
    chrome_modal(paint, ud);
    for (;;) {
        if (tty_quit_requested() || chrome_modal_interrupted()) {
            chrome_modal(under, under_ud);
            return 0;
        }
        tty_event ev;
        if (!tty_read(&ev, -1)) {
            if (!chrome_modal_interrupted())
                continue;
            chrome_modal(under, under_ud);
            return 0;
        }
        if (ev.key == TK_TEXT)
            free(ev.text);
        int yn = chrome_read_yesno(&ev);
        if (yn == 1) {
            chrome_modal(under, under_ud);
            return 1;
        }
        if (yn == 0) {
            chrome_modal(under, under_ud);
            return 0;
        }
        if (ev.key == TK_RESIZE)
            chrome_paint();
    }
}

int confirm_run(const char *question)
{
    if (!question || !*question)
        return 0;
    return modal_yesno(paint_question, (void *)question);
}

int confirm_permission(const char *from, const struct permission *p)
{
    if (!p || !p->tool)
        return 0;
    struct ask a = {from, p};
    return modal_yesno(paint_permission, &a);
}
