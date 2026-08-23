#include "form.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chrome.h"
#include "frontend.h"
#include "text.h"
#include "tty.h"
#include "ui.h"

#define FORM_MAX    1024
#define FORM_FIELDS 12

// Everything is drawn at this indent, labels and values alike, so the form
// lines up with the lists it is opened from.
#define FORM_INDENT 2

struct slot {
    char   text[FORM_MAX];
    size_t len;
    size_t at;          /* the caret, as a byte offset into text */
    int    choice;      /* FORM_CHOICE: where in the list it sits */
};

struct state {
    struct form *form;
    struct slot  slots[FORM_FIELDS];
    int          focus;
    int          label_width;
    int          top;       /* the first laid-out row the window shows */
};

static int lead_byte(const char *s, size_t at)
{
    return ((unsigned char)s[at] & 0xC0) != 0x80;
}

static size_t step_left(const struct slot *s, size_t at)
{
    while (at > 0 && !lead_byte(s->text, --at))
        ;
    return at;
}

static size_t step_right(const struct slot *s, size_t at)
{
    while (at < s->len && !lead_byte(s->text, ++at))
        ;
    return at;
}

static void cut(struct slot *s, size_t from, size_t to)
{
    memmove(s->text + from, s->text + to, s->len - to);
    s->len -= to - from;
    s->text[s->len] = '\0';
    s->at = from;
}

static void insert(struct slot *s, const char *text, size_t n)
{
    if (!n || s->len + n >= sizeof s->text)
        return;
    memmove(s->text + s->at + n, s->text + s->at, s->len - s->at);
    memcpy(s->text + s->at, text, n);
    s->len += n;
    s->at += n;
    s->text[s->len] = '\0';
}

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
    return st->slots[i].text;
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

/* ---- drawing ---------------------------------------------------------- */

// One painted row. A field wide enough to wrap owns several of them, and only
// the first carries the label.
struct line {
    int         field;      /* -1 for a note or a blank row */
    const char *text;       /* the slice this row shows */
    size_t      len;
    size_t      caret;      /* byte offset into the slice, or NO_CARET */
};

#define NO_CARET ((size_t)-1)
#define LINES_MAX 512

static int lead(const char *s, size_t at)
{
    return ((unsigned char)s[at] & 0xC0) != 0x80;
}

// Where a value starts, which is also what a wrapped row is indented to.
static int value_column(const struct state *st)
{
    return FORM_INDENT + st->label_width + 4;
}

static int value_budget(const struct state *st, int columns)
{
    int budget = columns - value_column(st) - 2;
    return budget < 4 ? 4 : budget;
}

static int wrap_into(struct line *out, int n, int max, int field,
                     const char *text, int budget)
{
    size_t rest = text ? strlen(text) : 0;
    if (!rest) {
        if (n < max)
            out[n++] = (struct line){field, text ? text : "", 0, NO_CARET};
        return n;
    }
    while (rest && n < max) {
        size_t skip = 0;
        size_t got = ui_wrap_row(text, rest, (size_t)budget, &skip, NULL);
        out[n++] = (struct line){field, text, got, NO_CARET};
        text += got + skip;
        rest -= got + skip;
    }
    return n;
}

// Every row the form would paint, in order. The window over them is chosen
// afterwards, so growing and scrolling are the same measurement.
static int layout(const struct state *st, int columns, struct line *out, int max)
{
    const struct form *form = st->form;
    int                n = 0;

    int note_budget = columns - FORM_INDENT - 2;
    if (note_budget < 8)
        note_budget = 8;

    for (int i = 0; i < form->notes_n; i++)
        n = wrap_into(out, n, max, -1, form->notes[i] ? form->notes[i] : "",
                      note_budget);
    if (form->notes_n && n < max)
        out[n++] = (struct line){-1, "", 0, NO_CARET};

    int budget = value_budget(st, columns);

    for (int i = 0; i < form->fields_n && n < max; i++) {
        int at = n;

        if (form->fields[i].kind == FORM_CHOICE) {
            out[n++] = (struct line){i, NULL, 0, NO_CARET};
            continue;
        }

        const struct slot *s = &st->slots[i];
        n = wrap_into(out, n, max, i, s->text, budget);

        if (i != st->focus)
            continue;

        // The row the caret falls on is the last one starting at or before it,
        // which puts a caret sitting on a wrap onto the row it wrapped out of.
        for (int r = n - 1; r >= at; r--) {
            size_t from = (size_t)(out[r].text - s->text);
            if (from > s->at)
                continue;
            out[r].caret = s->at - from > out[r].len ? out[r].len : s->at - from;
            break;
        }
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

// Two cells before every value, whatever kind it is, so the values line up in
// one column. A choice in hand spends them on the arrow that says it is one.
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

// The terminal's own caret sits with the prompt below, so this one is drawn
// rather than moved.
static void put_slice(const struct line *l)
{
    ui_esc(ui_style(UI_TEXT));
    if (l->caret == NO_CARET) {
        ui_putn(l->text, l->len);
        ui_esc(ui_style(UI_RESET));
        return;
    }

    size_t at = l->caret, next = at;
    if (next < l->len)
        while (next < l->len && !lead(l->text, ++next))
            ;

    ui_putn(l->text, at);
    ui_esc("\x1b[7m");
    if (next > at)
        ui_putn(l->text + at, next - at);
    else
        ui_put(" ");
    ui_esc("\x1b[27m");
    if (next < l->len)
        ui_putn(l->text + next, l->len - next);
    ui_esc(ui_style(UI_RESET));
}

// What the window may show, once the title above and the hint below are out.
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
    int                n = layout(st, columns, lines, LINES_MAX);
    int                room = room_for();

    // Short of the room it has, the form is drawn whole and nothing scrolls.
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
            if (lines[i].caret != NO_CARET)
                caret = i;
        }
        if (first >= 0) {
            if (first < st->top)
                st->top = first;
            if (last >= st->top + room)
                st->top = last - room + 1;
        }
        // A field taller than the window keeps the caret rather than the label.
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
        int                heads = i == st->top || lines[i - 1].field != l->field;

        if (heads) {
            put_label(st, f->label ? f->label : "", focused);
            put_gutter(st, l->field, focused);
        } else {
            ui_pad(value_column(st));
        }

        if (f->kind == FORM_CHOICE)
            put_choice(st, l->field, focused);
        else
            put_slice(l);
        ui_put("\n");
    }

    ui_put("\n");
    ui_esc(ui_style(UI_DIM));
    ui_pad(FORM_INDENT);
    ui_put("tab to move, enter to keep, esc to leave it alone");
    ui_esc(ui_style(UI_RESET));
}

