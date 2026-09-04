#include "form.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chrome.h"
#include "frontend.h"
#include "md.h"
#include "replbox.h"
#include "tty.h"
#include "ui.h"
#include "viewport.h"

#define FORM_INDENT 2

/* what a block's content is indented by under its label */
#define FORM_CONTENT 2

/* a label wider than this keeps its own row rather than moving every value */
#define FORM_LABEL_MAX 12

/* the cells left of a value for the ‹ › a focused choice draws */
#define FORM_GUTTER 2

#define HIT_MAX 128

struct slot {
    struct replbox box;
    int            choice;
    int            rows;
};

struct line {
    int                   field;
    const char           *label; /* the block's label */
    unsigned char         inl;   /* label and value share this row */
    const struct md_text *note;
    size_t                from, len;
    const char           *plain; /* a row of the form's own, e.g. "+12 more" */
    int                   row;
    int                   indent;
    enum ui_role          role;
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
    int              top;
    int              pinned; /* the view is where the user scrolled it */
    int              rows, room;
    int              label_width;
    int              folds; /* the form has something collapsed to show */
    char             more[FORM_FIELDS][24];
    char             more_notes[24];

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
    return replbox_line(&st->slots[i].box);
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

static int field_shown(const struct state *st, int i)
{
    return st->form->fields[i].kind != FORM_TOGGLE || st->folds;
}

static void focus_step(struct state *st, int delta)
{
    int n = st->form->fields_n;
    if (n <= 0)
        return;
    st->pinned = 0;
    for (int step = 0; step < n; step++) {
        st->focus = (st->focus + delta) % n;
        if (st->focus < 0)
            st->focus += n;
        if (field_shown(st, st->focus))
            return;
    }
}

/* arrows stop at the first and last field rather than wrapping */
static void focus_move(struct state *st, int delta)
{
    int want = st->focus + delta;
    while (want >= 0 && want < st->form->fields_n && !field_shown(st, want))
        want += delta;
    if (want < 0 || want >= st->form->fields_n)
        return;
    st->pinned = 0;
    st->focus = want;
}

/* a toggle field turned on opens every collapsed block of the form */
static int expanded(const struct state *st)
{
    for (int i = 0; i < st->form->fields_n; i++)
        if (st->form->fields[i].kind == FORM_TOGGLE && st->slots[i].choice)
            return 1;
    return 0;
}

static int value_column(void)
{
    return FORM_INDENT + FORM_CONTENT;
}

/* where the value of a one-line block sits, beside its label: the widest
   label, then a space and the two cells a focused choice marks itself with */
static int inline_column(const struct state *st)
{
    return FORM_INDENT + st->label_width + 1 + FORM_GUTTER;
}

/* a form that labels its notes runs them down the value column; one that does
   not is prose, and prose starts at the left edge */
static int note_column(const struct state *st)
{
    return st->form->note_labels ? value_column() : FORM_INDENT;
}

#define REPL_GUTTER 2

static int inline_budget(const struct state *st, int columns)
{
    int budget = columns - inline_column(st) - 2;
    return budget < 8 ? 8 : budget;
}

/* a text field keeps the column it starts in, however many rows it takes: it
   wraps under itself beside its label rather than moving out from under it */
static void widths(struct state *st)
{
    int cols = inline_budget(st, ui_columns()) + REPL_GUTTER;
    for (int i = 0; i < st->form->fields_n; i++)
        replbox_width(&st->slots[i].box, cols);
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

static void open_up(struct state *st)
{
    for (int i = 0; i < st->form->fields_n; i++)
        if (st->form->fields[i].kind == FORM_TOGGLE)
            st->slots[i].choice = 1;
}

/* a field showing only the head of its text: it reads, it does not edit */
static int folded(struct state *st, int i)
{
    const struct form_field *f = &st->form->fields[i];
    if (f->kind != FORM_TEXT || f->rows_max <= 0 || expanded(st))
        return 0;
    return st->slots[i].rows >= f->rows_max &&
           replbox_wants(&st->slots[i].box) > f->rows_max;
}

static struct line *more_row(struct lines *out, const char *text)
{
    struct line *l = line_add(out);
    if (l) {
        l->field = -1;
        l->plain = text;
        l->role = UI_DIM;
    }
    return l;
}

static void wrap_notes(struct lines *out, const struct md_text *note, int budget,
                       enum ui_role role)
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
        l->role = role;
        at += got + skip;
        first = 0;
    }
}

static int is_blank(const struct line *l)
{
    return l->field < 0 && !l->label && !l->plain && !l->len;
}

