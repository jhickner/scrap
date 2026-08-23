#include "form.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chrome.h"
#include "frontend.h"
#include "md.h"
#include "replframe.h"
#include "replkeys.h"
#include "tty.h"
#include "ui.h"
#include "viewport.h"

#define FORM_FIELDS 12

#define FORM_INDENT 2

#define HIT_MAX 128

struct slot {
    Repl repl;
    int  choice;
    int  rows;
};

struct line {
    int                   field;
    const struct md_text *note;
    size_t                from, len;
    int                   row;
    int                   indent;
};

struct lines {
    struct line *v;
    int          n, cap;
};

struct state {
    struct form     *form;
    struct md_text **notes;
    struct lines     lines;
    struct slot      slots[FORM_FIELDS];
    int              focus;
    int              label_width;
    int              top;
    int              pinned; /* the view is where the user scrolled it */
    int              rows, room;
    int              budget;

    struct replframe frame;
    int              framed;

    short hit[HIT_MAX];
};

static struct form_field *field_at(struct state *st, int i)
{
    return &st->form->fields[i];
}

static const char *slot_shown(const struct state *st, int i)
{
    const struct form_field *f = &st->form->fields[i];
    if (f->kind == FORM_CHOICE) {
        int at = st->slots[i].choice;
        if (at < 0 || at >= f->choices_n)
            return "";
        return f->choices[at];
    }
    return repl_line(&st->slots[i].repl);
}

static void cycle(struct state *st, int i, int delta)
{
    const struct form_field *f = &st->form->fields[i];
    if (f->kind != FORM_CHOICE || f->choices_n <= 0)
        return;
    st->slots[i].choice = (st->slots[i].choice + delta) % f->choices_n;
    if (st->slots[i].choice < 0)
        st->slots[i].choice += f->choices_n;
}

static void focus_step(struct state *st, int delta)
{
    int n = st->form->fields_n;
    if (n <= 0)
        return;
    st->pinned = 0;
    st->focus = (st->focus + delta) % n;
    if (st->focus < 0)
        st->focus += n;
}

static int value_column(const struct state *st)
{
    return FORM_INDENT + st->label_width + 4;
}

static int value_budget(const struct state *st, int columns)
{
    int budget = columns - value_column(st) - 2;
    return budget < 8 ? 8 : budget;
}

#define REPL_GUTTER 2

static int repl_width(const struct state *st)
{
    return st->budget + REPL_GUTTER;
}

static struct line *line_add(struct lines *l)
{
    if (l->n == l->cap) {
        int          cap = l->cap ? l->cap * 2 : 128;
        struct line *grown = realloc(l->v, (size_t)cap * sizeof *grown);
        if (!grown)
            return NULL;
        l->v = grown;
        l->cap = cap;
    }
    struct line *r = &l->v[l->n];
    memset(r, 0, sizeof *r);
    l->n++;
    return r;
}

static void wrap_notes(struct lines *out, const struct md_text *note, int budget)
{
    size_t      rest = 0;
    const char *text = md_text_plain(note, &rest);
    size_t      at = 0;

    if (!rest) {
        struct line *l = line_add(out);
        if (l)
            l->field = -1;
        return;
    }
    /* What a note indents itself by, its wrapped rows keep. */
    size_t lead = 0;
    while (lead < rest && text[lead] == ' ')
        lead++;
    if ((int)lead > budget / 2)
        lead = 0;

    int first = 1;
    while (at < rest) {
        int room = first ? budget : budget - (int)lead;
        if (room < 8)
            room = 8;
        size_t skip = 0;
        size_t got = ui_wrap_row(text + at, rest - at, (size_t)room, &skip, NULL);
        struct line *l = line_add(out);
        if (!l)
            return;
        l->field = -1;
        l->note = note;
        l->from = at;
        l->len = got;
        l->indent = first ? 0 : (int)lead;
        at += got + skip;
        first = 0;
    }
}

