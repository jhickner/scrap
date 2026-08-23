#include "form.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chrome.h"
#include "frontend.h"
#include "replframe.h"
#include "tty.h"
#include "ui.h"

#define FORM_FIELDS 12

#define FORM_INDENT 2

struct slot {
    Repl repl;
    int  choice;
    int  rows;
};

struct state {
    struct form *form;
    struct slot  slots[FORM_FIELDS];
    int          focus;
    int          label_width;
    int          top;
    int          budget;

    struct replframe frame;
    int              framed;
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
    st->focus = (st->focus + delta) % n;
    if (st->focus < 0)
        st->focus += n;
}

struct line {
    int         field;
    const char *text;
    size_t      len;
    int         row;
};

#define LINES_MAX 512

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

static int wrap_notes(struct line *out, int n, int max, const char *text,
                      int budget)
{
    size_t rest = text ? strlen(text) : 0;
    if (!rest) {
        if (n < max)
            out[n++] = (struct line){-1, "", 0, 0};
        return n;
    }
    while (rest && n < max) {
        size_t skip = 0;
        size_t got = ui_wrap_row(text, rest, (size_t)budget, &skip, NULL);
        out[n++] = (struct line){-1, text, got, 0};
        text += got + skip;
        rest -= got + skip;
    }
    return n;
}

static int layout(struct state *st, int columns, struct line *out, int max)
{
    const struct form *form = st->form;
    int                n = 0;

    int note_budget = columns - FORM_INDENT - 2;
    if (note_budget < 8)
        note_budget = 8;

    for (int i = 0; i < form->notes_n; i++)
        n = wrap_notes(out, n, max, form->notes[i] ? form->notes[i] : "",
                       note_budget);
    if (form->notes_n && n < max)
        out[n++] = (struct line){-1, "", 0, 0};

    st->budget = value_budget(st, columns);

    for (int i = 0; i < form->fields_n && n < max; i++) {
        if (form->fields[i].kind == FORM_CHOICE) {
            st->slots[i].rows = 1;
            out[n++] = (struct line){i, NULL, 0, 0};
            continue;
        }

        int rows = repl_input_rows(&st->slots[i].repl, repl_width(st));
        if (rows < 1)
            rows = 1;
        st->slots[i].rows = rows;
        for (int r = 0; r < rows && n < max; r++)
            out[n++] = (struct line){i, NULL, 0, r};
    }
    return n;
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

    static struct line lines[LINES_MAX];
    st->framed = -1;
    int n = layout(st, columns, lines, LINES_MAX);
    int room = room_for();

    if (n <= room) {
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

        if (l->field < 0) {
            if (l->len) {
                ui_pad(FORM_INDENT);
                ui_esc(ui_style(UI_DIM));
                ui_putn(l->text, l->len);
                ui_esc(ui_style(UI_RESET));
            }
            ui_put("\n");
            continue;
        }

        struct form_field *f = field_at(st, l->field);
        int                focused = l->field == st->focus;

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
    for (int i = 0; i < st->form->fields_n; i++) {
        struct form_field *f = field_at(st, i);
        struct slot       *s = &st->slots[i];

        repl_init(&s->repl, NULL, 0);

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
    replframe_free(&st->frame);
}

static void store(struct state *st)
{
    for (int i = 0; i < st->form->fields_n; i++) {
        struct form_field *f = field_at(st, i);
        if (!f->value || !f->size)
            continue;
        snprintf(f->value, f->size, "%s", slot_shown(st, i));
    }
}

static int feed(struct state *st, ReplKey key, uint32_t cp, const char *text)
{
    struct slot *s = &st->slots[st->focus];
    ReplEvent    ev = {.key = key, .codepoint = cp, .text = text};

    st->budget = value_budget(st, ui_columns());
    repl_set_width(&s->repl, repl_width(st));
    st->framed = -1;
    return repl_handle_input(&s->repl, &ev);
}

static void step_or_leave(struct state *st, int delta)
{
    struct slot *s = &st->slots[st->focus];
    if (field_at(st, st->focus)->kind != FORM_TEXT) {
        focus_step(st, delta);
        return;
    }
    int was = s->repl.cursor;
    feed(st, delta < 0 ? REPL_KEY_UP : REPL_KEY_DOWN, 0, NULL);
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

        switch (ev.key) {
        case TK_TEXT:

            if (typing && ev.text) {
                st.framed = -1;
                repl_insert_text(&st.slots[st.focus].repl, ev.text);
            }
            free(ev.text);
            break;

        case TK_CHAR:

            if (ev.cp == 3) {
                chrome_modal(NULL, NULL);
                unload(&st);
                return 0;
            }
            if (!typing) {
                if (ev.cp == ' ')
                    cycle(&st, st.focus, 1);
                break;
            }
            feed(&st, REPL_KEY_CHAR, ev.cp, NULL);
            break;

        case TK_LEFT:
        case TK_RIGHT:
            if (typing)
                feed(&st, ev.key == TK_LEFT ? REPL_KEY_LEFT : REPL_KEY_RIGHT, 0, NULL);
            else
                cycle(&st, st.focus, ev.key == TK_LEFT ? -1 : 1);
            break;

        case TK_UP:
            step_or_leave(&st, -1);
            break;
        case TK_DOWN:
            step_or_leave(&st, 1);
            break;

        case TK_TAB:
            focus_step(&st, 1);
            break;

        case TK_NEWLINE:
            if (typing)
                feed(&st, REPL_KEY_NEWLINE, 0, NULL);
            break;

        case TK_ENTER:
            store(&st);
            chrome_modal(NULL, NULL);
            unload(&st);
            return 1;

        case TK_ESCAPE:
        case TK_EOF:
            chrome_modal(NULL, NULL);
            unload(&st);
            return 0;

        default:
            if (typing) {
                static const ReplKey MAP[] = {
                    [TK_BACKSPACE] = REPL_KEY_BACKSPACE,
                    [TK_WORD_LEFT] = REPL_KEY_WORD_LEFT,
                    [TK_WORD_RIGHT] = REPL_KEY_WORD_RIGHT,
                };
                if (ev.key == TK_HOME)
                    feed(&st, REPL_KEY_CHAR, 1, NULL);
                else if (ev.key == TK_END)
                    feed(&st, REPL_KEY_CHAR, 5, NULL);
                else if (ev.key == TK_DELETE) {
                    struct slot *s = &st.slots[st.focus];
                    if (s->repl.cursor < s->repl.len) {
                        feed(&st, REPL_KEY_RIGHT, 0, NULL);
                        feed(&st, REPL_KEY_BACKSPACE, 0, NULL);
                    }
                }
                else if ((size_t)ev.key < sizeof MAP / sizeof *MAP && MAP[ev.key])
                    feed(&st, MAP[ev.key], 0, NULL);
            }
            break;
        }
        chrome_paint();
    }
}