/* ---- the loop --------------------------------------------------------- */

static void load(struct state *st)
{
    for (int i = 0; i < st->form->fields_n; i++) {
        struct form_field *f = field_at(st, i);
        struct slot       *s = &st->slots[i];

        snprintf(s->text, sizeof s->text, "%s", f->value ? f->value : "");
        s->len = s->at = strlen(s->text);

        if (f->kind == FORM_CHOICE) {
            s->choice = 0;
            for (int c = 0; c < f->choices_n; c++)
                if (f->choices[c] && !strcmp(f->choices[c], s->text)) {
                    s->choice = c;
                    break;
                }
        }

        int cells = (int)ui_cells(f->label ? f->label : "");
        if (cells > st->label_width)
            st->label_width = cells;
    }
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
            return 0;
        }

        struct slot       *s = &st.slots[st.focus];
        struct form_field *f = field_at(&st, st.focus);
        int                typing = f->kind == FORM_TEXT;

        switch (ev.key) {
        case TK_TEXT:
            // A paste is one row: newlines would break out of the field.
            for (char *p = ev.text; p && *p; p++)
                if (*p == '\n' || *p == '\r')
                    *p = ' ';
            if (typing)
                insert(s, ev.text, ev.text ? strlen(ev.text) : 0);
            free(ev.text);
            break;
        case TK_CHAR:
            if (ev.cp == 3 || ev.cp == 4) {
                chrome_modal(NULL, NULL);
                return 0;
            }
            if (!typing) {
                // A choice takes the space bar as "the next one", so the
                // field can be walked without reaching for the arrows.
                if (ev.cp == ' ')
                    cycle(&st, st.focus, 1);
                break;
            }
            if (ev.cp == 21) {          /* ctrl-u */
                cut(s, 0, s->at);
            } else if (ev.cp == 23) {   /* ctrl-w */
                size_t to = s->at;
                while (s->at && s->text[s->at - 1] == ' ')
                    s->at--;
                while (s->at && s->text[s->at - 1] != ' ')
                    s->at--;
                cut(s, s->at, to);
            } else if (ev.cp >= 0x20) {
                char buf[4];
                insert(s, buf, text_utf8_encode(ev.cp, buf));
            }
            break;
        case TK_BACKSPACE:
            if (typing && s->at)
                cut(s, step_left(s, s->at), s->at);
            break;
        case TK_DELETE:
            if (typing && s->at < s->len)
                cut(s, s->at, step_right(s, s->at));
            break;
        case TK_LEFT:
            if (typing)
                s->at = step_left(s, s->at);
            else
                cycle(&st, st.focus, -1);
            break;
        case TK_RIGHT:
            if (typing)
                s->at = step_right(s, s->at);
            else
                cycle(&st, st.focus, 1);
            break;
        case TK_HOME:
            if (typing)
                s->at = 0;
            break;
        case TK_END:
            if (typing)
                s->at = s->len;
            break;
        case TK_TAB:
        case TK_DOWN:
            focus_step(&st, 1);
            break;
        case TK_UP:
            focus_step(&st, -1);
            break;
        case TK_ENTER:
            store(&st);
            chrome_modal(NULL, NULL);
            return 1;
        case TK_ESCAPE:
        case TK_EOF:
            chrome_modal(NULL, NULL);
            return 0;
        default:
            break;
        }
        chrome_paint();
    }
}