static struct line *blank_row(struct lines *out)
{
    if (!out->n || is_blank(&out->v[out->n - 1]))
        return NULL;
    struct line *l = line_add(out);
    if (l)
        l->field = -1;
    return l;
}

static struct line *head_row(struct lines *out, const char *label, int field)
{
    struct line *l = line_add(out);
    if (l) {
        l->field = field;
        l->label = label;
        l->row = -1;
    }
    return l;
}

/* a block of one row wears its label beside it; only a long one is indented
   under a heading of its own */
static void fold_inline(struct lines *out, int head, int budget)
{
    if (out->n != head + 2)
        return;
    struct line *h = &out->v[head];
    struct line *body = &out->v[head + 1];
    if (body->field != h->field || body->plain || body->indent)
        return;
    if (body->note &&
        (int)ui_cells_n(md_text_plain(body->note, NULL) + body->from,
                        body->len) > budget)
        return;

    const char *label = h->label;
    *h = *body;
    h->label = label;
    h->inl = 1;
    h->row = body->note ? -1 : body->row;
    out->n--;

    /* one-line blocks run together: the blank was for a heading */
    if (head >= 2 && is_blank(&out->v[head - 1]) && out->v[head - 2].inl) {
        memmove(&out->v[head - 1], &out->v[head],
                (size_t)(out->n - head) * sizeof *out->v);
        out->n--;
    }
}

static int layout(struct state *st, int columns)
{
    const struct form *form = st->form;
    struct lines      *out = &st->lines;

    out->n = 0;

    int note_budget = columns - note_column(st) - 2;
    if (note_budget < 8)
        note_budget = 8;
    int inline_room = inline_budget(st, columns);

    int exp = expanded(st);
    int from = -1;

    st->folds = 0;

    for (int i = 0; i < form->notes_n; i++) {
        const char *label = form->note_labels ? form->note_labels[i] : NULL;
        int         head = -1;
        if (label) {
            blank_row(out);
            head = out->n;
            head_row(out, label, -1);
        }
        if (i == form->notes_from && form->notes_max > 0)
            from = out->n;
        wrap_notes(out, st->notes ? st->notes[i] : NULL, note_budget,
                   form->note_roles ? form->note_roles[i] : UI_DIM);
        /* only a block of its own folds onto one row: the notes that follow
           an unlabelled one belong to it, the log being the reason */
        int alone = i + 1 >= form->notes_n ||
                    (form->note_labels && form->note_labels[i + 1]);
        if (head >= 0 && alone) {
            int was = out->n;
            fold_inline(out, head, inline_room);
            if (from > head && out->n < was)
                from -= was - out->n;
        }
    }

    if (from >= 0 && out->n - from > form->notes_max)
        st->folds = 1;

    /* the oldest of the block goes, and a row says how much of it */
    int drop = !exp && from >= 0 ? out->n - from - form->notes_max : 0;
    if (drop > 0) {
        snprintf(st->more_notes, sizeof st->more_notes, "+%d earlier", drop);
        memmove(&out->v[from + 1], &out->v[from + drop],
                (size_t)(out->n - from - drop) * sizeof *out->v);
        out->n -= drop - 1;
        out->v[from] = (struct line){.field = -1,
                                     .plain = st->more_notes,
                                     .role = UI_DIM};
    }

    blank_row(out);

    for (int i = 0; i < form->fields_n; i++) {
        const struct form_field *f = &form->fields[i];

        if (f->kind == FORM_TOGGLE && !st->folds)
            continue;

        /* a button is its own label: it has no content under it */
        if (f->kind == FORM_BUTTON || f->kind == FORM_TOGGLE) {
            st->slots[i].rows = 1;
            blank_row(out);
            head_row(out, f->label, i);
            continue;
        }

        if (f->kind == FORM_CHOICE) {
            st->slots[i].rows = 1;
            struct line *l = line_add(out);
            if (l) {
                l->field = i;
                l->label = f->label;
                l->inl = 1;
            }
            continue;
        }

        int rows = replbox_wants(&st->slots[i].box);

        int shown = rows;
        if (f->rows_max > 0 && rows > f->rows_max) {
            st->folds = 1;
            if (!exp)
                shown = f->rows_max;
        }

        st->slots[i].rows = shown;

        struct line *l = line_add(out);
        if (l) {
            l->field = i;
            l->label = f->label;
            l->inl = 1;
        }
        for (int r = 1; r < shown; r++) {
            struct line *k = line_add(out);
            if (!k)
                break;
            k->field = i;
            k->row = r;
        }
        if (shown < rows) {
            snprintf(st->more[i], sizeof st->more[i], "+%d more", rows - shown);
            struct line *k = more_row(out, st->more[i]);
            if (k)
                k->indent = inline_column(st) - value_column();
        }
    }
    return out->n;
}