static int layout(struct state *st, int columns)
{
    const struct form *form = st->form;
    struct lines      *out = &st->lines;

    out->n = 0;

    int note_budget = columns - FORM_INDENT - 2;
    if (note_budget < 8)
        note_budget = 8;

    for (int i = 0; i < form->notes_n; i++)
        wrap_notes(out, st->notes ? st->notes[i] : NULL, note_budget);
    if (form->notes_n) {
        struct line *l = line_add(out);
        if (l)
            l->field = -1;
    }

    st->budget = value_budget(st, columns);

    for (int i = 0; i < form->fields_n; i++) {
        if (form->fields[i].kind == FORM_BUTTON) {
            if (i == 0 || form->fields[i - 1].kind != FORM_BUTTON) {
                struct line *gap = line_add(out);
                if (gap)
                    gap->field = -1;
            }
            st->slots[i].rows = 1;
            struct line *l = line_add(out);
            if (l)
                l->field = i;
            continue;
        }
        if (form->fields[i].kind == FORM_CHOICE) {
            st->slots[i].rows = 1;
            struct line *l = line_add(out);
            if (l)
                l->field = i;
            continue;
        }

        int rows = repl_input_rows(&st->slots[i].repl, repl_width(st));
        if (rows < 1)
            rows = 1;
        st->slots[i].rows = rows;
        for (int r = 0; r < rows; r++) {
            struct line *l = line_add(out);
            if (!l)
                break;
            l->field = i;
            l->row = r;
        }
    }
    return out->n;
}

static void put_label(const struct state *st, const char *label, int focused)
{
    ui_pad(FORM_INDENT);
    ui_esc(ui_style(focused ? UI_ACCENT : UI_DIM));
    ui_put(label);
    ui_esc(ui_style(UI_RESET));
    int pad = st->label_width - (int)ui_cells(label) + 2;
    ui_pad(pad > 1 ? pad : 1);
}

static void put_gutter(const struct state *st, int i, int focused)
{
    if (st->form->fields[i].kind == FORM_CHOICE && focused) {
        ui_esc(ui_style(UI_ACCENT));
        ui_put("\xe2\x80\xb9 ");
        ui_esc(ui_style(UI_RESET));
        return;
    }
    ui_pad(2);
}

static void put_choice(const struct state *st, int i, int focused)
{
    const char *shown = slot_shown(st, i);

    ui_esc(ui_style(UI_TEXT));
    ui_put(shown && *shown ? shown : "\xe2\x80\x94");
    ui_esc(ui_style(UI_RESET));
    if (focused) {
        ui_esc(ui_style(UI_ACCENT));
        ui_put(" \xe2\x80\xba");
        ui_esc(ui_style(UI_RESET));
    }
}

static int framed(struct state *st, int i)
{
    if (st->framed == i)
        return 1;
    st->framed = -1;
    if (!replframe_render(&st->frame, &st->slots[i].repl, st->slots[i].rows,
                          repl_width(st), 1))
        return 0;
    st->framed = i;
    return 1;
}

static void put_value_row(struct state *st, int i, int row, int focused)
{
    if (!framed(st, i))
        return;

    const Repl *r = &st->slots[i].repl;
    replframe_paint_row(&st->frame, row, REPL_GUTTER, focused,
                        r->cursor >= r->len || r->buf[r->cursor] == '\n');
}

static int room_for(void)
{
    int rows = tty_rows() - 4 - chrome_gap();
    return rows < 3 ? 3 : rows;
}

