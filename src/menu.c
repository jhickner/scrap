#include "menu.h"

#include <stdio.h>
#include <string.h>

#include "ui.h"

#define BOX_TL "\xe2\x95\xad"
#define BOX_TR "\xe2\x95\xae"
#define BOX_BL "\xe2\x95\xb0"
#define BOX_BR "\xe2\x95\xaf"
#define BOX_H  "\xe2\x94\x80"
#define BOX_V  "\xe2\x94\x82"
#define BOX_ML "\xe2\x94\x9c"
#define BOX_MR "\xe2\x94\xa4"

/* the borders, the space inside them and the cursor an item leaves room for */
#define MENU_TRIM 6

void menu_clear(struct menu *m)
{
    m->n = 0;
    m->sel = 0;
    m->open = 0;
}

int menu_add(struct menu *m, const char *label, int apart)
{
    if (m->n >= MENU_MAX)
        return 0;
    snprintf(m->item[m->n], sizeof m->item[0], "%s", label);
    m->apart[m->n] = (unsigned char)!!apart;
    m->n++;
    return 1;
}

void menu_step(struct menu *m, int dir)
{
    if (m->n < 1)
        return;
    m->sel += dir;
    if (m->sel >= m->n)
        m->sel = 0;
    else if (m->sel < 0)
        m->sel = m->n - 1;
}

int menu_rows(const struct menu *m)
{
    int rows = m->n + 2;
    for (int i = 0; i < m->n; i++)
        rows += m->apart[i];
    return rows;
}

int menu_width(const struct menu *m)
{
    size_t wide = 0;
    for (int i = 0; i < m->n; i++) {
        size_t cells = ui_cells(m->item[i]);
        if (cells > wide)
            wide = cells;
    }
    return (int)wide + MENU_TRIM;
}

static int item_of(const struct menu *m, int row, int *rule)
{
    int at = 1;
    *rule = 0;
    for (int i = 0; i < m->n; i++) {
        if (m->apart[i]) {
            if (row == at) {
                *rule = 1;
                return -1;
            }
            at++;
        }
        if (row == at)
            return i;
        at++;
    }
    return -1;
}

static void put_rule(const char *left, const char *right, int width)
{
    ui_esc(ui_style(UI_ACCENT));
    ui_put(left);
    for (int i = 2; i < width; i++)
        ui_put(BOX_H);
    if (width > 1)
        ui_put(right);
    ui_esc(ui_style(UI_RESET));
}

void menu_paint_row(const struct menu *m, int row, int width)
{
    int rows = menu_rows(m);
    if (row < 0 || row >= rows || width < MENU_TRIM)
        return;

    if (row == 0) {
        put_rule(BOX_TL, BOX_TR, width);
        return;
    }
    if (row == rows - 1) {
        put_rule(BOX_BL, BOX_BR, width);
        return;
    }

    int rule = 0;
    int at = item_of(m, row, &rule);
    if (rule) {
        put_rule(BOX_ML, BOX_MR, width);
        return;
    }

    ui_esc(ui_style(UI_ACCENT));
    ui_put(BOX_V);
    ui_esc(ui_style(UI_RESET));

    int picked = at == m->sel;
    ui_esc(ui_style(picked ? UI_ACCENT : UI_RESET));
    ui_put(picked ? " \xe2\x86\x92 " : "   ");

    size_t budget = (size_t)(width - MENU_TRIM);
    size_t fit = at >= 0 ? ui_fit_bytes(m->item[at], budget) : 0;
    if (at >= 0)
        ui_putn(m->item[at], fit);
    ui_esc(ui_style(UI_RESET));

    int used = 4 + (at >= 0 ? (int)ui_cells_n(m->item[at], fit) : 0);
    if (width - 1 > used)
        ui_pad(width - 1 - used);

    ui_esc(ui_style(UI_ACCENT));
    ui_put(BOX_V);
    ui_esc(ui_style(UI_RESET));
}