static void put_label(const char *label, int focused)
{
    ui_pad(FORM_INDENT);
    ui_esc(ui_style(focused ? UI_ACCENT : UI_DIM));
    ui_put(label);
    ui_esc(ui_style(UI_RESET));
}

static void put_choice_marker(int focused)
{
    if (!focused) {
        ui_pad(2);
        return;
    }
    ui_esc(ui_style(UI_ACCENT));
    ui_put("\xe2\x80\xb9 ");
    ui_esc(ui_style(UI_RESET));
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

static void put_value_row(struct state *st, int i, int row, int focused)
{
    if (!replbox_render(&st->slots[i].box, st->slots[i].rows))
        return;
    replbox_paint_row(&st->slots[i].box, row, REPL_GUTTER, focused);
}

static int room_for(void)
{
    int rows = chrome_modal_rows() - 2;
    return rows < 3 ? 3 : rows;
}

static void paint(void *ud)
{
    struct state *st = ud;
    struct form  *form = st->form;
    int           columns = ui_columns();

    widths(st);
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
        int first = -1, last = -1, caret = -1, body = -1;
        for (int i = 0; i < n; i++) {
            if (lines[i].field != st->focus)
                continue;
            if (first < 0)
                first = i;
            if (body < 0 && lines[i].row >= 0)
                body = i;
            last = i;
        }
        struct replbox *box = &st->slots[st->focus].box;
        if (body >= 0 && field_at(st, st->focus)->kind == FORM_TEXT &&
            replbox_render(box, st->slots[st->focus].rows) &&
            replbox_caret(box) >= 0)
            caret = body + replbox_caret(box);

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

    {
        char        said[256];
        const char *title = form->title ? form->title : "";
        if (n > room)
            snprintf(said, sizeof said, "%s \xc2\xb7 %d\xe2\x80\x93%d of %d",
                     title, st->top + 1, st->top + room, n);
        else
            snprintf(said, sizeof said, "%s", title);
        chrome_title_paint(said);
    }

    int end = n <= room ? n : st->top + room;
    for (int i = st->top; i < end; i++) {
        const struct line *l = &lines[i];
        int                at = base + 1 + i - st->top;

        if (l->field >= 0 && at >= 0 && at < HIT_MAX)
            st->hit[at] = (short)l->field;

        if (l->field < 0) {
            if (l->inl) {
                put_label(l->label, 0);
                int used = FORM_INDENT + (int)ui_cells(l->label);
                ui_pad(inline_column(st) > used ? inline_column(st) - used : 1);
                md_text_put(l->note, l->from, l->len, l->role);
            } else if (l->label)
                put_label(l->label, 0);
            else if (l->plain) {
                ui_pad(value_column() + l->indent);
                ui_esc(ui_style(l->role));
                ui_put(l->plain);
                ui_esc(ui_style(UI_RESET));
            } else if (l->len) {
                ui_pad(note_column(st) + l->indent);
                md_text_put(l->note, l->from, l->len, l->role);
            }
            ui_put("\n");
            continue;
        }

        struct form_field *f = field_at(st, l->field);
        int                focused = l->field == st->focus;

        if (l->inl) {
            const char *label = l->label ? l->label : "";
            put_label(label, focused);
            int used = FORM_INDENT + (int)ui_cells(label);
            int choice = f->kind == FORM_CHOICE;
            /* the marker of a focused choice stands in for the gap */
            int want = inline_column(st) - (choice ? 2 : 0);
            int gap = want - used;
            if (gap < !choice)
                gap = !choice;
            ui_pad(gap);
            if (choice) {
                put_choice_marker(focused);
                put_choice(st, l->field, focused);
            } else
                put_value_row(st, l->field, 0, focused);
            ui_put("\n");
            continue;
        }

        if (l->row < 0) {
            const char *said = f->label ? f->label : "";
            if (f->kind == FORM_TOGGLE && f->choices_n > 1)
                said = f->choices[st->slots[l->field].choice ? 1 : 0];
            put_label(said ? said : "", focused);
            ui_put("\n");
            continue;
        }

        ui_pad(inline_column(st));
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
    if (st->form->notes_n > 0) {
        st->notes = calloc((size_t)st->form->notes_n, sizeof *st->notes);
        for (int i = 0; st->notes && i < st->form->notes_n; i++)
            st->notes[i] = md_text_parse(st->form->notes[i]);
    }

    for (int i = 0; i < st->form->fields_n; i++) {
        struct form_field *f = field_at(st, i);
        struct slot       *s = &st->slots[i];

        replbox_init(&s->box, NULL, 0);

        if (f->kind == FORM_BUTTON)
            continue;
        if (f->kind == FORM_TOGGLE) {
            s->choice = f->value && f->value[0] ? 1 : 0;
            continue;
        }
        if (f->kind == FORM_CHOICE) {
            s->choice = 0;
            for (int c = 0; c < f->choices_n; c++)
                if (f->choices[c] && f->value && !strcmp(f->choices[c], f->value)) {
                    s->choice = c;
                    break;
                }
        } else if (f->value) {
            replbox_set_text(&s->box, f->value);
        }

        int cells = (int)ui_cells(f->label ? f->label : "");
        if (cells > st->label_width && cells <= FORM_LABEL_MAX &&
            f->kind != FORM_TOGGLE && f->kind != FORM_BUTTON)
            st->label_width = cells;
    }

    for (int i = 0; st->form->note_labels && i < st->form->notes_n; i++) {
        const char *label = st->form->note_labels[i];
        int         cells = (int)ui_cells(label ? label : "");
        if (cells > st->label_width && cells <= FORM_LABEL_MAX)
            st->label_width = cells;
    }
}

static void unload(struct state *st)
{
    for (int i = 0; i < st->form->fields_n; i++)
        replbox_free(&st->slots[i].box);
    for (int i = 0; st->notes && i < st->form->notes_n; i++)
        md_text_free(st->notes[i]);
    free(st->notes);
    st->notes = NULL;
    free(st->lines.v);
    memset(&st->lines, 0, sizeof st->lines);
}

static void store(struct state *st)
{
    for (int i = 0; i < st->form->fields_n; i++) {
        struct form_field *f = field_at(st, i);
        if (!f->value || !f->size || f->kind == FORM_BUTTON ||
            f->kind == FORM_TOGGLE)
            continue;
        snprintf(f->value, f->size, "%s", slot_shown(st, i));
    }
}

/* a toggle opens or closes the collapsed blocks and the form stays up */
static int press_toggle(struct state *st)
{
    if (field_at(st, st->focus)->kind != FORM_TOGGLE)
        return 0;
    struct slot *s = &st->slots[st->focus];
    s->choice = !s->choice;
    st->pinned = 0;
    return 1;
}

static int press_button(struct state *st)
{
    struct form_field *f = field_at(st, st->focus);
    if (f->kind != FORM_BUTTON)
        return 0;
    store(st);
    if (f->value && f->size)
        snprintf(f->value, f->size, "1");
    chrome_modal(NULL, NULL);
    unload(st);
    return 1;
}

static int feed(struct state *st, const tty_event *ev)
{
    if (folded(st, st->focus))
        open_up(st);

    widths(st);
    st->pinned = 0;
    return replbox_key(&st->slots[st->focus].box, ev);
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

/* the last field cannot be left, so the arrow scrolls the view instead: the
   notes sit above the first field and the wheel is otherwise the only way
   there */
static void step_or_scroll(struct state *st, int delta)
{
    int was = st->focus;
    focus_move(st, delta);
    if (st->focus == was)
        scroll_by(st, delta);
}

static void step_or_leave(struct state *st, int delta, const tty_event *ev)
{
    if (field_at(st, st->focus)->kind != FORM_TEXT || folded(st, st->focus)) {
        step_or_scroll(st, delta);
        return;
    }
    int was = replbox_repl(&st->slots[st->focus].box)->cursor;
    feed(st, ev);
    if (replbox_repl(&st->slots[st->focus].box)->cursor == was)
        step_or_scroll(st, delta);
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
        int                button = f->kind == FORM_BUTTON ||
                                    f->kind == FORM_TOGGLE;

        if (ev.key == TK_CHAR && ev.cp == 3) {
            chrome_modal(NULL, NULL);
            unload(&st);
            return 0;
        }

        switch (ev.key) {
        case TK_UP:
            step_or_leave(&st, -1, &ev);
            break;
        case TK_DOWN:
            step_or_leave(&st, 1, &ev);
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
            if (press_toggle(&st))
                break;
            if (press_button(&st))
                return 1;
            break;
        }

        case TK_ENTER:
            if (press_toggle(&st))
                break;
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
                    if (press_toggle(&st))
                        break;
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
            feed(&st, &ev);
            free(ev.text);
            break;
        }
        }
        chrome_paint();
    }
}