static void paint(void *ud)
{
    struct state *st = ud;
    struct form  *form = st->form;
    int           columns = ui_columns();

    st->framed = -1;
    int                n = layout(st, columns);
    const struct line *lines = st->lines.v;
    int room = room_for();
    int base = chrome_gap();

    for (int i = 0; i < HIT_MAX; i++)
        st->hit[i] = -1;

    st->rows = n;
    st->room = room;

    if (n <= room) {
        st->top = 0;
    } else if (st->pinned) {
        if (st->top > n - room)
            st->top = n - room;
        if (st->top < 0)
            st->top = 0;
    } else {
        int first = -1, last = -1, caret = -1;
        for (int i = 0; i < n; i++) {
            if (lines[i].field != st->focus)
                continue;
            if (first < 0)
                first = i;
            last = i;
        }
        if (first >= 0 && field_at(st, st->focus)->kind == FORM_TEXT &&
            framed(st, st->focus) && st->frame.have_cursor)
            caret = first + st->frame.cursor_y;

        if (first >= 0) {
            if (first < st->top)
                st->top = first;
            if (last >= st->top + room)
                st->top = last - room + 1;
        }

        if (caret >= 0) {
            if (caret < st->top)
                st->top = caret;
            if (caret >= st->top + room)
                st->top = caret - room + 1;
        }
        if (st->top > n - room)
            st->top = n - room;
        if (st->top < 0)
            st->top = 0;
    }

    ui_esc(ui_style(UI_CHROME));
    ui_put(UI_BAR);
    ui_esc(ui_style(UI_RESET));
    ui_put(" ");
    ui_esc(ui_style(UI_DIM));
    {
        char        said[256];
        const char *title = form->title ? form->title : "";
        if (n > room)
            snprintf(said, sizeof said, "%s \xc2\xb7 %d\xe2\x80\x93%d of %d",
                     title, st->top + 1, st->top + room, n);
        else
            snprintf(said, sizeof said, "%s", title);
        size_t fit = ui_fit_bytes(said, (size_t)(columns > 3 ? columns - 3 : 1));
        ui_putn(said, fit);
    }
    ui_esc(ui_style(UI_RESET));
    ui_put("\n");

    int end = n <= room ? n : st->top + room;
    for (int i = st->top; i < end; i++) {
        const struct line *l = &lines[i];
        int                at = base + 1 + i - st->top;

        if (l->field >= 0 && at >= 0 && at < HIT_MAX)
            st->hit[at] = (short)l->field;

        if (l->field < 0) {
            if (l->len) {
                ui_pad(FORM_INDENT + l->indent);
                md_text_put(l->note, l->from, l->len, UI_DIM);
            }
            ui_put("\n");
            continue;
        }

        struct form_field *f = field_at(st, l->field);
        int                focused = l->field == st->focus;

        if (f->kind == FORM_BUTTON) {
            ui_pad(FORM_INDENT);
            ui_esc(ui_style(focused ? UI_ACCENT : UI_DIM));
            ui_put(f->label ? f->label : "");
            ui_esc(ui_style(UI_RESET));
            ui_put("\n");
            continue;
        }

        if (i == st->top || lines[i - 1].field != l->field) {
            put_label(st, f->label ? f->label : "", focused);
            put_gutter(st, l->field, focused);
        } else {
            ui_pad(value_column(st));
        }

        if (f->kind == FORM_CHOICE)
            put_choice(st, l->field, focused);
        else
            put_value_row(st, l->field, l->row, focused);
        ui_put("\n");
    }

    ui_put("\n");
    ui_esc(ui_style(UI_DIM));
    ui_pad(FORM_INDENT);
    ui_put("tab next field  \xc2\xb7  shift-enter newline  \xc2\xb7  "
           "enter confirm  \xc2\xb7  esc cancel");
    ui_esc(ui_style(UI_RESET));
}

static void load(struct state *st)
{
    st->framed = -1;
    if (st->form->notes_n > 0) {
        st->notes = calloc((size_t)st->form->notes_n, sizeof *st->notes);
        for (int i = 0; st->notes && i < st->form->notes_n; i++)
            st->notes[i] = md_text_parse(st->form->notes[i]);
    }

    for (int i = 0; i < st->form->fields_n; i++) {
        struct form_field *f = field_at(st, i);
        struct slot       *s = &st->slots[i];

        repl_init(&s->repl, NULL, 0);

        if (f->kind == FORM_BUTTON)
            continue;
        if (f->kind == FORM_CHOICE) {
            s->choice = 0;
            for (int c = 0; c < f->choices_n; c++)
                if (f->choices[c] && f->value && !strcmp(f->choices[c], f->value)) {
                    s->choice = c;
                    break;
                }
        } else if (f->value && *f->value) {
            repl_insert_text(&s->repl, f->value);
        }

        int cells = (int)ui_cells(f->label ? f->label : "");
        if (cells > st->label_width)
            st->label_width = cells;
    }
}

static void unload(struct state *st)
{
    for (int i = 0; i < st->form->fields_n; i++)
        repl_free(&st->slots[i].repl);
    for (int i = 0; st->notes && i < st->form->notes_n; i++)
        md_text_free(st->notes[i]);
    free(st->notes);
    st->notes = NULL;
    free(st->lines.v);
    memset(&st->lines, 0, sizeof st->lines);
    replframe_free(&st->frame);
}

static void store(struct state *st)
{
    for (int i = 0; i < st->form->fields_n; i++) {
        struct form_field *f = field_at(st, i);
        if (!f->value || !f->size || f->kind == FORM_BUTTON)
            continue;
        snprintf(f->value, f->size, "%s", slot_shown(st, i));
    }
}

static int press_button(struct state *st)
{
    struct form_field *f = field_at(st, st->focus);
    if (f->kind != FORM_BUTTON)
        return 0;
    if (f->value && f->size)
        snprintf(f->value, f->size, "1");
    chrome_modal(NULL, NULL);
    unload(st);
    return 1;
}

static int feed(struct state *st, const ReplEvent *ev)
{
    struct slot *s = &st->slots[st->focus];

    st->budget = value_budget(st, ui_columns());
    repl_set_width(&s->repl, repl_width(st));
    st->framed = -1;
    st->pinned = 0;
    return repl_handle_input(&s->repl, ev);
}

static void scroll_by(struct state *st, int rows)
{
    if (st->rows <= st->room)
        return;
    st->pinned = 1;
    st->top += rows;
    if (st->top > st->rows - st->room)
        st->top = st->rows - st->room;
    if (st->top < 0)
        st->top = 0;
}

static void step_or_leave(struct state *st, int delta)
{
    struct slot *s = &st->slots[st->focus];
    if (field_at(st, st->focus)->kind != FORM_TEXT) {
        focus_step(st, delta);
        return;
    }
    int       was = s->repl.cursor;
    ReplEvent ev = {.key = delta < 0 ? REPL_KEY_UP : REPL_KEY_DOWN};
    feed(st, &ev);
    if (s->repl.cursor == was)
        focus_step(st, delta);
}

int form_run(struct form *form)
{
    if (!form || form->fields_n <= 0 || form->fields_n > FORM_FIELDS)
        return 0;
    if (!frontend_has_keyboard() || !tty_is_raw())
        return 0;

    struct state st = {.form = form};
    load(&st);

    chrome_modal(paint, &st);
    for (;;) {
        tty_event ev;
        if (!tty_read(&ev, -1)) {
            if (!chrome_modal_interrupted())
                continue;
            chrome_modal(NULL, NULL);
            unload(&st);
            return 0;
        }

        struct form_field *f = field_at(&st, st.focus);
        int                typing = f->kind == FORM_TEXT;
        int                button = f->kind == FORM_BUTTON;

        if (ev.key == TK_CHAR && ev.cp == 3) {
            chrome_modal(NULL, NULL);
            unload(&st);
            return 0;
        }

        switch (ev.key) {
        case TK_UP:
            step_or_leave(&st, -1);
            break;
        case TK_DOWN:
            step_or_leave(&st, 1);
            break;

        case TK_TAB:
            focus_step(&st, 1);
            break;

        case TK_PAGE_UP:
            scroll_by(&st, -(st.room > 1 ? st.room - 1 : 1));
            break;

        case TK_PAGE_DOWN:
            scroll_by(&st, st.room > 1 ? st.room - 1 : 1);
            break;

        case TK_SCROLL_UP:
            scroll_by(&st, -3);
            break;

        case TK_SCROLL_DOWN:
            scroll_by(&st, 3);
            break;

        case TK_MOUSE_DOWN: {
            int top = viewport_chrome_top();
            if (top < 0)
                continue;
            int at = ev.row - 1 - top;
            if (at < 0 || at >= HIT_MAX || st.hit[at] < 0)
                continue;
            st.focus = st.hit[at];
            if (press_button(&st))
                return 1;
            break;
        }

        case TK_ENTER:
            if (press_button(&st))
                return 1;
            store(&st);
            chrome_modal(NULL, NULL);
            unload(&st);
            return 1;

        case TK_ESCAPE:
        case TK_EOF:
            chrome_modal(NULL, NULL);
            unload(&st);
            return 0;

        default: {
            if (!typing) {
                if (button && ev.key == TK_CHAR && ev.cp == ' ') {
                    free(ev.text);
                    if (press_button(&st))
                        return 1;
                    break;
                }
                if (ev.key == TK_CHAR && ev.cp == ' ')
                    cycle(&st, st.focus, 1);
                else if (ev.key == TK_LEFT || ev.key == TK_RIGHT)
                    cycle(&st, st.focus, ev.key == TK_LEFT ? -1 : 1);
                free(ev.text);
                break;
            }
            ReplEvent re;
            if (replkeys_map(&ev, &re))
                feed(&st, &re);
            free(ev.text);
            break;
        }
        }
        chrome_paint();
    }
}
